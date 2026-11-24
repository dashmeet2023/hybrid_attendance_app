/*
  Hybrid Attendance - classroom NFC reader
  ESP32-C3 Super Mini  <->  PN532 (HW-147C, I2C mode)  <->  Raspberry Pi (USB serial)

  The Pi (nfc/pi_nfc_reader.py) sends each card UID to the attendance server and
  replies with the server's decision, which this board shows on its LED.

  PN532 DIP switch: 1 = ON, 2 = OFF  (I2C mode)

  Wiring (PN532 -> ESP32-C3 Super Mini):
    VCC -> 3.3
    GND -> G
    SDA -> GPIO6
    SCL -> GPIO7
    IRQ -> GPIO3
    RST -> GPIO4
  ESP32-C3 USB-C -> any USB port on the Raspberry Pi  (shows up as /dev/ttyACM0)

  Arduino IDE settings:
    Board: "ESP32C3 Dev Module" (or "Nologo ESP32C3 Super Mini")
    USB CDC On Boot: Enabled      <-- required, otherwise nothing reaches the Pi
    Library: "Adafruit PN532" (also installs Adafruit BusIO)

  Messages to the Pi (one JSON object per line):
    {"event":"ready","fw":"1.1","pn532":"1.6"}
    {"event":"card","uid":"04A1B2C3D4E5F6","len":7}
    {"event":"pong"} / {"event":"info",...} / {"event":"ok","cmd":"START"}
    {"event":"error","msg":"..."}

  Commands from the Pi (one per line):
    PING            -> {"event":"pong"}
    INFO            -> firmware + PN532 chip version + scanning state
    START / STOP    -> start / stop sending card reads (scanning is on at boot)
    BLINK           -> 3 short blinks (kept for older Pi scripts)
    RESULT <state>  -> show the server's decision on the LED:
                       VERIFIED      1 long flash (1 s)      = attendance marked
                       PENDING       2 slow blinks           = room check not ready, tap again
                       UNKNOWN_CARD  3 medium blinks         = card not assigned yet
                       REJECTED      6 fast blinks           = not accepted
                       OFFLINE/ERROR 10 very fast blinks     = Pi could not reach the server

  Changes from 1.0:
    - RESULT command so students see whether the tap was accepted, not only a read flash
    - PN532 health check every 5 s; re-initialised automatically after an I2C glitch
    - commands from the Pi are answered while waiting for the PN532 at boot
*/

#include <Wire.h>
#include <Adafruit_PN532.h>

#define FW_VERSION   "1.1"

#define PN532_SDA    6
#define PN532_SCL    7
#define PN532_IRQ    3
#define PN532_RST    4
#define LED_PIN      8      // on-board blue LED, active LOW

#define SAME_CARD_COOLDOWN_MS 2000   // ignore the same card held on the reader
#define HEALTH_CHECK_MS       5000   // how often to confirm the PN532 still answers

Adafruit_PN532 nfc(PN532_IRQ, PN532_RST, &Wire);

bool scanning = true;
bool pn532Ok = false;
uint32_t pn532Version = 0;

String lastUid = "";
unsigned long lastUidTime = 0;
unsigned long lastHealthCheck = 0;

String rxLine = "";

// ---------- helpers ----------

void ledOn()  { digitalWrite(LED_PIN, LOW); }
void ledOff() { digitalWrite(LED_PIN, HIGH); }

void blink(int times, int ms) {
  for (int i = 0; i < times; i++) {
    ledOn();  delay(ms);
    ledOff(); delay(ms);
  }
}

