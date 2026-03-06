/**
 * QR204 Thermal Printer Controller
 * Hardware: SEEED XIAO ESP32-C3
 * Printer: QR204 (ESC/POS compatible, 9600 baud, inverted TTL logic)
 *
 * Hardware notes (confirmed during bring-up):
 *   - QR204 uses INVERTED TTL logic on its UART lines. The ESP32 UART driver
 *     handles this with the `invert=true` flag in HardwareSerial::begin().
 *     No level shifter or hardware inverter is needed.
 *   - Do NOT send ESC @ (0x1B 0x40) to this printer — it disrupts its state
 *     and causes subsequent commands to be silently ignored.
 *   - PubSubClient payload pointer must be copied to a heap buffer as the
 *     VERY FIRST operation in the MQTT callback, before any String construction
 *     or Serial calls, to prevent heap reallocation from invalidating the pointer.
 *
 * Wiring:
 *   D6 / GPIO21 (TX) → Printer RX
 *   D7 / GPIO20 (RX) ← Printer TX
 *   5V               → Printer VCC
 *   GND              → Printer GND
 *
 * MQTT Topics (subscribe):
 *   homeassistant/printer/cmd/text       - {"text":"...","bold":false,"size":1,"align":"left","cut":true}
 *   homeassistant/printer/cmd/shopping   - {"title":"...","items":["..."],"cut":true}
 *   homeassistant/printer/cmd/recipe     - {"name":"...","servings":"4","time":"30m","ingredients":[...],"steps":[...]}
 *   homeassistant/printer/cmd/qrcode     - {"data":"...","label":"...","size":6,"cut":true}
 *   homeassistant/printer/cmd/image      - {"bitmap":[...],"width":384,"height":200,"cut":true}
 *   homeassistant/printer/cmd/raw        - base64-encoded raw ESC/POS bytes
 *   homeassistant/printer/cmd/ota        - {"url":"http://host/firmware.bin"}
 *   homeassistant/printer/cmd/restart    - any payload
 *   homeassistant/printer/cmd/diagnose   - any payload — prints a hardware test page
 *
 * MQTT Topics (publish):
 *   homeassistant/printer/status         - online / offline / printing / error / ota
 *   homeassistant/printer/version        - firmware version string
 *   homeassistant/printer/ota/progress   - 0-100 during HTTP OTA
 */

#define FIRMWARE_VERSION "1.2.0"  // Confirmed working: 9600 baud, inverted TTL

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include "ESCPOSPrinter.h"
#include "secrets.h"

// ── Pin config ────────────────────────────────────────────────────────────────
#define PRINTER_TX   21      // D6 on XIAO ESP32-C3 silkscreen
#define PRINTER_RX   20      // D7 on XIAO ESP32-C3 silkscreen
#define PRINTER_BAUD 9600    // QR204 factory default

// ── MQTT topics ───────────────────────────────────────────────────────────────
#define MQTT_TOPIC_BASE     "homeassistant/printer"
#define MQTT_TOPIC_STATUS   MQTT_TOPIC_BASE "/status"
#define MQTT_TOPIC_VERSION  MQTT_TOPIC_BASE "/version"
#define MQTT_TOPIC_OTA_PROG MQTT_TOPIC_BASE "/ota/progress"
#define MQTT_CMD_TEXT       MQTT_TOPIC_BASE "/cmd/text"
#define MQTT_CMD_SHOPPING   MQTT_TOPIC_BASE "/cmd/shopping"
#define MQTT_CMD_RECIPE     MQTT_TOPIC_BASE "/cmd/recipe"
#define MQTT_CMD_QRCODE     MQTT_TOPIC_BASE "/cmd/qrcode"
#define MQTT_CMD_IMAGE      MQTT_TOPIC_BASE "/cmd/image"
#define MQTT_CMD_RAW        MQTT_TOPIC_BASE "/cmd/raw"
#define MQTT_CMD_OTA        MQTT_TOPIC_BASE "/cmd/ota"
#define MQTT_CMD_RESTART    MQTT_TOPIC_BASE "/cmd/restart"
#define MQTT_CMD_DIAGNOSE   MQTT_TOPIC_BASE "/cmd/diagnose"

