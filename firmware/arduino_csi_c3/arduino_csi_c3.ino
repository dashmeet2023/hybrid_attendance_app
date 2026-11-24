/*
  Hybrid Attendance - CSI room-presence sensor, ESP32-C3 (Arduino IDE)
  Arduino alternative to Espressif's esp-csi "csi_recv_router" (no ESP-IDF needed).

  Runs two Wi-Fi interfaces at once (AP + STA):
    STA  joins the classroom router and pings it ~100x/s; the CSI of every reply is measured.
         Keeps retrying in the background - the board never freezes waiting for Wi-Fi.
    AP   hosts its own network "HybridCSI-Admin". Connect a phone/laptop to it and open
         http://192.168.4.1  (login admin / admin12345) for the admin dashboard:
         router link, live CSI rate + movement graph, per-minute history, event log, CSV download.
         Note: the AP follows the router's channel once connected (single radio).

  USB serial output (switch any time by typing in the Serial Monitor, New Line):
    SUMMARY  one readable line per second (testing)
    RAW      CSI_DATA lines in the exact csi_recv_router format, for the Raspberry Pi pipeline
  Set START_MODE to MODE_RAW for the Pi. Event log lines start with "LOG" (the Pi ignores them).

  Arduino IDE: Board "ESP32C3 Dev Module" (or Nologo ESP32C3 Super Mini), USB CDC On Boot = Enabled.
  Router: 2.4 GHz, fixed channel (e.g. 6), 20 MHz, WPA2. On the Pi this board is /dev/ttyACM0.
*/
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include "esp_wifi.h"
#include "ping/ping_sock.h"
#include "freertos/queue.h"

// ---------------- settings ----------------
const char *WIFI_SSID = "ATTENDANCE_WIFI";     // classroom router
const char *WIFI_PASS = "your-wifi-password";
const char *AP_SSID   = "HybridCSI-Admin";     // network hosted by this board
const char *AP_PASS   = "admin12345";          // at least 8 characters
const char *WEB_USER  = "admin";               // dashboard login
const char *WEB_PASS  = "admin12345";
#define MODE_SUMMARY 0
#define MODE_RAW     1
#define START_MODE   MODE_SUMMARY              // MODE_RAW for the Raspberry Pi
#define PING_INTERVAL_MS 10                    // 100 packets per second
#define TZ_OFFSET_S  19800                     // IST (UTC+5:30), for log timestamps
#define MOTION_THRESHOLD 8.0                   // dashboard "movement" hint only; the Pi model decides presence
// ------------------------------------------

#define LED_PIN 8                              // Super Mini on-board LED, active LOW
#define MAX_CSI_LEN 128                        // 64 subcarriers x (imag, real)
#define SPARK_SECONDS 60
#define HISTORY_MIN 120
#define LOG_LINES 40

struct CsiPacket {
  int8_t rssi, noise_floor;
  uint8_t rate, sig_mode, mcs, cwb, channel, secondary_channel;
  uint16_t sig_len;
  uint32_t timestamp;
  uint8_t mac[6];
  uint16_t len;
  bool first_word_invalid;
  int8_t buf[MAX_CSI_LEN];
};

struct SecondStat { uint16_t pkts; float rssi, motion; };
struct MinuteStat { char when[20]; uint32_t pkts; float rssi, motion, motionMax; };

static QueueHandle_t csiQueue;
static uint8_t routerMac[6];
static volatile uint32_t dropped = 0;
static int mode = START_MODE;
static uint32_t seq = 0, totalPackets = 0;
static String rxLine;

static WebServer web(80);
static bool staConnected = false, csiOk = false;
static esp_ping_handle_t pingHandle = nullptr;
static unsigned long lastConnAttempt = 0, lastBlink = 0;

static SecondStat spark[SPARK_SECONDS];
static int sparkHead = 0, sparkCount = 0;
static MinuteStat hist[HISTORY_MIN];
static int histHead = 0, histCount = 0;
static String logLines[LOG_LINES];
static int logHead = 0, logCount = 0;

