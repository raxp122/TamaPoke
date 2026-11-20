#include <iostream>

#include "gesture.h"

static int failures = 0;
#define EXPECT_EQ(actual, expected) do { \
  auto a = (actual); auto e = (expected); \
  if (!(a == e)) { std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": " << #actual << " was " << +a << ", expected " << +e << "\n"; failures++; } \
} while (0)

int main() {
  // taps, also with a shaky finger
  EXPECT_EQ(classifyGesture(0, 0, 80), G_TAP);
  EXPECT_EQ(classifyGesture(30, -35, 200), G_TAP);
  EXPECT_EQ(classifyGesture(-49, 20, 400), G_TAP);
  // 50-69 px slow slide: before this was lost (neither tap nor swipe), now a tap
  EXPECT_EQ(classifyGesture(60, 10, 700), G_TAP);
  EXPECT_EQ(classifyGesture(0, 65, 600), G_TAP);
  // held too long: nothing (the long press is handled separately)
  EXPECT_EQ(classifyGesture(5, 5, 2000), G_NONE);
  // swipes in the four directions
  EXPECT_EQ(classifyGesture(150, 10, 250), G_SWIPE_RIGHT);
  EXPECT_EQ(classifyGesture(-150, 30, 250), G_SWIPE_LEFT);
  EXPECT_EQ(classifyGesture(20, 140, 250), G_SWIPE_DOWN);
  EXPECT_EQ(classifyGesture(-10, -140, 250), G_SWIPE_UP);
  // a bit diagonal but clearly horizontal (before: rejected when dy >= 70)
  EXPECT_EQ(classifyGesture(200, 90, 300), G_SWIPE_RIGHT);
  // slow swipe (before: rejected above 800 ms)
  EXPECT_EQ(classifyGesture(-180, 15, 1100), G_SWIPE_LEFT);
  // quick short flick
  EXPECT_EQ(classifyGesture(55, 5, 150), G_SWIPE_RIGHT);
  // pure diagonal: ambiguous, ignored
  EXPECT_EQ(classifyGesture(120, 110, 250), G_NONE);
  // very slow long drag: ignored
  EXPECT_EQ(classifyGesture(200, 0, 3000), G_NONE);
  if (failures) { std::cerr << failures << " gesture test(s) failed\n"; return 1; }
  std::cout << "gesture tests OK\n";
  return 0;
}