// ── Globals ───────────────────────────────────────────────────────────────────
WiFiClient     wifiClient;
PubSubClient   mqtt(wifiClient);
HardwareSerial printerSerial(1);
ESCPOSPrinter  printer(printerSerial);
unsigned long  lastReconnectAttempt = 0;

// ── Forward declarations ──────────────────────────────────────────────────────
void connectWiFi();
void setupArduinoOTA();
bool mqttReconnect();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void printText(JsonDocument& doc);
void printShopping(JsonDocument& doc);
void printRecipe(JsonDocument& doc);
void printQRCode(JsonDocument& doc);
void printImage(JsonDocument& doc);
void printRaw(const char* b64);
void sendDiagnose();
void handleHttpOta(const char* url);
void publishStatus(const char* status);
void publishProgress(int pct);
String base64Decode(const char* encoded);

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // true = invert signal polarity — required for QR204's inverted TTL UART
  printerSerial.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX, PRINTER_TX, true);
  printer.begin();

  connectWiFi();
  setupArduinoOTA();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  if (!mqtt.setBufferSize(8192))
    Serial.println("WARN: MQTT buffer allocation failed");

  mqttReconnect();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();

  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      mqttReconnect();
    }
  } else {
    mqtt.loop();
  }
}

// ── WiFi ──────────────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
}

// ── ArduinoOTA ────────────────────────────────────────────────────────────────
void setupArduinoOTA() {
  ArduinoOTA.setHostname("esp32-printer");
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA start");
    publishStatus("ota");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("OTA complete");
    publishStatus("offline");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static int lastPct = -1;
    int pct = (done * 100) / total;
    if (pct / 10 != lastPct / 10) { publishProgress(pct); lastPct = pct; }
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error [%u]\n", error);
    publishStatus("error");
  });

  ArduinoOTA.begin();
  Serial.printf("ArduinoOTA ready — IP: %s\n", WiFi.localIP().toString().c_str());
}

// ── MQTT ──────────────────────────────────────────────────────────────────────
bool mqttReconnect() {
  if (mqtt.connect("esp32_printer", MQTT_USER, MQTT_PASS,
                   MQTT_TOPIC_STATUS, 0, true, "offline")) {
    Serial.println("MQTT connected");
    mqtt.subscribe(MQTT_CMD_TEXT);
    mqtt.subscribe(MQTT_CMD_SHOPPING);
    mqtt.subscribe(MQTT_CMD_RECIPE);
    mqtt.subscribe(MQTT_CMD_QRCODE);
    mqtt.subscribe(MQTT_CMD_IMAGE);
    mqtt.subscribe(MQTT_CMD_RAW);
    mqtt.subscribe(MQTT_CMD_OTA);
    mqtt.subscribe(MQTT_CMD_RESTART);
    mqtt.subscribe(MQTT_CMD_DIAGNOSE);
    publishStatus("online");
    mqtt.publish(MQTT_TOPIC_VERSION, FIRMWARE_VERSION, true);
    return true;
  }
  Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
  return false;
}

void publishStatus(const char* status) {
  mqtt.publish(MQTT_TOPIC_STATUS, status, true);
}

void publishProgress(int pct) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", pct);
  mqtt.publish(MQTT_TOPIC_OTA_PROG, buf, false);
}

// ── MQTT callback ─────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Copy payload to heap-allocated buffer as the VERY FIRST operation.
  // Any heap activity (String, Serial, etc.) before this can trigger a
  // reallocation that shifts PubSubClient's internal buffer, corrupting
  // the payload pointer. (Confirmed bug on ESP32-C3 + PubSubClient.)
  char* buf = (char*)malloc(length + 1);
  if (!buf) return;
  memcpy(buf, payload, length);
  buf[length] = '\0';

  String t(topic);

  // ── No-JSON commands ──────────────────────────────────────────────────────
  if (t == MQTT_CMD_RESTART) {
    Serial.println("Restarting...");
    free(buf);
    publishStatus("offline");
    mqtt.disconnect();
    delay(200);
    ESP.restart();
    return;
  }

  if (t == MQTT_CMD_DIAGNOSE) {
    free(buf);
    sendDiagnose();
    return;
  }

  if (t == MQTT_CMD_RAW) {
    printRaw(buf);
    free(buf);
    return;
  }

  // ── JSON commands ─────────────────────────────────────────────────────────
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, buf);
  free(buf);

  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    publishStatus("error");
    return;
  }

  if (t == MQTT_CMD_OTA) {
    const char* url = doc["url"] | "";
    if (strlen(url)) handleHttpOta(url);
    else publishStatus("error");
    return;
  }

  publishStatus("printing");

  if      (t == MQTT_CMD_TEXT)     printText(doc);
  else if (t == MQTT_CMD_SHOPPING) printShopping(doc);
  else if (t == MQTT_CMD_RECIPE)   printRecipe(doc);
  else if (t == MQTT_CMD_QRCODE)   printQRCode(doc);
  else if (t == MQTT_CMD_IMAGE)    printImage(doc);

  publishStatus("online");
}

