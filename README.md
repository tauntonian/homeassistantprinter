# QR204 Thermal Printer — Home Assistant Integration

Control a **QR204 ESC/POS thermal printer** via a **SEEED XIAO ESP32-C3** over MQTT, fully integrated with Home Assistant.

---

## Architecture

```
Home Assistant / CLI
       │  MQTT
       ▼
 SEEED XIAO ESP32-C3  ──UART──▶  QR204 Thermal Printer
```

---

## Hardware Wiring

| XIAO ESP32-C3 | QR204 Printer | Notes                     |
|---------------|---------------|---------------------------|
| 5V            | VCC (red)     | Printer needs 5–9V        |
| GND           | GND (black)   | Common ground             |
| **D6** / GPIO21 | RX (white)  | ESP32 TX → Printer RX     |
| **D7** / GPIO20 | TX (green)  | ESP32 RX ← Printer TX     |

> **Pin labels:** On the XIAO ESP32-C3 silkscreen, **D6 is the TX pin (GPIO21)** and **D7 is the RX pin (GPIO20)**. Wire D6 to the printer's RX line and D7 to the printer's TX line.

> **Baud rate:** The QR204 typically ships at **9600 baud**. Check your printer's DIP switches or config sheet. Update `PRINTER_BAUD` in `main.cpp` if yours is different (some units ship at 115200).

---

## Firmware Setup

### Prerequisites
- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- SEEED XIAO ESP32-C3 board

### Steps

1. **Clone / copy** the `printer_esp32/` folder.
2. **Edit `src/secrets.h`** with your WiFi and MQTT credentials.
3. **Build & upload:**
   ```bash
   cd printer_esp32
   pio run --target upload
   pio device monitor   # watch serial output
   ```
4. On first boot you should see `MQTT connected` in the serial monitor and the printer status sensor in HA will show `online`.

---

## Home Assistant Setup

