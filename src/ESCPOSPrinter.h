#pragma once
#include <HardwareSerial.h>

/**
 * Lightweight ESC/POS driver for the QR204 thermal printer.
 * Wraps the raw serial byte sequences into readable methods.
 */
class ESCPOSPrinter {
public:
  explicit ESCPOSPrinter(HardwareSerial& serial) : _s(serial) {}

  // ── Lifecycle ──────────────────────────────────────────────────────────────
  void begin() {
    delay(200);
    _s.write(0x1B); _s.write(0x40);  // ESC @ — initialize
    delay(100);
  }

  void reset() {
    _s.write(0x1B); _s.write(0x40);
  }

  // ── Text formatting ────────────────────────────────────────────────────────
  void setBold(bool on) {
    _s.write(0x1B); _s.write(0x45); _s.write(on ? 1 : 0);
  }

  void setUnderline(bool on) {
    _s.write(0x1B); _s.write(0x2D); _s.write(on ? 1 : 0);
  }

  // size: 1=normal, 2=double-width, 3=double-height, 4=double
  void setSize(int size) {
    uint8_t n = 0x00;
    if (size == 2) n = 0x10;       // double width
    else if (size == 3) n = 0x01;  // double height
    else if (size >= 4) n = 0x11;  // double width + height
    _s.write(0x1D); _s.write(0x21); _s.write(n);
  }

  // align: "left", "center", "right"
  void setAlign(const char* align) {
    uint8_t n = 0;
    if (strcmp(align, "center") == 0) n = 1;
    else if (strcmp(align, "right") == 0) n = 2;
    _s.write(0x1B); _s.write(0x61); _s.write(n);
  }

  // ── Output ─────────────────────────────────────────────────────────────────
  void print(const char* text) { _s.print(text); }
  void println(const char* text) { _s.print(text); _s.write(0x0A); }
  void print(String s) { _s.print(s); }
  void println(String s) { _s.print(s); _s.write(0x0A); }

  void feed(int lines = 1) {
    _s.write(0x1B); _s.write(0x64); _s.write((uint8_t)lines);
  }

  void cut(bool partial = true) {
    feed(4);
    _s.write(0x1D); _s.write(0x56); _s.write(partial ? 0x01 : 0x00);
  }

  void feedAndCut() { cut(); }

  // ── QR Code (ESC/POS 2D GS(k) commands) ───────────────────────────────────
  // size: module size 1–8, data: UTF-8 string
  void printQRCode(const char* data, uint8_t size = 6) {
    int len = strlen(data);

    // Set model (model 2)
    _s.write(0x1D); _s.write(0x28); _s.write(0x6B);
    _s.write(0x04); _s.write(0x00);
    _s.write(0x31); _s.write(0x41); _s.write(0x32); _s.write(0x00);

    // Set size
    _s.write(0x1D); _s.write(0x28); _s.write(0x6B);
    _s.write(0x03); _s.write(0x00);
    _s.write(0x31); _s.write(0x43); _s.write(size);

    // Set error correction level M
    _s.write(0x1D); _s.write(0x28); _s.write(0x6B);
    _s.write(0x03); _s.write(0x00);
    _s.write(0x31); _s.write(0x45); _s.write(0x31);

    // Store data
    uint16_t pLen = len + 3;
    _s.write(0x1D); _s.write(0x28); _s.write(0x6B);
    _s.write((uint8_t)(pLen & 0xFF)); _s.write((uint8_t)(pLen >> 8));
    _s.write(0x31); _s.write(0x50); _s.write(0x30);
    _s.print(data);

    // Print
    _s.write(0x1D); _s.write(0x28); _s.write(0x6B);
    _s.write(0x03); _s.write(0x00);
    _s.write(0x31); _s.write(0x51); _s.write(0x30);

    delay(200 + len * 5);
  }

  // ── Bitmap (ESC/POS raster) ────────────────────────────────────────────────
  // buf: 1-bit packed, MSB first, width pixels wide, height lines tall
  // width must be <= 384 (48 bytes) for 58mm paper
  void printBitmap(int width, int height, const uint8_t* buf) {
    int byteWidth = (width + 7) / 8;
    for (int y = 0; y < height; y++) {
      _s.write(0x1B); _s.write(0x2A);  // ESC *
      _s.write(0x00);                  // mode 0 = 8-dot single density
      _s.write((uint8_t)(width & 0xFF));
      _s.write((uint8_t)(width >> 8));
      _s.write(buf + y * byteWidth, byteWidth);
      _s.write(0x0A);
      delayMicroseconds(1200);
    }
  }

private:
  HardwareSerial& _s;
};
