/**
 * QR204 Thermal Printer Controller
 * Hardware: SEEED XIAO ESP32-C3
 * Printer: QR204 (ESC/POS compatible)
 *
 * MQTT Topics (subscribe):
 *   homeassistant/printer/cmd/text       - Print plain text (JSON: {"text":"..."})
 *   homeassistant/printer/cmd/shopping   - Print shopping list (JSON: {"items":[...],"title":"..."})
 *   homeassistant/printer/cmd/recipe     - Print recipe (JSON: {"name":"...","ingredients":[...],"steps":[...]})
 *   homeassistant/printer/cmd/qrcode     - Print QR code (JSON: {"data":"...","label":"..."})
 *   homeassistant/printer/cmd/image      - Print bitmap image (JSON: {"bitmap":[...],"width":...,"height":...})
 *   homeassistant/printer/cmd/raw        - Raw ESC/POS bytes as base64 string
 *   homeassistant/printer/cmd/ota        - HTTP pull OTA (JSON: {"url":"http://...firmware.bin"})
 *   homeassistant/printer/cmd/restart    - Reboot the ESP32 (any payload)
 *
 * MQTT Topics (publish):
 *   homeassistant/printer/status         - "online" / "offline" / "printing" / "error" / "ota"
 *   homeassistant/printer/paper          - "ok" / "low" / "out"
 *   homeassistant/printer/version        - Firmware version string (published on connect)
 *   homeassistant/printer/ota/progress   - OTA progress 0–100 (published during HTTP OTA)
 *
 * OTA update methods:
 *   1. ArduinoOTA (push) — use `pio run -t upload --upload-port <IP>` or Arduino IDE
 *      Password set in secrets.h as OTA_PASSWORD
 *   2. HTTP pull OTA    — publish {"url":"http://host/firmware.bin"} to cmd/ota
 *      The ESP fetches and flashes the binary, then reboots automatically.
 */

#define FIRMWARE_VERSION "1.1.0"

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <Update.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include "ESCPOSPrinter.h"
#include "secrets.h"  // WiFi + MQTT credentials

// ── Pin config ────────────────────────────────────────────────────────────────
// XIAO ESP32-C3 pinout (confirmed from datasheet diagram):
//   D6 = GPIO21 = UART TX  → connect to printer RX (white wire)
//   D7 = GPIO20 = UART RX  → connect to printer TX (green wire)
// NOTE: D6 is labelled "TX" and D7 is labelled "RX" on the board silkscreen.
#define PRINTER_TX 21   // D6
#define PRINTER_RX 20   // D7
#define PRINTER_BAUD 9600   // QR204 default — change to 115200 if reconfigured

// ── MQTT ──────────────────────────────────────────────────────────────────────
#define MQTT_TOPIC_BASE     "homeassistant/printer"
#define MQTT_TOPIC_STATUS   MQTT_TOPIC_BASE "/status"
#define MQTT_TOPIC_PAPER    MQTT_TOPIC_BASE "/paper"
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

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
HardwareSerial printerSerial(1);
ESCPOSPrinter printer(printerSerial);

unsigned long lastReconnectAttempt = 0;

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
void handleHttpOta(const char* url);
void publishStatus(const char* status);
void publishProgress(int pct);
String base64Decode(const char* encoded);

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  printerSerial.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX, PRINTER_TX);
  printer.begin();

  connectWiFi();
  setupArduinoOTA();   // ArduinoOTA must start after WiFi

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(8192);  // Large buffer for images
  mqttReconnect();
  publishStatus("online");
  mqtt.publish(MQTT_TOPIC_VERSION, FIRMWARE_VERSION, true);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();  // Must be called every loop for push OTA to work

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
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
  }
}