### 1. MQTT Broker
Ensure the [Mosquitto broker add-on](https://github.com/home-assistant/addons/tree/master/mosquitto) (or any MQTT broker) is running and configured in HA's MQTT integration.

### 2. Add configuration
Copy the contents of `printer_integration.yaml` into your `configuration.yaml`, or use the [packages](https://www.home-assistant.io/docs/configuration/packages/) feature:

```yaml
# configuration.yaml
homeassistant:
  packages:
    printer: !include packages/printer.yaml
```

Then place `printer_integration.yaml` as `config/packages/printer.yaml`.

### 3. Fill in your details
- Replace `YOUR_MEALIE_HOST`, `YOUR_MEALIE_API_TOKEN`
- Replace `YOUR_HA_LONG_LIVED_TOKEN` (Settings → Profile → Long-Lived Access Tokens)
- Set your WiFi SSID/password in the `printer_wifi_qr` script

### 4. Restart Home Assistant
```
Developer Tools → YAML → Check Configuration → Restart
```

---

## MQTT Topics Reference

### Commands (publish to these)

| Topic | Payload | Description |
|-------|---------|-------------|
| `homeassistant/printer/cmd/text` | `{"text":"...","bold":false,"size":1,"align":"left","cut":true}` | Print plain text |
| `homeassistant/printer/cmd/shopping` | `{"title":"Shopping List","items":["Milk","Eggs"],"cut":true}` | Print shopping list |
| `homeassistant/printer/cmd/recipe` | `{"name":"...","servings":"4","time":"30m","ingredients":[...],"steps":[...],"cut":true}` | Print recipe |
| `homeassistant/printer/cmd/qrcode` | `{"data":"https://...","label":"Scan me","size":6,"cut":true}` | Print QR code |
| `homeassistant/printer/cmd/image` | `{"bitmap":[...],"width":384,"height":200,"cut":true}` | Print bitmap image |
| `homeassistant/printer/cmd/raw` | base64-encoded ESC/POS bytes | Raw printer commands |

### Status (subscribe to these)

| Topic | Values | Description |
|-------|--------|-------------|
| `homeassistant/printer/status` | `online` / `offline` / `printing` / `error` | Printer state |
| `homeassistant/printer/paper` | `ok` / `low` / `out` | Paper status (future) |

---

## Python Helper

For image printing and Mealie integration, use the `printer_helper.py` script.

### Install dependencies
```bash
pip install paho-mqtt Pillow requests
```

### Configure environment
```bash
export MQTT_HOST=192.168.1.x
export MQTT_USER=mqtt_user
export MQTT_PASS=mqtt_password
export MEALIE_HOST=http://192.168.1.x:9000
export MEALIE_TOKEN=your_mealie_api_token
export HA_HOST=http://192.168.1.x:8123
export HA_TOKEN=your_ha_long_lived_token
```

### Usage
```bash
# Print an image
python printer_helper.py image photo.jpg

# Print a Mealie recipe by slug
python printer_helper.py recipe chocolate-chip-cookies

# Print the HA shopping list
python printer_helper.py shopping

# Print a WiFi QR code
python printer_helper.py wifi "MyHomeWiFi" "password123"

# Print text
python printer_helper.py text "Hello from the terminal!"
```

### Run as a Home Assistant shell command
Add to `configuration.yaml`:
```yaml
shell_command:
  print_image: "python3 /config/printer_helper.py image {{ path }}"
  print_recipe: "python3 /config/printer_helper.py recipe {{ slug }}"
```

---

## OTA Updates

Two methods are available so you never need to physically touch the device again after the first USB flash.

### Method 1 — ArduinoOTA (push from your dev machine)

PlatformIO pushes the compiled binary directly to the ESP32 over WiFi using the `espota` protocol.

```bash
# Build and upload wirelessly — use the _ota environment defined in platformio.ini
pio run -e seeed_xiao_esp32c3_ota -t upload
```

- Set `upload_port` in `platformio.ini` to your device's IP (printed to serial on boot).
- Set `--auth` in `upload_flags` to match `OTA_PASSWORD` in `secrets.h`.
- The device appears as `esp32-printer` in Arduino IDE's network ports too.

### Method 2 — HTTP pull OTA (triggered via MQTT)

Host the compiled `.bin` anywhere on your local network (a simple Python HTTP server works fine), then trigger the update via MQTT. The ESP fetches and flashes itself, then reboots.

**Step 1 — Serve the binary:**
```bash
# In the project folder after building
cd .pio/build/seeed_xiao_esp32c3
python3 -m http.server 8080
```

**Step 2 — Publish the OTA command:**
```bash
# Via mosquitto_pub
mosquitto_pub -h 192.168.1.x -u mqtt_user -P mqtt_pass \
  -t homeassistant/printer/cmd/ota \
  -m '{"url":"http://192.168.1.YOUR_PC:8080/firmware.bin"}'

# Or via the HA Developer Tools → Services → mqtt.publish
```

**Step 3 — Watch progress:**
- `homeassistant/printer/status` → `"ota"` while flashing
- `homeassistant/printer/ota/progress` → `0` … `100`
- `homeassistant/printer/status` → `"online"` after reboot
- `homeassistant/printer/version` → updated version string

### Remote restart

If you ever need a clean reboot without a firmware change:
```bash
mosquitto_pub -h 192.168.1.x -u mqtt_user -P mqtt_pass \
  -t homeassistant/printer/cmd/restart -m "reboot"
```

---



```yaml
# Lovelace card
type: entities
entities:
  - entity: sensor.printer_status
    name: Printer Status
  - entity: sensor.printer_paper
    name: Paper Level
type: button
tap_action:
  action: call-service
  service: script.printer_shopping_list
name: "🛒 Print Shopping List"
icon: mdi:printer
```

---

## Extending the System

Adding a new print type is straightforward:

1. **ESP32 (main.cpp):** Add a new `#define MQTT_CMD_MYTYPE` topic, subscribe to it in `mqttReconnect()`, add a handler in `mqttCallback()`, write a `printMyType(JsonDocument&)` function.

2. **Home Assistant:** Add a new `script:` entry in `printer_integration.yaml` that publishes to the new topic.

3. **Python helper:** Add a `cmd_mytype()` function and a new subparser.

### Ideas for expansion
- 📅 Print today's calendar events (HA calendar integration)
- 🌤️ Print weather forecast
- 📦 Print a delivery tracking summary
- 🎫 Print event tickets / reminders
- 🏷️ Print labels with barcodes (EAN-13 via ESC/POS `GS k`)
- 🤖 Print AI-generated jokes or quotes via Claude API

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| No output, printer LED solid | Wrong baud rate | Check DIP switches, update `PRINTER_BAUD` |
| Garbled output | TX/RX crossed | Swap D6/D7 wiring |
| MQTT won't connect | Broker IP/credentials | Check `secrets.h`, test with MQTT Explorer |
| Image looks terrible | Contrast too low | Pass through Pillow's `autocontrast` in helper |
| QR code won't scan | Data too long or size too small | Reduce data length or increase `size` (max 8) |
| `status` stays offline | ESP32 not booting | Check USB CDC boot flag in `platformio.ini` |