// ── Print handlers ────────────────────────────────────────────────────────────

void printText(JsonDocument& doc) {
  const char* text  = doc["text"]  | "No text";
  bool bold         = doc["bold"]  | false;
  int  size         = doc["size"]  | 1;
  const char* align = doc["align"] | "left";
  bool cut          = doc["cut"]   | true;

  printer.setAlign(align);
  printer.setSize(size);
  if (bold) printer.setBold(true);
  printer.println(text);
  if (bold) printer.setBold(false);
  printer.reset();
  if (cut) printer.feedAndCut();
}

void printShopping(JsonDocument& doc) {
  const char* title = doc["title"] | "Shopping List";
  bool cut          = doc["cut"]   | true;

  printer.setAlign("center");
  printer.setSize(2);
  printer.setBold(true);
  printer.println(title);
  printer.setBold(false);
  printer.setSize(1);
  printer.println("──────────────────────");
  printer.setAlign("left");

  JsonArray items = doc["items"].as<JsonArray>();
  int i = 1;
  for (JsonVariant item : items) {
    char line[64];
    snprintf(line, sizeof(line), "%2d. %s", i++, item.as<const char*>());
    printer.println(line);
  }

  printer.println("");
  char footer[32];
  snprintf(footer, sizeof(footer), "Total: %d items", (int)items.size());
  printer.setAlign("center");
  printer.println(footer);
  printer.reset();
  if (cut) printer.feedAndCut();
}

void printRecipe(JsonDocument& doc) {
  const char* name     = doc["name"]     | "Recipe";
  const char* servings = doc["servings"] | "";
  const char* time     = doc["time"]     | "";
  bool cut             = doc["cut"]      | true;

  printer.setAlign("center");
  printer.setSize(2);
  printer.setBold(true);
  printer.println(name);
  printer.setBold(false);
  printer.setSize(1);

  if (strlen(servings) || strlen(time)) {
    char meta[64] = "";
    if (strlen(servings)) snprintf(meta, sizeof(meta), "Servings: %s", servings);
    if (strlen(time))     snprintf(meta + strlen(meta), sizeof(meta) - strlen(meta), "  Time: %s", time);
    printer.println(meta);
  }
  printer.println("──────────────────────");

  printer.setAlign("left");
  printer.setBold(true);
  printer.println("INGREDIENTS");
  printer.setBold(false);
  for (JsonVariant ing : doc["ingredients"].as<JsonArray>()) {
    char line[64];
    snprintf(line, sizeof(line), "  * %s", ing.as<const char*>());
    printer.println(line);
  }

  printer.println("");
  printer.setBold(true);
  printer.println("STEPS");
  printer.setBold(false);
  int s = 1;
  for (JsonVariant step : doc["steps"].as<JsonArray>()) {
    char num[8];
    snprintf(num, sizeof(num), "%d.", s++);
    printer.print(num);
    printer.println(step.as<const char*>());
    printer.println("");
  }

  printer.reset();
  if (cut) printer.feedAndCut();
}

void printQRCode(JsonDocument& doc) {
  const char* data  = doc["data"]  | "https://home.local";
  const char* label = doc["label"] | "";
  int         size  = doc["size"]  | 6;
  bool cut          = doc["cut"]   | true;

  printer.setAlign("center");
  if (strlen(label)) {
    printer.setBold(true);
    printer.println(label);
    printer.setBold(false);
  }
  printer.printQRCode(data, size);
  printer.println("");
  printer.reset();
  if (cut) printer.feedAndCut();
}

