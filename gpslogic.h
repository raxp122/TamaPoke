#pragma once
// NMEA parser for the LC76G GNSS module of the -G board (pure logic, covered by
// tests/gps_tests.cpp). Feed it the text the module sends; it keeps the latest
// position, satellites and UTC time.
#include <stddef.h>
#include <stdint.h>

struct GpsInfo {
  bool fix = false;          // valid position (RMC status A or GGA quality > 0)
  double lat = 0, lon = 0;   // degrees, south/west negative
  float altM = 0;            // metres above sea level (GGA)
  float hdop = 0;
  uint8_t satsUsed = 0;      // GGA
  uint8_t satsView = 0;      // GSV (largest count seen in the last cycle)
  bool timeValid = false;
  uint8_t hh = 0, mm = 0, ss = 0;
  bool dateValid = false;
  uint8_t day = 0, mon = 0;
  uint16_t year = 0;
  uint32_t sentences = 0;    // valid sentences parsed (module alive)
  uint32_t badChecksums = 0;
};

class GpsParser {
 public:
  void feed(const char *data, size_t len);
  const GpsInfo &info() const { return gi; }
  // UTC epoch from the last RMC date+time, 0 when unknown
  uint32_t utcEpoch() const;

 private:
  void line(char *s);
  GpsInfo gi;
  char buf[120];
  uint8_t n = 0;
  bool inLine = false;
};

// helpers (exposed for the tests)
bool nmeaChecksumOk(const char *sentence);           // "$....*HH"
double nmeaCoord(const char *value, const char *hemi);  // "4807.038","N" -> 48.1173
