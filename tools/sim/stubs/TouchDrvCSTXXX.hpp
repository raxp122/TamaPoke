#pragma once
// Touch controller stand-in: the simulator sets the finger position.
#include <Arduino.h>
#include <Wire.h>

struct SimTouchState {
  bool down = false;
  int16_t x = 0, y = 0;
};
extern SimTouchState gSimTouch;

class TouchDrvCST92xx {
 public:
  void setPins(int, int) {}
  bool begin(TwoWire &, uint8_t, int, int) { return true; }
  void reset() {}
  void setMaxCoordinates(int, int) {}
  void setMirrorXY(bool, bool) {}
  uint8_t getPoint(int16_t *x, int16_t *y, uint8_t) {
    if (!gSimTouch.down) return 0;
    *x = gSimTouch.x;
    *y = gSimTouch.y;
    return 1;
  }
};
