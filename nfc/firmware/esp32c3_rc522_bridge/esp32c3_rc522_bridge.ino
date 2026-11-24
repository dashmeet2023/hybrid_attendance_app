/*
  Hybrid Attendance - classroom RFID/NFC reader
  ESP32-C3 Super Mini  <->  RC522 (RFID-RC522, SPI)  <->  Raspberry Pi (USB serial)

  Same serial protocol as the PN532 version, so nfc/pi_nfc_reader.py works unchanged.

  Wiring (RC522 -> ESP32-C3 Super Mini):
    3.3V -> 3.3        (NEVER 5V)
    GND  -> G
    RST  -> GPIO3
    IRQ  -> not connected
    MISO -> GPIO5
    MOSI -> GPIO6
    SCK  -> GPIO4
    SDA  -> GPIO7      (SDA = SPI chip-select on this board)
  ESP32-C3 USB-C -> Raspberry Pi USB  (shows up as /dev/ttyACM0)

  Arduino IDE settings:
    Board: "ESP32C3 Dev Module" (or "Nologo ESP32C3 Super Mini")
    USB CDC On Boot: Enabled
    Library: "MFRC522" by GithubCommunity (miguelbalboa)

  Messages to the Pi (one JSON object per line):
    {"event":"ready","fw":"2.0-rc522","chip":"0x92"}
    {"event":"card","uid":"A1B2C3D4","len":4}
    {"event":"pong"} / {"event":"info",...} / {"event":"ok","cmd":"..."} / {"event":"error","msg":"..."}

  Commands from the Pi (one per line):
    PING, INFO, START, STOP, BLINK
    RESULT <VERIFIED|PENDING|UNKNOWN_CARD|REJECTED|OFFLINE|ERROR>  -> LED pattern:
      VERIFIED 1 long flash | PENDING 2 slow | UNKNOWN_CARD 3 medium | REJECTED 6 fast | OFFLINE/ERROR 10 very fast
*/

#include <SPI.h>
#include <MFRC522.h>

#define FW_VERSION "2.0-rc522"

#define RC522_SCK   4
#define RC522_MISO  5
#define RC522_MOSI  6
#define RC522_SS    7
#define RC522_RST   3
#define LED_PIN     8      // on-board blue LED, active LOW

#define SAME_CARD_COOLDOWN_MS 2000
#define HEALTH_CHECK_MS       5000

MFRC522 rfid(RC522_SS, RC522_RST);

bool scanning = true;
bool readerOk = false;
byte chipVersion = 0;

String lastUid = "";
unsigned long lastUidTime = 0;
unsigned long lastHealthCheck = 0;
String rxLine = "";

// ---------- helpers ----------

void ledOn()  { digitalWrite(LED_PIN, LOW); }
void ledOff() { digitalWrite(LED_PIN, HIGH); }

void blink(int times, int ms) {
  for (int i = 0; i < times; i++) { ledOn(); delay(ms); ledOff(); delay(ms); }
}

void sendJson(const String &json) { Serial.println(json); }
void sendOk(const String &cmd) { sendJson("{\"event\":\"ok\",\"cmd\":\"" + cmd + "\"}"); }
void sendError(const String &msg) { sendJson("{\"event\":\"error\",\"msg\":\"" + msg + "\"}"); }

String chipString() {
  if (!readerOk) return "none";
  String s = String(chipVersion, HEX);
  s.toUpperCase();
  return "0x" + s;
}

void sendReady() {
  sendJson("{\"event\":\"ready\",\"fw\":\"" FW_VERSION "\",\"chip\":\"" + chipString() + "\"}");
}

// ---------- RC522 ----------

// 0x00 or 0xFF means nothing answered on SPI (wiring/power problem).
bool chipAnswers() {
  chipVersion = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  return chipVersion != 0x00 && chipVersion != 0xFF;
}

bool initReader() {
  rfid.PCD_Init();
  delay(10);
  if (!chipAnswers()) return false;
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);  // better read range
  return true;
}

// Re-check the reader now and then; re-initialise it if it stopped answering.
void checkReader() {
  if (millis() - lastHealthCheck < HEALTH_CHECK_MS) return;
  lastHealthCheck = millis();
  if (chipAnswers()) {
    if (!readerOk) { readerOk = initReader(); if (readerOk) sendReady(); }
    return;
  }
  if (readerOk) sendError("RC522 stopped responding - reinitialising");
  readerOk = initReader();
  if (readerOk) sendReady();
  else { sendError("RC522 not found - check wiring (3.3V, GND, SCK=4, MISO=5, MOSI=6, SDA=7, RST=3)"); blink(1, 300); }
}

// ---------- result feedback ----------

void showResult(const String &state) {
  if (state == "VERIFIED")                          { ledOn(); delay(1000); ledOff(); }
  else if (state == "PENDING")                      blink(2, 400);
  else if (state == "UNKNOWN_CARD")                 blink(3, 200);
  else if (state == "REJECTED")                     blink(6, 80);
  else if (state == "OFFLINE" || state == "ERROR")  blink(10, 40);
  else { sendError("unknown result: " + state); return; }
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
    sendJson("{\"event\":\"info\",\"fw\":\"" FW_VERSION "\",\"chip\":\"" + chipString() +
             "\",\"reader_ok\":" + String(readerOk ? "true" : "false") +
             ",\"scanning\":" + String(scanning ? "true" : "false") + "}");
  } else if (cmd == "START") {
    scanning = true; sendOk(cmd);
  } else if (cmd == "STOP") {
    scanning = false; sendOk(cmd);
  } else if (cmd == "BLINK") {
    blink(3, 100); sendOk(cmd);
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
  delay(1000);

  SPI.begin(RC522_SCK, RC522_MISO, RC522_MOSI, RC522_SS);

  readerOk = initReader();
  while (!readerOk) {
    sendError("RC522 not found - check wiring (3.3V, GND, SCK=4, MISO=5, MOSI=6, SDA=7, RST=3)");
    blink(1, 500);
    for (int i = 0; i < 15; i++) { readSerialCommands(); delay(100); }
    readerOk = initReader();
  }
  sendReady();
  blink(2, 150);
  lastHealthCheck = millis();
}

void loop() {
  readSerialCommands();

  if (!scanning) { delay(10); return; }

  if (millis() - lastUidTime > SAME_CARD_COOLDOWN_MS) checkReader();
  if (!readerOk) { delay(10); return; }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) { delay(20); return; }

  String uidHex = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uidHex += "0";
    uidHex += String(rfid.uid.uidByte[i], HEX);
  }
  uidHex.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  unsigned long now = millis();
  if (uidHex != lastUid || now - lastUidTime > SAME_CARD_COOLDOWN_MS) {
    sendJson("{\"event\":\"card\",\"uid\":\"" + uidHex + "\",\"len\":" + String(rfid.uid.size) + "}");
    ledOn(); delay(80); ledOff();
  }
  lastUid = uidHex;
  lastUidTime = now;
}