// ── ArduinoOTA (push OTA from PlatformIO / Arduino IDE) ──────────────────────
void setupArduinoOTA() {
  ArduinoOTA.setHostname("esp32-printer");
  ArduinoOTA.setPassword(OTA_PASSWORD);  // defined in secrets.h

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.printf("OTA start: %s\n", type.c_str());
    publishStatus("ota");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("OTA complete — rebooting");
    publishStatus("offline");
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    int pct = (done * 100) / total;
    Serial.printf("OTA progress: %d%%\r", pct);
    // Throttle MQTT publishes to every 10% to avoid flooding the broker
    static int lastPct = -1;
    if (pct / 10 != lastPct / 10) {
      publishProgress(pct);
      lastPct = pct;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error [%u]: ", error);
    if      (error == OTA_AUTH_ERROR)    Serial.println("Auth failed");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive failed");
    else if (error == OTA_END_ERROR)     Serial.println("End failed");
    publishStatus("error");
  });

  ArduinoOTA.begin();
  Serial.printf("ArduinoOTA ready — hostname: esp32-printer  IP: %s\n",
                WiFi.localIP().toString().c_str());
}

// ── MQTT connection ───────────────────────────────────────────────────────────
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

// ── MQTT callback dispatcher ──────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);

  // ── Restart command — no JSON needed ────────────────────────────────────────
  if (t == MQTT_CMD_RESTART) {
    Serial.println("Restart command received — rebooting");
    publishStatus("offline");
    mqtt.disconnect();
    delay(200);
    ESP.restart();
    return;
  }

  // ── HTTP pull OTA ────────────────────────────────────────────────────────────
  if (t == MQTT_CMD_OTA) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) {
      Serial.println("OTA: bad JSON");
      publishStatus("error");
      return;
    }
    const char* url = doc["url"] | "";
    if (!strlen(url)) {
      Serial.println("OTA: missing url field");
      publishStatus("error");
      return;
    }
    handleHttpOta(url);
    return;
  }

  publishStatus("printing");

  if (t == MQTT_CMD_RAW) {
    char buf[length + 1];
    memcpy(buf, payload, length);
    buf[length] = '\0';
    printRaw(buf);
    publishStatus("online");
    return;
  }

  // Parse JSON for all other commands
  // JsonDocument is the ArduinoJson v7 replacement for StaticJsonDocument.
  // It allocates from the heap and sizes itself automatically.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    publishStatus("error");
    return;
  }

  if      (t == MQTT_CMD_TEXT)     printText(doc);
  else if (t == MQTT_CMD_SHOPPING) printShopping(doc);
  else if (t == MQTT_CMD_RECIPE)   printRecipe(doc);
  else if (t == MQTT_CMD_QRCODE)   printQRCode(doc);
  else if (t == MQTT_CMD_IMAGE)    printImage(doc);

  publishStatus("online");
}

// ── Print handlers ────────────────────────────────────────────────────────────

// {"text":"Hello World!","bold":false,"size":1,"align":"left","cut":true}
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

// {"title":"Shopping List","items":["Milk","Eggs","Bread"],"cut":true}
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

// {"name":"...","servings":"4","time":"30min","ingredients":[...],"steps":[...]}
void printRecipe(JsonDocument& doc) {
  const char* name      = doc["name"]     | "Recipe";
  const char* servings  = doc["servings"] | "";
  const char* time      = doc["time"]     | "";
  bool cut              = doc["cut"]      | true;

  // Header
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

  // Ingredients
  printer.setAlign("left");
  printer.setBold(true);
  printer.println("INGREDIENTS");
  printer.setBold(false);
  JsonArray ingredients = doc["ingredients"].as<JsonArray>();
  for (JsonVariant ing : ingredients) {
    char line[64];
    snprintf(line, sizeof(line), "  * %s", ing.as<const char*>());
    printer.println(line);
  }

  printer.println("");
  printer.setBold(true);
  printer.println("STEPS");
  printer.setBold(false);

  JsonArray steps = doc["steps"].as<JsonArray>();
  int s = 1;
  for (JsonVariant step : steps) {
    char num[8];
    snprintf(num, sizeof(num), "%d.", s++);
    printer.print(num);
    printer.println(step.as<const char*>());
    printer.println("");
  }

  printer.reset();
  if (cut) printer.feedAndCut();
}

