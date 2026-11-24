/*
  Hybrid Attendance - classroom RFID reader (RC522) -> Raspberry Pi (USB serial)
  One file for: ESP32 DevKit (WROOM-32), ESP8266 NodeMCU, ESP32-C3 Super Mini.
  The board is detected at compile time; wiring is the same as the test code.

  Wiring (RC522 -> board):
               ESP32 DevKit   ESP8266 NodeMCU   ESP32-C3
    3.3V       3V3            3V                3.3       (NEVER 5V)
    GND        GND            G                 G
    RST        D22            D3                3
    MISO       D19            D6                5
    MOSI       D23            D7                6
    SCK        D18            D5                4
    SDA        D5             D8                7
    IRQ        not connected

  Library: "MFRC522" by GithubCommunity.  Serial 115200.
  Pi: python nfc/pi_nfc_reader.py /dev/ttyUSB0   (ESP32-C3: /dev/ttyACM0)

  Messages to the Pi:   {"event":"ready"|"card"|"pong"|"info"|"ok"|"error", ...}
  Commands from the Pi: PING, INFO, START, STOP, BLINK,
                        RESULT VERIFIED|PENDING|UNKNOWN_CARD|REJECTED|OFFLINE|ERROR
  LED: VERIFIED 1 long | PENDING 2 slow | UNKNOWN_CARD 3 medium | REJECTED 6 fast | OFFLINE/ERROR 10 very fast
*/
#include <SPI.h>
#include <MFRC522.h>

#if defined(ESP8266)
  #define BOARD_NAME "esp8266"
  #define SS_PIN  D8
  #define RST_PIN D3
  #define LED_PIN LED_BUILTIN
  #define LED_ACTIVE_LOW 1
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
  #define BOARD_NAME "esp32c3"
  #define SCK_PIN  4
  #define MISO_PIN 5
  #define MOSI_PIN 6
  #define SS_PIN   7
  #define RST_PIN  3
  #define LED_PIN  8
  #define LED_ACTIVE_LOW 1
#else
  #define BOARD_NAME "esp32"
  #define SCK_PIN  18
  #define MISO_PIN 19
  #define MOSI_PIN 23
  #define SS_PIN   5
  #define RST_PIN  22
  #define LED_PIN  2
  #define LED_ACTIVE_LOW 0
#endif

#define FW_VERSION "2.1-rc522-" BOARD_NAME
#define SAME_CARD_COOLDOWN_MS 2000
#define HEALTH_CHECK_MS       5000

MFRC522 rfid(SS_PIN, RST_PIN);
bool scanning = true, readerOk = false;
byte chipVersion = 0;
String lastUid = "", rxLine = "";
unsigned long lastUidTime = 0, lastHealthCheck = 0;

void ledOn()  { digitalWrite(LED_PIN, LED_ACTIVE_LOW ? LOW : HIGH); }
void ledOff() { digitalWrite(LED_PIN, LED_ACTIVE_LOW ? HIGH : LOW); }
void blink(int n, int ms) { for (int i = 0; i < n; i++) { ledOn(); delay(ms); ledOff(); delay(ms); } }

void sendJson(const String &j) { Serial.println(j); }
void sendOk(const String &c) { sendJson("{\"event\":\"ok\",\"cmd\":\"" + c + "\"}"); }
void sendError(const String &m) { sendJson("{\"event\":\"error\",\"msg\":\"" + m + "\"}"); }
String chipString() { if (!readerOk) return "none"; String s = String(chipVersion, HEX); s.toUpperCase(); return "0x" + s; }
void sendReady() { sendJson("{\"event\":\"ready\",\"fw\":\"" FW_VERSION "\",\"chip\":\"" + chipString() + "\"}"); }

bool chipAnswers() {
  chipVersion = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  return chipVersion != 0x00 && chipVersion != 0xFF;
}

bool initReader() {
  rfid.PCD_Init();
  delay(10);
  if (!chipAnswers()) return false;
  rfid.PCD_SetAntennaGain(rfid.RxGain_max);
  return true;
}

// Re-check the reader every few seconds; re-initialise it after a glitch.
void checkReader() {
  if (millis() - lastHealthCheck < HEALTH_CHECK_MS) return;
  lastHealthCheck = millis();
  if (chipAnswers()) { if (!readerOk) { readerOk = initReader(); if (readerOk) sendReady(); } return; }
  if (readerOk) sendError("RC522 stopped responding - reinitialising");
  readerOk = initReader();
  if (readerOk) sendReady(); else { sendError("RC522 not found - check wiring"); blink(1, 300); }
}

void showResult(const String &s) {
  if (s == "VERIFIED")                        { ledOn(); delay(1000); ledOff(); }
  else if (s == "PENDING")                    blink(2, 400);
  else if (s == "UNKNOWN_CARD")               blink(3, 200);
  else if (s == "REJECTED")                   blink(6, 80);
  else if (s == "OFFLINE" || s == "ERROR")    blink(10, 40);
  else { sendError("unknown result: " + s); return; }
  sendOk("RESULT " + s);
}

void handleCommand(String cmd) {
  cmd.trim(); cmd.toUpperCase();
  if (!cmd.length()) return;
  if (cmd == "PING") sendJson("{\"event\":\"pong\"}");
  else if (cmd == "INFO")
    sendJson("{\"event\":\"info\",\"fw\":\"" FW_VERSION "\",\"chip\":\"" + chipString() +
             "\",\"reader_ok\":" + String(readerOk ? "true" : "false") +
             ",\"scanning\":" + String(scanning ? "true" : "false") + "}");
  else if (cmd == "START") { scanning = true; sendOk(cmd); }
  else if (cmd == "STOP")  { scanning = false; sendOk(cmd); }
  else if (cmd == "BLINK") { blink(3, 100); sendOk(cmd); }
  else if (cmd.startsWith("RESULT ")) { String s = cmd.substring(7); s.trim(); showResult(s); }
  else sendError("unknown command: " + cmd);
}

void readSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (rxLine.length()) handleCommand(rxLine); rxLine = ""; }
    else if (rxLine.length() < 64) rxLine += c;
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  ledOff();
  Serial.begin(115200);
  delay(1000);
#if defined(ESP8266)
  SPI.begin();
#else
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
#endif
  readerOk = initReader();
  while (!readerOk) {
    sendError("RC522 not found - check wiring");
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

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  unsigned long now = millis();
  if (uid != lastUid || now - lastUidTime > SAME_CARD_COOLDOWN_MS) {
    sendJson("{\"event\":\"card\",\"uid\":\"" + uid + "\",\"len\":" + String(rfid.uid.size) + "}");
    ledOn(); delay(80); ledOff();
  }
  lastUid = uid;
  lastUidTime = now;
}
