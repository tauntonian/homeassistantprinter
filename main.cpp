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
 *
 * MQTT Topics (publish):
 *   homeassistant/printer/status         - "online" / "offline" / "printing" / "error"
 *   homeassistant/printer/paper          - "ok" / "low" / "out"
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include "ESCPOSPrinter.h"
#include "secrets.h"  // WiFi + MQTT credentials

// ── Pin config ────────────────────────────────────────────────────────────────
// XIAO ESP32-C3 UART1: TX=D6(GPIO21), RX=D7(GPIO20)
#define PRINTER_TX 21
#define PRINTER_RX 20
#define PRINTER_BAUD 9600   // QR204 default — change to 115200 if reconfigured

// ── MQTT ──────────────────────────────────────────────────────────────────────
#define MQTT_TOPIC_BASE     "homeassistant/printer"
#define MQTT_TOPIC_STATUS   MQTT_TOPIC_BASE "/status"
#define MQTT_TOPIC_PAPER    MQTT_TOPIC_BASE "/paper"
#define MQTT_CMD_TEXT       MQTT_TOPIC_BASE "/cmd/text"
#define MQTT_CMD_SHOPPING   MQTT_TOPIC_BASE "/cmd/shopping"
#define MQTT_CMD_RECIPE     MQTT_TOPIC_BASE "/cmd/recipe"
#define MQTT_CMD_QRCODE     MQTT_TOPIC_BASE "/cmd/qrcode"
#define MQTT_CMD_IMAGE      MQTT_TOPIC_BASE "/cmd/image"
#define MQTT_CMD_RAW        MQTT_TOPIC_BASE "/cmd/raw"

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
HardwareSerial printerSerial(1);
ESCPOSPrinter printer(printerSerial);

unsigned long lastReconnectAttempt = 0;

// ── Forward declarations ──────────────────────────────────────────────────────
void connectWiFi();
bool mqttReconnect();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void printText(JsonDocument& doc);
void printShopping(JsonDocument& doc);
void printRecipe(JsonDocument& doc);
void printQRCode(JsonDocument& doc);
void printImage(JsonDocument& doc);
void printRaw(const char* b64);
void publishStatus(const char* status);
String base64Decode(const char* encoded);

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  printerSerial.begin(PRINTER_BAUD, SERIAL_8N1, PRINTER_RX, PRINTER_TX);
  printer.begin();

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(8192);  // Large buffer for images
  mqttReconnect();
  publishStatus("online");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
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
    publishStatus("online");
    return true;
  }
  Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
  return false;
}

void publishStatus(const char* status) {
  mqtt.publish(MQTT_TOPIC_STATUS, status, true);
}

// ── MQTT callback dispatcher ──────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  publishStatus("printing");
  String t(topic);

  if (t == MQTT_CMD_RAW) {
    char buf[length + 1];
    memcpy(buf, payload, length);
    buf[length] = '\0';
    printRaw(buf);
    publishStatus("online");
    return;
  }

  // Parse JSON for all other commands
  StaticJsonDocument<4096> doc;
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

// Raw ESC/POS — base64 encoded byte string
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