// {"data":"https://...","label":"Join WiFi","size":8}
void printQRCode(JsonDocument& doc) {
  const char* data  = doc["data"]  | "https://home.local";
  const char* label = doc["label"] | "";
  int         size  = doc["size"]  | 6;  // 1-8, QR204 model size
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

// {"bitmap":[0,0,255,...],"width":384,"height":200}
// bitmap = flat array of 1-bit packed bytes (ESC/POS raster format)
void printImage(JsonDocument& doc) {
  int width   = doc["width"]  | 0;
  int height  = doc["height"] | 0;
  bool cut    = doc["cut"]    | true;
  JsonArray bitmapArr = doc["bitmap"].as<JsonArray>();

  if (!width || !height || bitmapArr.isNull()) {
    Serial.println("Invalid image payload");
    return;
  }

  int byteWidth = (width + 7) / 8;
  int totalBytes = byteWidth * height;
  uint8_t* buf = (uint8_t*)malloc(totalBytes);
  if (!buf) { Serial.println("OOM"); return; }

  int idx = 0;
  for (JsonVariant b : bitmapArr) {
    if (idx < totalBytes) buf[idx++] = b.as<uint8_t>();
  }

  printer.setAlign("center");
  printer.printBitmap(width, height, buf);
  free(buf);
  printer.reset();
  if (cut) printer.feedAndCut();
}

// ── HTTP pull OTA ─────────────────────────────────────────────────────────────
// Triggered via MQTT: {"url":"http://192.168.1.x:8080/firmware.bin"}
// The ESP fetches the binary over HTTP, writes it to flash, then reboots.
// Progress is reported to homeassistant/printer/ota/progress (0-100).
void handleHttpOta(const char* url) {
  Serial.printf("HTTP OTA starting: %s\n", url);
  publishStatus("ota");
  publishProgress(0);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);  // 30 s timeout for slow local servers

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP OTA: server returned %d\n", httpCode);
    http.end();
    publishStatus("error");
    return;
  }

  int contentLen = http.getSize();
  if (contentLen <= 0) {
    Serial.println("HTTP OTA: unknown content length — continuing anyway");
  }

  if (!Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
    Serial.println("HTTP OTA: not enough flash space");
    Update.printError(Serial);
    http.end();
    publishStatus("error");
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  int written = 0;
  int lastReportedPct = -1;

  while (http.connected() && (contentLen <= 0 || written < contentLen)) {
    size_t available = stream->available();
    if (!available) { delay(1); continue; }

    size_t toRead = min(available, sizeof(buf));
    size_t read   = stream->readBytes(buf, toRead);
    size_t wr     = Update.write(buf, read);
    if (wr != read) {
      Serial.printf("HTTP OTA: write mismatch (%u vs %u)\n", wr, read);
      break;
    }
    written += read;

    if (contentLen > 0) {
      int pct = (written * 100) / contentLen;
      if (pct / 10 != lastReportedPct / 10) {
        publishProgress(pct);
        mqtt.loop();  // keep MQTT alive during long flash
        lastReportedPct = pct;
      }
    }
  }

  http.end();

  if (Update.end()) {
    if (Update.isFinished()) {
      Serial.println("HTTP OTA: success — rebooting");
      publishProgress(100);
      publishStatus("offline");
      mqtt.disconnect();
      delay(300);
      ESP.restart();
    } else {
      Serial.println("HTTP OTA: update not finished");
      publishStatus("error");
    }
  } else {
    Serial.printf("HTTP OTA error: ");
    Update.printError(Serial);
    publishStatus("error");
  }
}

// ── Raw ESC/POS ───────────────────────────────────────────────────────────────
void printRaw(const char* b64) {
  String decoded = base64Decode(b64);
  printerSerial.write((const uint8_t*)decoded.c_str(), decoded.length());
  delay(500);
}

// ── Base64 decoder ────────────────────────────────────────────────────────────
static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
String base64Decode(const char* encoded) {
  String out;
  int len = strlen(encoded);
  int i = 0;
  while (i < len) {
    uint8_t c[4] = {0,0,0,0};
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
