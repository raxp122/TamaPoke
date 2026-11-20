#pragma once
#include <Arduino.h>
class TwoWire {
 public:
  bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
  void setTimeOut(uint16_t) {}
  void setClock(uint32_t) {}
};
extern TwoWire Wire;
