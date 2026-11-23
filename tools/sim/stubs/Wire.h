#pragma once
#include <Arduino.h>
#include <string>
// I2C stand-in. Only a fake LC76G GNSS (0x50 write / 0x54 read, Waveshare protocol)
// answers when gSimGpsFix >= 0; every other address NACKs.
extern int gSimGpsFix;  // -1: no module, 0: searching, 1: fix
std::string simGpsNmea();
class TwoWire {
 public:
  bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
  void setTimeOut(uint16_t) {}
  void setTimeout(unsigned long) {}
  void setClock(uint32_t) {}
  size_t setBufferSize(size_t n) { return n; }
  void beginTransmission(uint16_t a) { addr = a; tx.clear(); }
  size_t write(uint8_t b) { tx.push_back((char)b); return 1; }
  size_t write(const uint8_t *d, size_t n) { tx.append((const char *)d, n); return n; }
  uint8_t endTransmission(bool = true) {
    if (addr != 0x50 || gSimGpsFix < 0) return 2;  // address NACK
    if (tx.size() == 8 && (uint8_t)tx[0] == 0x08) { pending = simGpsNmea(); mode = 1; }  // length request
    else if (tx.size() == 8 && (uint8_t)tx[1] == 0x20) mode = 2;                          // data request
    return 0;
  }
  size_t requestFrom(uint16_t a, size_t n) {
    rx.clear(); pos = 0;
    if (a != 0x54 || gSimGpsFix < 0) return 0;
    if (mode == 1 && n == 4) {
      uint32_t l = pending.size();
      for (int i = 0; i < 4; i++) rx.push_back((char)((l >> (8 * i)) & 0xFF));
    } else if (mode == 2) {
      rx = pending.substr(0, n);
    }
    mode = 0;
    return rx.size();
  }
  uint8_t requestFrom(uint16_t a, uint8_t n) { return (uint8_t)requestFrom(a, (size_t)n); }
  int available() { return (int)(rx.size() - pos); }
  int read() { return pos < rx.size() ? (uint8_t)rx[pos++] : -1; }

 private:
  uint16_t addr = 0;
  std::string tx, rx, pending;
  size_t pos = 0;
  int mode = 0;
};
extern TwoWire Wire;