String uidToHex(const uint8_t *uid, uint8_t len) {
  String s = "";
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void sendJson(const String &json) {
  Serial.println(json);
}

void sendOk(const String &cmd) {
  sendJson("{\"event\":\"ok\",\"cmd\":\"" + cmd + "\"}");
}

void sendError(const String &msg) {
  sendJson("{\"event\":\"error\",\"msg\":\"" + msg + "\"}");
}

String pn532VersionString() {
  if (!pn532Version) return "none";
  return String((pn532Version >> 16) & 0xFF) + "." + String((pn532Version >> 8) & 0xFF);
}

// ---------- PN532 setup ----------

bool initPN532() {
  nfc.begin();
  pn532Version = nfc.getFirmwareVersion();
  if (!pn532Version) return false;
  nfc.SAMConfig();                       // normal mode, ready to read cards
  nfc.setPassiveActivationRetries(0x01); // don't block for long per read
  return true;
}

// Re-check the PN532 now and then; re-initialise it if it stopped answering.
void checkPN532() {
  if (millis() - lastHealthCheck < HEALTH_CHECK_MS) return;
  lastHealthCheck = millis();
  if (nfc.getFirmwareVersion()) {
    if (!pn532Ok) {
      pn532Ok = initPN532();
      if (pn532Ok) sendJson("{\"event\":\"ready\",\"fw\":\"" FW_VERSION "\",\"pn532\":\"" + pn532VersionString() + "\"}");
    }
    return;
  }
  if (pn532Ok) sendError("PN532 stopped responding - reinitialising");
  pn532Ok = initPN532();
  if (pn532Ok) {
    sendJson("{\"event\":\"ready\",\"fw\":\"" FW_VERSION "\",\"pn532\":\"" + pn532VersionString() + "\"}");
  } else {
    sendError("PN532 not found - check wiring and DIP switch (1=ON, 2=OFF)");
    blink(1, 300);
  }
}

// ---------- result feedback on the LED ----------

void showResult(const String &state) {
  if (state == "VERIFIED") {
    ledOn(); delay(1000); ledOff();
  } else if (state == "PENDING") {
    blink(2, 400);
  } else if (state == "UNKNOWN_CARD") {
    blink(3, 200);
  } else if (state == "REJECTED") {
    blink(6, 80);
  } else if (state == "OFFLINE" || state == "ERROR") {
    blink(10, 40);
  } else {
    sendError("unknown result: " + state);
    return;
  }
  sendOk("RESULT " + state);
}

// ---------- commands from the Pi ----------

void handleCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) return;

  if (cmd == "PING") {
    sendJson("{\"event\":\"pong\"}");
  } else if (cmd == "INFO") {
    sendJson("{\"event\":\"info\",\"fw\":\"" FW_VERSION "\",\"pn532\":\"" + pn532VersionString() +
             "\",\"pn532_ok\":" + String(pn532Ok ? "true" : "false") +
             ",\"scanning\":" + String(scanning ? "true" : "false") + "}");
  } else if (cmd == "START") {
    scanning = true;
    sendOk(cmd);
  } else if (cmd == "STOP") {
    scanning = false;
    sendOk(cmd);
  } else if (cmd == "BLINK") {
    blink(3, 100);
    sendOk(cmd);
  } else if (cmd.startsWith("RESULT ")) {
    String state = cmd.substring(7);
    state.trim();
    showResult(state);
  } else {
    sendError("unknown command: " + cmd);
  }
}

void readSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (rxLine.length()) handleCommand(rxLine);
      rxLine = "";
    } else if (rxLine.length() < 64) {
      rxLine += c;
    }
  }
}

// ---------- main ----------

void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledOff();

  Serial.begin(115200);
  delay(1000);  // give USB time to come up on the Pi

  Wire.begin(PN532_SDA, PN532_SCL);  // must come before nfc.begin()

  pn532Ok = initPN532();
  while (!pn532Ok) {
    sendError("PN532 not found - check wiring and DIP switch (1=ON, 2=OFF)");
    blink(1, 500);
    // Keep answering the Pi (PING/INFO) while waiting for the reader.
    for (int i = 0; i < 15; i++) { readSerialCommands(); delay(100); }
    pn532Ok = initPN532();
  }

  sendJson("{\"event\":\"ready\",\"fw\":\"" FW_VERSION "\",\"pn532\":\"" + pn532VersionString() + "\"}");
  blink(2, 150);
  lastHealthCheck = millis();
}

void loop() {
  readSerialCommands();

  if (!scanning) {
    delay(10);
    return;
  }

  // Only check health when no card was just read, so a tap is never delayed.
  if (millis() - lastUidTime > SAME_CARD_COOLDOWN_MS) checkPN532();
  if (!pn532Ok) {
    delay(10);
    return;
  }

  uint8_t uid[7] = {0};
  uint8_t uidLength = 0;

  // 100 ms timeout keeps the loop responsive to commands from the Pi
  bool found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100);

  if (found && uidLength > 0) {
    String uidHex = uidToHex(uid, uidLength);
    unsigned long now = millis();

    if (uidHex != lastUid || now - lastUidTime > SAME_CARD_COOLDOWN_MS) {
      sendJson("{\"event\":\"card\",\"uid\":\"" + uidHex + "\",\"len\":" + String(uidLength) + "}");
      ledOn(); delay(80); ledOff();   // "card read" flash; the result pattern follows from the Pi
    }
    lastUid = uidHex;
    lastUidTime = now;
  }
}
