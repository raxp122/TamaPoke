#pragma once
// Touch gesture classification (pure logic, covered by tests/gesture_tests.cpp).
#include <stdint.h>
#include <stdlib.h>

enum GestureKind : uint8_t { G_NONE, G_TAP, G_SWIPE_LEFT, G_SWIPE_RIGHT, G_SWIPE_UP, G_SWIPE_DOWN };

#define GESTURE_TAP_SLOP 50        // px: below this the finger did not really move -> tap
#define GESTURE_SWIPE_MIN 70       // px: normal swipe distance
#define GESTURE_FLICK_MIN 50       // px: a quick flick counts from here
#define GESTURE_FLICK_MS 300
#define GESTURE_SWIPE_MAX_MS 1200  // slow swipes are fine up to here
#define GESTURE_TAP_MAX_MS 1500
#define GESTURE_RELEASE_MS 60      // finger lifted only after this long without contact

// dx/dy: last position minus first position; dtMs: finger down time.
inline GestureKind classifyGesture(int dx, int dy, uint32_t dtMs) {
  int ax = abs(dx), ay = abs(dy);
  int major = ax > ay ? ax : ay;
  int minor = ax > ay ? ay : ax;
  if (major < GESTURE_TAP_SLOP) return dtMs <= GESTURE_TAP_MAX_MS ? G_TAP : G_NONE;
  bool clearAxis = minor * 10 <= major * 6;  // the movement goes mostly one way
  bool swipe = clearAxis && ((major >= GESTURE_SWIPE_MIN && dtMs <= GESTURE_SWIPE_MAX_MS) ||
                             (major >= GESTURE_FLICK_MIN && dtMs <= GESTURE_FLICK_MS));
  if (swipe) {
    if (ax > ay) return dx > 0 ? G_SWIPE_RIGHT : G_SWIPE_LEFT;
    return dy > 0 ? G_SWIPE_DOWN : G_SWIPE_UP;
  }
  // a short, slow slide on a button is still meant as a tap
  if (major < GESTURE_SWIPE_MIN && dtMs <= GESTURE_TAP_MAX_MS) return G_TAP;
  return G_NONE;
}
