#pragma once
// Host canvas with the subset of Arduino_GFX used by the sketch. Drawing follows
// the Adafruit-GFX algorithms closely enough for screenshots and layout checks.
// The classic 5x7 font comes from the installed Arduino_GFX library (see Makefile).
#include <Arduino.h>
#include <vector>

#include "font/glcdfont.h"

class Arduino_DataBus {
 public:
  virtual ~Arduino_DataBus() {}
};

class Arduino_ESP32QSPI : public Arduino_DataBus {
 public:
  template <typename... A>
  Arduino_ESP32QSPI(A...) {}
};

class Arduino_CO5300 {
 public:
  template <typename... A>
  Arduino_CO5300(A...) {}
  void setBrightness(uint8_t b) { brightness = b; }
  uint8_t brightness = 255;
};

// One recorded shape: used by the simulator to find elements outside the round panel.
struct SimShape {
  char kind;  // 'R' rect, 'r' round rect, 'C' circle, 'T' text
  int16_t x, y, w, h, r;
  bool filled;
  char text[48];
};

class Arduino_Canvas : public Print {
 public:
  Arduino_Canvas(int16_t w, int16_t h, Arduino_CO5300 *) : W(w), H(h), fb(w * h, 0) {}
  bool begin(int32_t = 0) { return true; }
  uint16_t *getFramebuffer() { return fb.data(); }
  void flush();  // implemented by the simulator (copies the frame)
  int16_t width() const { return W; }
  int16_t height() const { return H; }

  // recording (cleared by fillScreen, read by the simulator)
  std::vector<SimShape> shapes;
  bool recording = true;