// ---------- time and event log ----------

static String nowStr() {
  struct tm t;
  char buf[24];
  if (getLocalTime(&t, 0) && t.tm_year > 120) {
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
  } else {
    unsigned long s = millis() / 1000;
    snprintf(buf, sizeof(buf), "up %02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  }
  return String(buf);
}

static void logEvent(const String &msg) {
  String line = nowStr() + "  " + msg;
  logLines[logHead] = line;
  logHead = (logHead + 1) % LOG_LINES;
  if (logCount < LOG_LINES) logCount++;
  Serial.println("LOG " + line);
}

// ---------- CSI capture ----------

// Runs in the Wi-Fi task: keep it short, copy the packet and return.
static void csiCallback(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf || memcmp(info->mac, routerMac, 6) != 0) return;   // only the router's replies
  CsiPacket p;
  const wifi_pkt_rx_ctrl_t &rx = info->rx_ctrl;
  p.rssi = rx.rssi; p.noise_floor = rx.noise_floor; p.rate = rx.rate; p.sig_mode = rx.sig_mode;
  p.mcs = rx.mcs; p.cwb = rx.cwb; p.channel = rx.channel; p.secondary_channel = rx.secondary_channel;
  p.sig_len = rx.sig_len; p.timestamp = rx.timestamp;
  memcpy(p.mac, info->mac, 6);
  p.len = info->len > MAX_CSI_LEN ? MAX_CSI_LEN : info->len;
  p.first_word_invalid = info->first_word_invalid;
  memcpy(p.buf, info->buf, p.len);
  if (xQueueSend(csiQueue, &p, 0) != pdTRUE) dropped++;
}

static bool startCsi() {
  wifi_csi_config_t cfg = {};
  cfg.lltf_en = true;            // 64-subcarrier legacy LTF, as used by the pipeline
  cfg.htltf_en = false;
  cfg.stbc_htltf2_en = false;
  cfg.ltf_merge_en = true;
  cfg.channel_filter_en = true;
  cfg.manu_scale = true;
  cfg.shift = false;
  esp_err_t e1 = esp_wifi_set_csi_config(&cfg);
  esp_err_t e2 = esp_wifi_set_csi_rx_cb(csiCallback, nullptr);
  esp_err_t e3 = esp_wifi_set_csi(true);
  if (e1 || e2 || e3) {
    logEvent("CSI enable FAILED (config=" + String(e1) + " cb=" + String(e2) + " enable=" + String(e3) +
             "). This Arduino core may be built without CSI.");
    return false;
  }
  logEvent("CSI enabled");
  return true;
}

static void stopPing() {
  if (!pingHandle) return;
  esp_ping_stop(pingHandle);
  esp_ping_delete_session(pingHandle);
  pingHandle = nullptr;
}

static void startPing() {
  stopPing();
  IPAddress gw = WiFi.gatewayIP();
  esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
  IP_ADDR4(&pc.target_addr, gw[0], gw[1], gw[2], gw[3]);
  pc.count = ESP_PING_COUNT_INFINITE;
  pc.interval_ms = PING_INTERVAL_MS;
  pc.timeout_ms = 1000;
  pc.data_size = 1;
  pc.task_stack_size = 3072;
  esp_ping_callbacks_t cbs = {};
  if (esp_ping_new_session(&pc, &cbs, &pingHandle) == ESP_OK) {
    esp_ping_start(pingHandle);
    logEvent("Pinging router " + gw.toString() + " every " + String(PING_INTERVAL_MS) + " ms");
  } else {
    pingHandle = nullptr;
    logEvent("Could not start ping session");
  }
}

// ---------- Wi-Fi (never blocks) ----------

static void checkStation() {
  bool now = WiFi.status() == WL_CONNECTED;
  if (now && !staConnected) {
    staConnected = true;
    memcpy(routerMac, WiFi.BSSID(), 6);
    logEvent("Connected to " + String(WIFI_SSID) + " (" + WiFi.BSSIDstr() + "), IP " + WiFi.localIP().toString() +
             ", channel " + String(WiFi.channel()) + ", RSSI " + String(WiFi.RSSI()) + " dBm");
    configTime(TZ_OFFSET_S, 0, "pool.ntp.org", "time.google.com");
    startPing();
    digitalWrite(LED_PIN, LOW);            // LED steady on = connected
  } else if (!now && staConnected) {
    staConnected = false;
    memset(routerMac, 0, 6);
    stopPing();
    logEvent("Lost connection to " + String(WIFI_SSID) + " - retrying");
    lastConnAttempt = millis();
  }
  if (!now && millis() - lastConnAttempt > 15000) {   // retry every 15 s
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    lastConnAttempt = millis();
  }
  if (!now && millis() - lastBlink > 500) {           // LED blinking = not connected
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    lastBlink = millis();
  }
}

// ---------- statistics (dashboard + SUMMARY) ----------

static float prevAmp[64];
static bool havePrev = false;
static uint32_t secCount = 0;
static double secRssi = 0, secMotion = 0;
static uint16_t secLen = 0;
static unsigned long secStart = 0;
static uint32_t minPkts = 0, minSeconds = 0;
static double minRssi = 0, minMotion = 0, minMotionMax = 0;
static SecondStat lastSecond = {0, 0, 0};

static void addStats(const CsiPacket &p) {
  int n = p.len / 2;
  if (n > 64) n = 64;
  float amp[64];
  double mean = 0;
  for (int i = 0; i < n; i++) {
    float im = p.buf[2 * i], re = p.buf[2 * i + 1];
    amp[i] = sqrtf(im * im + re * re);
    mean += amp[i];
  }
  mean = n ? mean / n : 0;
  if (havePrev && mean > 0) {
    double diff = 0;
    for (int i = 0; i < n; i++) diff += fabs(amp[i] - prevAmp[i]);
    secMotion += diff / n / mean * 100;   // relative frame-to-frame change, %
  }
  memcpy(prevAmp, amp, sizeof(float) * n);
  havePrev = true;
  secCount++; secRssi += p.rssi; secLen = p.len;
}

static void tickSecond() {
  if (millis() - secStart < 1000) return;
  SecondStat s;
  s.pkts = secCount;
  s.rssi = secCount ? secRssi / secCount : 0;
  s.motion = secCount ? secMotion / secCount : 0;
  lastSecond = s;
  spark[sparkHead] = s;
  sparkHead = (sparkHead + 1) % SPARK_SECONDS;
  if (sparkCount < SPARK_SECONDS) sparkCount++;

  if (mode == MODE_SUMMARY) {
    if (secCount)
      Serial.printf("CSI %3u pkt/s | %u subcarriers | RSSI %5.1f dBm | motion %5.2f %s | dropped %lu\n",
                    s.pkts, secLen / 2, s.rssi, s.motion, s.motion > MOTION_THRESHOLD ? "<-- movement" : "",
                    (unsigned long)dropped);
    else
      Serial.printf("CSI   0 pkt/s - %s\n", staConnected ? "connected but no CSI (router channel/2.4 GHz?)"
                                                         : "not connected to the router yet");
  }

  minPkts += s.pkts; minSeconds++;
  if (s.pkts) { minRssi += s.rssi * s.pkts; minMotion += s.motion * s.pkts; }
  if (s.motion > minMotionMax) minMotionMax = s.motion;
  if (minSeconds >= 60) {
    MinuteStat &m = hist[histHead];
    String when = nowStr();
    strncpy(m.when, when.c_str(), sizeof(m.when) - 1);
    m.when[sizeof(m.when) - 1] = 0;
    m.pkts = minPkts;
    m.rssi = minPkts ? minRssi / minPkts : 0;
    m.motion = minPkts ? minMotion / minPkts : 0;
    m.motionMax = minMotionMax;
    histHead = (histHead + 1) % HISTORY_MIN;
    if (histCount < HISTORY_MIN) histCount++;
    minPkts = minSeconds = 0; minRssi = minMotion = minMotionMax = 0;
  }
  secCount = 0; secRssi = secMotion = 0; secStart = millis();
}

static void printRaw(const CsiPacket &p) {
  // Same 25 columns as csi_recv_router (ESP32/S2/S3/C3 layout).
  char head[200];
  snprintf(head, sizeof(head),
           "CSI_DATA,%lu,%02x:%02x:%02x:%02x:%02x:%02x,%d,%d,%d,%d,%d,0,0,0,0,0,0,%d,0,%d,%d,%lu,0,%d,0,%d,%d,\"[",
           (unsigned long)seq++, p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5],
           p.rssi, p.rate, p.sig_mode, p.mcs, p.cwb, p.noise_floor, p.channel, p.secondary_channel,
           (unsigned long)p.timestamp, p.sig_len, p.len, p.first_word_invalid ? 1 : 0);
  String line = head;
  for (int i = 0; i < p.len; i++) {
    if (i) line += ',';
    line += String((int)p.buf[i]);
  }
  line += "]\"";
  Serial.println(line);
}

