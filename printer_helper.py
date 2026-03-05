#!/usr/bin/env python3
"""
printer_helper.py — Run on your Home Assistant host or any always-on machine.

Handles:
  1. Image → ESC/POS bitmap conversion and MQTT publishing
  2. Mealie recipe fetching and MQTT publishing
  3. CLI for quick testing

Requirements:
  pip install paho-mqtt Pillow requests

Usage:
  python printer_helper.py image    path/to/photo.jpg
  python printer_helper.py recipe   my-recipe-slug
  python printer_helper.py shopping
  python printer_helper.py wifi     "MySSID" "MyPassword"
  python printer_helper.py text     "Hello World"
"""

import argparse
import base64
import json
import os
import struct
import sys

import paho.mqtt.client as mqtt
import requests
from PIL import Image, ImageOps

# ── Config ────────────────────────────────────────────────────────────────────
MQTT_HOST   = os.getenv("MQTT_HOST",  "192.168.1.x")
MQTT_PORT   = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER   = os.getenv("MQTT_USER",  "mqtt_user")
MQTT_PASS   = os.getenv("MQTT_PASS",  "mqtt_password")

MEALIE_HOST  = os.getenv("MEALIE_HOST",  "http://192.168.1.x:9000")
MEALIE_TOKEN = os.getenv("MEALIE_TOKEN", "YOUR_MEALIE_API_TOKEN")

HA_HOST  = os.getenv("HA_HOST",  "http://192.168.1.x:8123")
HA_TOKEN = os.getenv("HA_TOKEN", "YOUR_HA_LONG_LIVED_TOKEN")

PRINTER_WIDTH = 384  # pixels — 58mm paper at 203 DPI

# ── MQTT publish helper ───────────────────────────────────────────────────────
def publish(topic: str, payload: dict | str):
    client = mqtt.Client()
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.connect(MQTT_HOST, MQTT_PORT, 60)
    data = json.dumps(payload) if isinstance(payload, dict) else payload
    result = client.publish(topic, data)
    result.wait_for_publish()
    client.disconnect()
    print(f"✓ Published to {topic}")

# ── Image processing ──────────────────────────────────────────────────────────
def image_to_bitmap(path: str) -> tuple[list[int], int, int]:
    """Convert an image file to ESC/POS 1-bit raster bytes."""
    img = Image.open(path).convert("RGB")

    # Scale to printer width, preserve aspect ratio
    w, h = img.size
    new_h = int(h * PRINTER_WIDTH / w)
    img = img.resize((PRINTER_WIDTH, new_h), Image.LANCZOS)

    # Dither to 1-bit
    img = img.convert("L")               # greyscale
    img = ImageOps.autocontrast(img)     # normalise contrast
    img = img.convert("1", dither=Image.Dither.FLOYDSTEINBERG)

    byte_width = (PRINTER_WIDTH + 7) // 8
    bitmap = []
    for y in range(new_h):
        for bx in range(byte_width):
            byte = 0
            for bit in range(8):
                px = bx * 8 + bit
                if px < PRINTER_WIDTH:
                    # PIL 1-bit: 0=black, 255=white; ESC/POS: 1=black
                    pixel = img.getpixel((px, y))
                    if pixel == 0:
                        byte |= (0x80 >> bit)
            bitmap.append(byte)

    return bitmap, PRINTER_WIDTH, new_h


def cmd_image(path: str):
    print(f"Processing image: {path}")
    bitmap, w, h = image_to_bitmap(path)
    print(f"  → {w}×{h}px, {len(bitmap)} bytes")
    publish("homeassistant/printer/cmd/image", {
        "bitmap": bitmap,
        "width":  w,
        "height": h,
        "cut":    True,
    })

# ── Mealie recipe ─────────────────────────────────────────────────────────────
def cmd_recipe(slug: str):
    url = f"{MEALIE_HOST}/api/recipes/{slug}"
    headers = {"Authorization": f"Bearer {MEALIE_TOKEN}"}
    resp = requests.get(url, headers=headers, timeout=10)
    resp.raise_for_status()
    data = resp.json()

    name     = data.get("name", slug)
    servings = str(data.get("recipeYield", ""))
    time_str = ""
    total_time = data.get("totalTime")
    if total_time:
        # ISO 8601 duration e.g. PT30M → simplify
        time_str = total_time.replace("PT", "").replace("H", "h ").replace("M", "m").strip()

    ingredients = [
        i.get("display") or f"{i.get('quantity','')} {i.get('unit',{}).get('name','')} {i.get('food',{}).get('name','')}".strip()
        for i in data.get("recipeIngredient", [])
    ]

    steps = [
        s.get("text", "")
        for s in data.get("recipeInstructions", [])
    ]

    publish("homeassistant/printer/cmd/recipe", {
        "name":        name,
        "servings":    servings,
        "time":        time_str,
        "ingredients": ingredients,
        "steps":       steps,
        "cut":         True,
    })

# ── Shopping list via HA REST API ─────────────────────────────────────────────
def cmd_shopping():
    url = f"{HA_HOST}/api/states/todo.shopping_list"
    headers = {
        "Authorization": f"Bearer {HA_TOKEN}",
        "Content-Type": "application/json",
    }
    resp = requests.get(url, headers=headers, timeout=10)
    resp.raise_for_status()
    data = resp.json()
    items_raw = data.get("attributes", {}).get("items", [])
    items = [i["summary"] for i in items_raw if i.get("status") != "completed"]

    if not items:
        print("Shopping list is empty.")
        return

    publish("homeassistant/printer/cmd/shopping", {
        "title": "Shopping List",
        "items": items,
        "cut":   True,
    })

# ── WiFi QR ───────────────────────────────────────────────────────────────────
def cmd_wifi(ssid: str, password: str, security: str = "WPA"):
    publish("homeassistant/printer/cmd/qrcode", {
        "data":  f"WIFI:T:{security};S:{ssid};P:{password};;",
        "label": f"Join {ssid}",
        "size":  7,
        "cut":   True,
    })

# ── Plain text ────────────────────────────────────────────────────────────────
def cmd_text(text: str):
    publish("homeassistant/printer/cmd/text", {
        "text": text,
        "cut":  True,
    })

# ── CLI ───────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="QR204 Printer Helper")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_img = sub.add_parser("image",    help="Print an image file")
    p_img.add_argument("path")

    p_rec = sub.add_parser("recipe",   help="Print a Mealie recipe by slug")
    p_rec.add_argument("slug")

    sub.add_parser("shopping", help="Print the HA shopping list")

    p_wifi = sub.add_parser("wifi",    help="Print a WiFi QR code")
    p_wifi.add_argument("ssid")
    p_wifi.add_argument("password")
    p_wifi.add_argument("--security", default="WPA")

    p_txt = sub.add_parser("text",     help="Print plain text")
    p_txt.add_argument("text")

    args = parser.parse_args()

    if   args.cmd == "image":    cmd_image(args.path)
    elif args.cmd == "recipe":   cmd_recipe(args.slug)
    elif args.cmd == "shopping": cmd_shopping()
    elif args.cmd == "wifi":     cmd_wifi(args.ssid, args.password, args.security)
    elif args.cmd == "text":     cmd_text(args.text)


if __name__ == "__main__":
    main()
