#pragma once
// Écran simulé : framebuffer 1 bit, fenêtres partielles alignées sur 8 px et
// effacées par firstPage(), comme GxEPD2.
#include <Adafruit_GFX.h>
#define GxEPD_BLACK 0x0000
#define GxEPD_WHITE 0xFFFF
struct GxEPD2_420 { static const int16_t WIDTH = 400, HEIGHT = 300; GxEPD2_420(int, int, int, int) {} };
template <class D, int H>
class GxEPD2_BW : public Adafruit_GFX {
 public:
  uint8_t fb[D::WIDTH * D::HEIGHT];
  int16_t wx = 0, wy = 0, ww = D::WIDTH, wh = D::HEIGHT;
  GxEPD2_BW(D) : Adafruit_GFX(D::WIDTH, D::HEIGHT) { memset(fb, 1, sizeof fb); }
  void init(int) {}
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < wx || y < wy || x >= wx + ww || y >= wy + wh) return;
    if (x < 0 || y < 0 || x >= width() || y >= height()) return;
    fb[y * D::WIDTH + x] = (c == GxEPD_WHITE) ? 1 : 0;
  }
  void setFullWindow() { wx = 0; wy = 0; ww = D::WIDTH; wh = D::HEIGHT; }
  void setPartialWindow(int16_t x, int16_t y, int16_t w, int16_t h) {
    int16_t x1 = x & ~7, x2 = (x + w + 7) & ~7;
    wx = x1; wy = y; ww = x2 - x1; wh = h;
  }
  void powerOff() {}
  void firstPage() { fillScreen(GxEPD_WHITE); }
  bool nextPage() { return false; }
};