// ---------- admin website ----------

static const char INDEX_HTML[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>CSI sensor admin</title>
<style>
body{margin:0;font-family:system-ui,sans-serif;background:#0f1a19;color:#eef5f3}
header{padding:12px 14px;background:#172524;font-weight:700}
main{padding:10px;display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr))}
.card{background:#172524;border-radius:12px;padding:12px}
.card h2{font-size:14px;margin:0 0 8px;color:#8fa7a3;font-weight:600}
.big{font-size:28px;font-weight:800}.ok{color:#3ecf8e}.bad{color:#ff6b57}.warn{color:#f2b33d}
table{width:100%;border-collapse:collapse;font-size:12px}td,th{padding:4px;border-bottom:1px solid #24403c;text-align:left}
pre{font-size:11px;white-space:pre-wrap;margin:0;max-height:260px;overflow:auto}
canvas{width:100%;height:110px;background:#0f1a19;border-radius:8px}
a{color:#7fb3ff}.wide{grid-column:1/-1}
</style></head><body>
<header>Hybrid Attendance &middot; CSI sensor admin <span id="clock" style="float:right;font-weight:400"></span></header>
<main>
<div class="card"><h2>Router link</h2><div class="big" id="sta">-</div><div id="stainfo"></div></div>
<div class="card"><h2>CSI now</h2><div class="big" id="pps">-</div><div id="csiinfo"></div></div>
<div class="card"><h2>Admin network</h2><div id="ap"></div></div>
<div class="card wide"><h2>Movement, last 60 s (relative CSI change %)</h2><canvas id="spark" width="600" height="110"></canvas></div>
<div class="card wide"><h2>Per-minute history <a href="/log.csv" style="float:right">Download CSV</a></h2>
<table><thead><tr><th>Time</th><th>Packets</th><th>RSSI</th><th>Motion avg</th><th>Motion max</th></tr></thead><tbody id="hist"></tbody></table></div>
<div class="card wide"><h2>Event log</h2><pre id="log"></pre></div>
</main>
<script>
const $=id=>document.getElementById(id);
function draw(sp,th){const c=$('spark'),x=c.getContext('2d');x.clearRect(0,0,c.width,c.height);
 const max=Math.max(th*2,...sp.map(s=>s.m));const w=c.width/60;
 x.strokeStyle='#f2b33d';x.setLineDash([4,4]);const ty=c.height-th/max*c.height;x.beginPath();x.moveTo(0,ty);x.lineTo(c.width,ty);x.stroke();x.setLineDash([]);
 sp.forEach((s,i)=>{const h=s.m/max*c.height;x.fillStyle=s.m>th?'#ff6b57':'#3ecf8e';x.fillRect(i*w+1,c.height-h,w-2,h);});}
async function poll(){try{const r=await fetch('/api/status',{cache:'no-store'});const d=await r.json();
 $('clock').textContent=d.time;
 $('sta').innerHTML=d.sta.connected?'<span class="ok">Connected</span>':'<span class="bad">Not connected</span>';
 $('stainfo').textContent=d.sta.connected?`${d.sta.ssid} · IP ${d.sta.ip} · ch ${d.sta.channel} · RSSI ${d.sta.rssi} dBm`:`Trying ${d.sta.ssid} every 15 s`;
 const pps=d.csi.pps;$('pps').innerHTML=`<span class="${pps>50?'ok':pps>0?'warn':'bad'}">${pps} pkt/s</span>`;
 $('csiinfo').textContent=`motion ${d.csi.motion.toFixed(2)}% ${d.csi.motion>d.threshold?'(movement)':'(still)'} · total ${d.csi.total} · dropped ${d.csi.dropped} · CSI ${d.csi.enabled?'on':'FAILED'} · serial ${d.mode}`;
 $('ap').textContent=`${d.ap.ssid} · http://${d.ap.ip} · ${d.ap.clients} client(s) · uptime ${d.uptime}`;
 draw(d.spark,d.threshold);
 $('hist').innerHTML=d.history.map(m=>`<tr><td>${m.t}</td><td>${m.p}</td><td>${m.r.toFixed(1)}</td><td>${m.m.toFixed(2)}</td><td>${m.x.toFixed(2)}</td></tr>`).join('')||'<tr><td colspan=5>First entry after one minute</td></tr>';
 $('log').textContent=d.log.join('\n');
}catch(e){$('sta').textContent='Dashboard offline';}}
setInterval(poll,2000);poll();
</script></body></html>)HTML";

static bool authed() {
  if (!web.authenticate(WEB_USER, WEB_PASS)) {
    web.requestAuthentication();
    return false;
  }
  return true;
}

static String jsonEscape(const String &s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

static void handleStatus() {
  if (!authed()) return;
  unsigned long s = millis() / 1000;
  char up[24];
  snprintf(up, sizeof(up), "%lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
  String j;
  j.reserve(9000);
  j += "{\"time\":\"" + nowStr() + "\",\"uptime\":\"" + String(up) + "\",\"mode\":\"" + String(mode == MODE_RAW ? "RAW" : "SUMMARY") + "\"";
  j += ",\"threshold\":" + String(MOTION_THRESHOLD, 1);
  j += ",\"sta\":{\"connected\":" + String(staConnected ? "true" : "false") + ",\"ssid\":\"" + jsonEscape(WIFI_SSID) + "\"";
  if (staConnected)
    j += ",\"ip\":\"" + WiFi.localIP().toString() + "\",\"channel\":" + String(WiFi.channel()) + ",\"rssi\":" + String(WiFi.RSSI());
  j += "},\"ap\":{\"ssid\":\"" + jsonEscape(AP_SSID) + "\",\"ip\":\"" + WiFi.softAPIP().toString() + "\",\"clients\":" + String(WiFi.softAPgetStationNum()) + "}";
  j += ",\"csi\":{\"enabled\":" + String(csiOk ? "true" : "false") + ",\"pps\":" + String(lastSecond.pkts) +
       ",\"motion\":" + String(lastSecond.motion, 2) + ",\"total\":" + String(totalPackets) + ",\"dropped\":" + String(dropped) + "}";
  j += ",\"spark\":[";
  for (int i = 0; i < sparkCount; i++) {
    const SecondStat &x = spark[(sparkHead - sparkCount + i + SPARK_SECONDS) % SPARK_SECONDS];
    if (i) j += ',';
    j += "{\"p\":" + String(x.pkts) + ",\"m\":" + String(x.motion, 2) + "}";
  }
  j += "],\"history\":[";
  int shown = histCount < 30 ? histCount : 30;                 // newest first
  for (int i = 0; i < shown; i++) {
    const MinuteStat &m = hist[(histHead - 1 - i + HISTORY_MIN) % HISTORY_MIN];
    if (i) j += ',';
    j += "{\"t\":\"" + String(m.when) + "\",\"p\":" + String(m.pkts) + ",\"r\":" + String(m.rssi, 1) +
         ",\"m\":" + String(m.motion, 2) + ",\"x\":" + String(m.motionMax, 2) + "}";
  }
  j += "],\"log\":[";
  for (int i = 0; i < logCount; i++) {                         // newest first
    if (i) j += ',';
    j += "\"" + jsonEscape(logLines[(logHead - 1 - i + LOG_LINES) % LOG_LINES]) + "\"";
  }
  j += "]}";
  web.send(200, "application/json", j);
}

static void handleCsv() {
  if (!authed()) return;
  String c = "minute,packets,rssi_dbm,motion_avg_pct,motion_max_pct\n";
  for (int i = 0; i < histCount; i++) {                        // oldest first
    const MinuteStat &m = hist[(histHead - histCount + i + HISTORY_MIN) % HISTORY_MIN];
    c += String(m.when) + "," + String(m.pkts) + "," + String(m.rssi, 1) + "," + String(m.motion, 2) + "," + String(m.motionMax, 2) + "\n";
  }
  c += "\nevent_log\n";
  for (int i = 0; i < logCount; i++) c += "\"" + logLines[(logHead - logCount + i + LOG_LINES) % LOG_LINES] + "\"\n";
  web.sendHeader("Content-Disposition", "attachment; filename=csi_log.csv");
  web.send(200, "text/csv", c);
}

// ---------- commands from the USB serial ----------

static void handleCommand(String cmd) {
  cmd.trim(); cmd.toUpperCase();
  if (cmd == "RAW")          { mode = MODE_RAW; }
  else if (cmd == "SUMMARY") { mode = MODE_SUMMARY; Serial.println("Mode: SUMMARY"); }
  else if (cmd.length())     Serial.println("Commands: SUMMARY, RAW");
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  Serial.begin(921600);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);          // never stall when no computer is reading the USB port
#endif
  delay(1500);
  Serial.println("\n=== ESP32-C3 CSI sensor (Arduino) ===");
  csiQueue = xQueueCreate(32, sizeof(CsiPacket));

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);              // steady packet rate
  // ESP32-C3 Super Mini boards have a poor antenna match: at full power many never finish
  // connecting ("Connecting......" forever). 8.5 dBm is the widely used fix.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.softAP(AP_SSID, AP_PASS);
  logEvent("Admin network " + String(AP_SSID) + " up: open http://" + WiFi.softAPIP().toString());

  csiOk = startCsi();
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastConnAttempt = millis();
  logEvent("Connecting to " + String(WIFI_SSID) + " ...");

  web.on("/", []() { if (authed()) web.send(200, "text/html", INDEX_HTML); });
  web.on("/api/status", handleStatus);
  web.on("/log.csv", handleCsv);
  web.onNotFound([]() { web.send(404, "text/plain", "Not found"); });
  web.begin();

  Serial.println(mode == MODE_RAW ? "Mode: RAW (CSI_DATA lines)" : "Mode: SUMMARY (type RAW for CSI_DATA lines)");
  secStart = millis();
}

void loop() {
  web.handleClient();
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { handleCommand(rxLine); rxLine = ""; }
    else if (rxLine.length() < 16) rxLine += c;
  }
  checkStation();
  CsiPacket p;
  while (xQueueReceive(csiQueue, &p, 0) == pdTRUE) {
    totalPackets++;
    addStats(p);
    if (mode == MODE_RAW) printRaw(p);
  }
  tickSecond();
  delay(1);
}