  void drawPixel(int16_t x, int16_t y, uint16_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    fb[y * W + x] = c;
  }
  void fillScreen(uint16_t c) {
    std::fill(fb.begin(), fb.end(), c);
    shapes.clear();
  }
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) {
    if (y < 0 || y >= H || w <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > W) w = W - x;
    for (int16_t i = 0; i < w; i++) fb[y * W + x + i] = c;
  }
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) {
    for (int16_t i = 0; i < h; i++) drawPixel(x, y + i, c);
  }
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    rec('R', x, y, w, h, 0, true);
    for (int16_t j = 0; j < h; j++) drawFastHLine(x, y + j, w, c);
  }
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    rec('R', x, y, w, h, 0, false);
    drawFastHLine(x, y, w, c);
    drawFastHLine(x, y + h - 1, w, c);
    drawFastVLine(x, y, h, c);
    drawFastVLine(x + w - 1, y, h, c);
  }
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) {
    bool steep = abs(y1 - y0) > abs(x1 - x0);
    if (steep) { std::swap(x0, y0); std::swap(x1, y1); }
    if (x0 > x1) { std::swap(x0, x1); std::swap(y0, y1); }
    int16_t dx = x1 - x0, dy = abs(y1 - y0), err = dx / 2, ystep = y0 < y1 ? 1 : -1;
    for (; x0 <= x1; x0++) {
      if (steep) drawPixel(y0, x0, c); else drawPixel(x0, y0, c);
      err -= dy;
      if (err < 0) { y0 += ystep; err += dx; }
    }
  }
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t c) {
    rec('C', x0 - r, y0 - r, 2 * r + 1, 2 * r + 1, r, false);
    int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
    drawPixel(x0, y0 + r, c); drawPixel(x0, y0 - r, c); drawPixel(x0 + r, y0, c); drawPixel(x0 - r, y0, c);
    while (x < y) {
      if (f >= 0) { y--; ddy += 2; f += ddy; }
      x++; ddx += 2; f += ddx;
      drawPixel(x0 + x, y0 + y, c); drawPixel(x0 - x, y0 + y, c); drawPixel(x0 + x, y0 - y, c); drawPixel(x0 - x, y0 - y, c);
      drawPixel(x0 + y, y0 + x, c); drawPixel(x0 - y, y0 + x, c); drawPixel(x0 + y, y0 - x, c); drawPixel(x0 - y, y0 - x, c);
    }
  }
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t c) {
    rec('C', x0 - r, y0 - r, 2 * r + 1, 2 * r + 1, r, true);
    for (int16_t dy = -r; dy <= r; dy++) {
      int16_t dx = (int16_t)std::sqrt((double)r * r - (double)dy * dy + 0.5);
      drawFastHLine(x0 - dx, y0 + dy, 2 * dx + 1, c);
    }
  }
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) {
    rec('r', x, y, w, h, r, false);
    roundRect(x, y, w, h, r, c, false);
  }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) {
    rec('r', x, y, w, h, r, true);
    roundRect(x, y, w, h, r, c, true);
  }
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t c) {
    int16_t minY = std::min({ y0, y1, y2 }), maxY = std::max({ y0, y1, y2 });
    for (int16_t y = minY; y <= maxY; y++) {
      double xs[3];
      int n = 0;
      int16_t px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
      for (int e = 0; e < 3; e++) {
        int a = e, b = (e + 1) % 3;
        if (py[a] == py[b]) continue;
        if ((y >= std::min(py[a], py[b])) && (y <= std::max(py[a], py[b])) && n < 3)
          xs[n++] = px[a] + (double)(y - py[a]) * (px[b] - px[a]) / (py[b] - py[a]);
      }
      if (!n) continue;
      double lo = xs[0], hi = xs[0];
      for (int i = 1; i < n; i++) { lo = std::min(lo, xs[i]); hi = std::max(hi, xs[i]); }
      drawFastHLine((int16_t)std::lround(lo), y, (int16_t)std::lround(hi - lo) + 1, c);
    }
  }

  // ---- text (classic 6x8 cells, scaled) ----
  void setTextSize(uint8_t s) { tsx = tsy = s ? s : 1; }
  void setTextSize(uint8_t sx, uint8_t sy) { tsx = sx ? sx : 1; tsy = sy ? sy : 1; }
  void setTextColor(uint16_t c) { fg = bg = c; }
  void setTextColor(uint16_t c, uint16_t b) { fg = c; bg = b; }
  void setCursor(int16_t x, int16_t y) { cx = x; cy = y; textStartX = x; textStartY = y; textLen = 0; }
  void setTextWrap(bool w) { wrap = w; }
  int16_t getCursorX() const { return cx; }
  int16_t getCursorY() const { return cy; }

  size_t write(uint8_t c) override {
    if (c == '\n') { cx = 0; cy += tsy * 8; return 1; }
    if (c == '\r') return 1;
    if (wrap && cx + tsx * 6 > W) { cx = 0; cy += tsy * 8; }
    drawChar(cx, cy, c);
    if (recording) {
      // one text shape per print run; extended while the cursor moves on the same line
      if (textLen && !shapes.empty() && shapes.back().kind == 'T' && shapes.back().y == cy &&
          shapes.back().x + shapes.back().w == cx) {
        SimShape &s = shapes.back();
        s.w += tsx * 6;
        size_t l = strlen(s.text);
        if (l + 1 < sizeof(s.text)) { s.text[l] = (char)c; s.text[l + 1] = 0; }
      } else {
        SimShape s = { 'T', cx, cy, (int16_t)(tsx * 6), (int16_t)(tsy * 8), 0, true, { (char)c, 0 } };
        shapes.push_back(s);
      }
      textLen++;
    }
    cx += tsx * 6;
    return 1;
  }
  using Print::write;

 private:
  void rec(char k, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, bool filled) {
    if (!recording) return;
    SimShape s = { k, x, y, w, h, r, filled, { 0 } };
    shapes.push_back(s);
  }
  bool inRound(int16_t x, int16_t y, int16_t rx, int16_t ry, int16_t w, int16_t h, int16_t r) const {
    // is (x,y) inside the rounded rectangle?
    int16_t ix = x < rx + r ? rx + r : (x > rx + w - 1 - r ? rx + w - 1 - r : x);
    int16_t iy = y < ry + r ? ry + r : (y > ry + h - 1 - r ? ry + h - 1 - r : y);
    long dx = x - ix, dy = y - iy;
    return dx * dx + dy * dy <= (long)r * r;
  }
  void roundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c, bool fill) {
    int16_t m = std::min(w, h) / 2;
    if (r > m) r = m;
    if (r < 0) r = 0;
    for (int16_t j = 0; j < h; j++) {
      for (int16_t i = 0; i < w; i++) {
        int16_t px = x + i, py = y + j;
        if (!inRound(px, py, x, y, w, h, r)) continue;
        if (!fill) {
          // border: inside, but a 4-neighbour is outside
          bool edge = i == 0 || j == 0 || i == w - 1 || j == h - 1 ||
                      !inRound(px - 1, py, x, y, w, h, r) || !inRound(px + 1, py, x, y, w, h, r) ||
                      !inRound(px, py - 1, x, y, w, h, r) || !inRound(px, py + 1, x, y, w, h, r);
          if (!edge) continue;
        }
        drawPixel(px, py, c);
      }
    }
  }
  void drawChar(int16_t x, int16_t y, uint8_t c) {
    if (c >= 176) c++;  // classic font quirk (no cp437)
    for (int8_t i = 0; i < 5; i++) {
      uint8_t line = font[c * 5 + i];
      for (int8_t j = 0; j < 8; j++, line >>= 1) {
        if (line & 1) {
          if (tsx == 1 && tsy == 1) drawPixel(x + i, y + j, fg);
          else for (int a = 0; a < tsx; a++) for (int b = 0; b < tsy; b++) drawPixel(x + i * tsx + a, y + j * tsy + b, fg);
        } else if (bg != fg) {
          for (int a = 0; a < tsx; a++) for (int b = 0; b < tsy; b++) drawPixel(x + i * tsx + a, y + j * tsy + b, bg);
        }
      }
    }
    if (bg != fg) for (int a = 0; a < tsx; a++) for (int b = 0; b < tsy * 8; b++) drawPixel(x + 5 * tsx + a, y + b, bg);
  }

  int16_t W, H;
  std::vector<uint16_t> fb;
  int16_t cx = 0, cy = 0, textStartX = 0, textStartY = 0;
  int textLen = 0;
  uint8_t tsx = 1, tsy = 1;
  uint16_t fg = 0xFFFF, bg = 0xFFFF;
  bool wrap = true;
};
