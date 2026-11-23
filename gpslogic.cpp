#include "gpslogic.h"
#include <stdlib.h>
#include <string.h>

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool nmeaChecksumOk(const char *s) {
  if (!s || s[0] != '$') return false;
  uint8_t sum = 0;
  const char *p = s + 1;
  while (*p && *p != '*') sum ^= (uint8_t)*p++;
  if (*p != '*') return false;
  int h = hexVal(p[1]), l = hexVal(p[2]);
  return h >= 0 && l >= 0 && ((h << 4) | l) == sum;
}

double nmeaCoord(const char *v, const char *hemi) {
  if (!v || !*v) return 0;
  double raw = strtod(v, nullptr);
  int deg = (int)(raw / 100);
  double d = deg + (raw - deg * 100) / 60.0;
  if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
  return d;
}

// splits "a,b,,c" in place; returns number of fields
static int splitFields(char *s, char **f, int max) {
  int n = 0;
  f[n++] = s;
  for (char *p = s; *p && n < max; p++) {
    if (*p == ',') {
      *p = 0;
      f[n++] = p + 1;
    }
  }
  return n;
}

static void parseTime(const char *t, GpsInfo &g) {
  if (strlen(t) < 6) return;
  g.hh = (t[0] - '0') * 10 + (t[1] - '0');
  g.mm = (t[2] - '0') * 10 + (t[3] - '0');
  g.ss = (t[4] - '0') * 10 + (t[5] - '0');
  g.timeValid = g.hh < 24 && g.mm < 60 && g.ss < 61;
}

void GpsParser::line(char *s) {
  if (!nmeaChecksumOk(s)) {
    gi.badChecksums++;
    return;
  }
  *strchr(s, '*') = 0;
  char *f[24];
  int n = splitFields(s + 1, f, 24);
  if (strlen(f[0]) < 5) return;
  const char *type = f[0] + 2;  // GPRMC, GNRMC, GLRMC...: ignore the talker
  gi.sentences++;
  if (!strcmp(type, "RMC") && n >= 10) {
    parseTime(f[1], gi);
    bool valid = f[2][0] == 'A';
    if (valid && f[3][0] && f[5][0]) {
      gi.lat = nmeaCoord(f[3], f[4]);
      gi.lon = nmeaCoord(f[5], f[6]);
    }
    gi.fix = valid;
    if (strlen(f[9]) >= 6) {
      gi.day = (f[9][0] - '0') * 10 + (f[9][1] - '0');
      gi.mon = (f[9][2] - '0') * 10 + (f[9][3] - '0');
      gi.year = 2000 + (f[9][4] - '0') * 10 + (f[9][5] - '0');
      gi.dateValid = gi.day >= 1 && gi.day <= 31 && gi.mon >= 1 && gi.mon <= 12;
    }
  } else if (!strcmp(type, "GGA") && n >= 10) {
    parseTime(f[1], gi);
    int q = atoi(f[6]);
    gi.satsUsed = (uint8_t)atoi(f[7]);
    gi.hdop = (float)atof(f[8]);
    if (q > 0 && f[2][0] && f[4][0]) {
      gi.lat = nmeaCoord(f[2], f[3]);
      gi.lon = nmeaCoord(f[4], f[5]);
      gi.altM = (float)atof(f[9]);
      gi.fix = true;
    } else if (q == 0) {
      gi.fix = false;
    }
  } else if (!strcmp(type, "GSV") && n >= 4) {
    int inView = atoi(f[3]);
    if (atoi(f[2]) == 1) gi.satsView = 0;  // first message of a cycle
    if (inView > gi.satsView) gi.satsView = (uint8_t)(inView > 255 ? 255 : inView);
  }
}

void GpsParser::feed(const char *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = data[i];
    if (c == '$') {
      inLine = true;
      n = 0;
    }
    if (!inLine) continue;
    if (c == '\r' || c == '\n' || c == 0) {
      if (n) {
        buf[n] = 0;
        line(buf);
      }
      inLine = false;
      n = 0;
      continue;
    }
    if (n < sizeof(buf) - 1) buf[n++] = c;
    else { inLine = false; n = 0; }  // too long: drop it
  }
}

uint32_t GpsParser::utcEpoch() const {
  if (!gi.dateValid || !gi.timeValid) return 0;
  // days from civil (Hinnant)
  int y = gi.year, m = gi.mon, d = gi.day;
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (uint32_t)(days * 86400L + gi.hh * 3600L + gi.mm * 60L + gi.ss);
}