void printImage(JsonDocument& doc) {
  int width           = doc["width"]  | 0;
  int height          = doc["height"] | 0;
  bool cut            = doc["cut"]    | true;
  JsonArray bitmapArr = doc["bitmap"].as<JsonArray>();

  if (!width || !height || bitmapArr.isNull()) {
    Serial.println("printImage: invalid payload");
    return;
  }

  int byteWidth  = (width + 7) / 8;
  int totalBytes = byteWidth * height;
  uint8_t* imgBuf = (uint8_t*)malloc(totalBytes);
  if (!imgBuf) { Serial.println("printImage: OOM"); return; }

  int idx = 0;
  for (JsonVariant b : bitmapArr)
    if (idx < totalBytes) imgBuf[idx++] = b.as<uint8_t>();

  printer.setAlign("center");
  printer.printBitmap(width, height, imgBuf);
  free(imgBuf);
  printer.reset();
  if (cut) printer.feedAndCut();
}

// ── Diagnose ──────────────────────────────────────────────────────────────────
void sendDiagnose() {
  // Sends raw ESC/POS bytes, bypassing ESCPOSPrinter entirely.
  // Use to verify the physical UART link independently of higher-level code.
  printerSerial.write(0x1B); printerSerial.write(0x61); printerSerial.write(0x01); // centre
  printerSerial.write(0x1D); printerSerial.write(0x21); printerSerial.write(0x11); // 2x size
  printerSerial.print("** DIAG OK **"); printerSerial.write(0x0A);
  printerSerial.write(0x1D); printerSerial.write(0x21); printerSerial.write(0x00); // normal
  char line[48];
  snprintf(line, sizeof(line), "FW: %s  Baud: %d", FIRMWARE_VERSION, PRINTER_BAUD);
  printerSerial.print(line); printerSerial.write(0x0A);
  printerSerial.write(0x1B); printerSerial.write(0x64); printerSerial.write(4);    // feed
  printerSerial.write(0x1D); printerSerial.write(0x56); printerSerial.write(0x01); // cut
  printerSerial.flush();
}

// ── Raw ESC/POS ───────────────────────────────────────────────────────────────
void printRaw(const char* b64) {
  String decoded = base64Decode(b64);
  printerSerial.write((const uint8_t*)decoded.c_str(), decoded.length());
  printerSerial.flush();
}

// ── HTTP pull OTA ─────────────────────────────────────────────────────────────
void handleHttpOta(const char* url) {
  Serial.printf("HTTP OTA: %s\n", url);
  publishStatus("ota");
  publishProgress(0);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP OTA: server returned %d\n", httpCode);
    http.end(); publishStatus("error"); return;
  }

  int contentLen = http.getSize();
  if (!Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
    Update.printError(Serial);
    http.end(); publishStatus("error"); return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t otaBuf[512];
  int written = 0, lastPct = -1;

  while (http.connected() && (contentLen <= 0 || written < contentLen)) {
    size_t avail = stream->available();
    if (!avail) { delay(1); continue; }
    size_t rd = stream->readBytes(otaBuf, min(avail, sizeof(otaBuf)));
    if (Update.write(otaBuf, rd) != rd) break;
    written += rd;
    if (contentLen > 0) {
      int pct = (written * 100) / contentLen;
      if (pct / 10 != lastPct / 10) {
        publishProgress(pct);
        mqtt.loop();
        lastPct = pct;
      }
    }
  }
  http.end();

  if (Update.end() && Update.isFinished()) {
    Serial.println("HTTP OTA: success — rebooting");
    publishProgress(100);
    publishStatus("offline");
    mqtt.disconnect();
    delay(300);
    ESP.restart();
  } else {
    Update.printError(Serial);
    publishStatus("error");
  }
}

// ── Base64 decoder ────────────────────────────────────────────────────────────
static const char b64chars[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64Decode(const char* encoded) {
  String out;
  int len = strlen(encoded), i = 0;
  while (i < len) {
    uint8_t c[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4 && i < len; j++, i++) {
      const char* p = strchr(b64chars, encoded[i]);
      c[j] = p ? (p - b64chars) : 0;
    }
    out += (char)((c[0] << 2) | (c[1] >> 4));
    if (encoded[i-2] != '=') out += (char)(((c[1] & 0xF) << 4) | (c[2] >> 2));
    if (encoded[i-1] != '=') out += (char)(((c[2] & 0x3) << 6) | c[3]);
  }
  return out;
}