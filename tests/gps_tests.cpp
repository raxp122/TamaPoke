#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

#include "gpslogic.h"

static int failures = 0;
#define EXPECT_TRUE(expr) do { if (!(expr)) { std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": " << #expr << "\n"; failures++; } } while (0)
#define EXPECT_NEAR(a, b, eps) EXPECT_TRUE(std::fabs((double)(a) - (double)(b)) < (eps))

// appends "*HH\r\n" with the right checksum
static std::string nmea(const std::string &body) {
  unsigned char sum = 0;
  for (char c : body) sum ^= (unsigned char)c;
  char tail[8];
  snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
  return "$" + body + tail;
}

int main() {
  EXPECT_TRUE(nmeaChecksumOk("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
  EXPECT_TRUE(!nmeaChecksumOk("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48"));
  EXPECT_NEAR(nmeaCoord("4807.038", "N"), 48.1173, 1e-4);
  EXPECT_NEAR(nmeaCoord("01131.000", "W"), -11.516667, 1e-5);

  // no fix yet: satellites in view, time known, no position
  {
    GpsParser p;
    std::string s = nmea("GNGGA,101530.000,,,,,0,00,99.0,,M,,M,,") +
                    nmea("GPGSV,2,1,07,01,40,083,,02,17,308,,03,13,172,,04,09,034,") +
                    nmea("GPGSV,2,2,07,05,40,083,,06,17,308,,07,13,172,") +
                    nmea("GNRMC,101530.000,V,,,,,,,231126,,,N");
    p.feed(s.data(), s.size());
    const GpsInfo &g = p.info();
    EXPECT_TRUE(!g.fix);
    EXPECT_TRUE(g.satsView == 7);
    EXPECT_TRUE(g.timeValid && g.hh == 10 && g.mm == 15 && g.ss == 30);
    EXPECT_TRUE(g.dateValid && g.day == 23 && g.mon == 11 && g.year == 2026);
    EXPECT_TRUE(p.utcEpoch() == 1795428930u);  // 2026-11-23 10:15:30 UTC
    EXPECT_TRUE(g.sentences == 4);
  }
  // fix in Milan, fed in odd chunks with noise between sentences
  {
    GpsParser p;
    std::string s = "garbage" + nmea("GNGGA,101531.000,4527.9420,N,00911.4100,E,1,09,0.8,122.5,M,47.0,M,,") +
                    "\r\n" + nmea("GNRMC,101531.000,A,4527.9420,N,00911.4100,E,0.10,0.00,231126,,,A");
    for (size_t i = 0; i < s.size(); i += 5) p.feed(s.data() + i, std::min<size_t>(5, s.size() - i));
    const GpsInfo &g = p.info();
    EXPECT_TRUE(g.fix);
    EXPECT_NEAR(g.lat, 45.4657, 1e-3);
    EXPECT_NEAR(g.lon, 9.1902, 1e-3);
    EXPECT_NEAR(g.altM, 122.5, 1e-3);
    EXPECT_TRUE(g.satsUsed == 9);
    EXPECT_NEAR(g.hdop, 0.8, 1e-3);
  }
  // southern / western hemisphere, corrupted sentence ignored
  {
    GpsParser p;
    std::string bad = nmea("GNRMC,120000.000,A,3352.0000,S,15112.0000,E,0,0,010126,,,A");
    bad[20] = 'X';  // breaks the checksum
    std::string s = bad + nmea("GPRMC,120000.000,A,3352.0000,S,07000.0000,W,0,0,010126,,,A");
    p.feed(s.data(), s.size());
    EXPECT_TRUE(p.info().badChecksums == 1);
    EXPECT_NEAR(p.info().lat, -33.8667, 1e-3);
    EXPECT_NEAR(p.info().lon, -70.0, 1e-3);
  }
  if (failures) { std::cerr << failures << " gps test(s) failed\n"; return 1; }
  std::cout << "gps tests OK\n";
  return 0;
}
