#include "gps.h"
#include <Wire.h>

#define GPS_ADDR_W 0x50
#define GPS_ADDR_R 0x54
#define TCA9554_ADDR 0x20
#define TCA_EXIO_GPS_RST 7      // -G board: GNSS reset line, high = running
#define GPS_POLL_MS 1000
#define GPS_STEP_MS 100         // the module needs a moment between request and read
#define GPS_MAX_READ 2000       // matches Wire.setBufferSize() in setup()
#define GPS_BOOT_WINDOW_MS (5UL * 60UL * 1000UL)

static bool present = false;
static bool fastBus = true;     // 400 kHz during GNSS transfers, 100 kHz if the module does not cope
static GpsParser parser;
static uint8_t step = 0;
static uint32_t t0 = 0, pending = 0, gen = 0;
static uint32_t lastLog = 0;
static bool loggedFix = false;

bool gpsPresent() { return present; }
const GpsInfo &gpsInfo() { return parser.info(); }
uint32_t gpsGen() { return gen; }
bool gpsBootWindow(uint32_t now) { return present && !loggedFix && now < GPS_BOOT_WINDOW_MS; }

static bool tcaRead(uint8_t reg, uint8_t *v) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint16_t)TCA9554_ADDR, (uint8_t)1) != 1) return false;
  *v = Wire.read();
  return true;
}

static bool tcaWrite(uint8_t reg, uint8_t v) {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(reg);
  Wire.write(v);
  return Wire.endTransmission() == 0;
}

void gpsBegin() {
  // TCA9554: only the GNSS reset becomes an output (high); every other pin keeps its state
  uint8_t out, cfg;
  if (tcaRead(0x01, &out) && tcaRead(0x03, &cfg)) {
    tcaWrite(0x01, out | (1 << TCA_EXIO_GPS_RST));
    tcaWrite(0x03, cfg & ~(1 << TCA_EXIO_GPS_RST));
  }
  delay(20);
  Wire.beginTransmission(GPS_ADDR_W);
  present = Wire.endTransmission() == 0;
  Serial.printf("GPS %s\n", present ? "LC76G found" : "not present");
}

static bool writeCmd(const uint8_t *data, size_t n) {
  Wire.beginTransmission(GPS_ADDR_W);
  Wire.write(data, n);
  return Wire.endTransmission() == 0;
}

static void logState(uint32_t now) {
  const GpsInfo &g = parser.info();
  if (g.fix && !loggedFix) {
    loggedFix = true;
    Serial.printf("GPS fix lat=%.6f lon=%.6f alt=%.1fm sats=%u hdop=%.1f\n", g.lat, g.lon, g.altM, g.satsUsed, g.hdop);
  } else if (!g.fix && now - lastLog > 10000) {
    lastLog = now;
    Serial.printf("GPS searching: %u satellites in view\n", g.satsView);
  }
}

void gpsPoll(uint32_t now, bool wanted) {
  if (!present) return;
  if (!wanted) { step = 0; return; }
  switch (step) {
    case 0: {  // ask how many bytes are waiting
      if (now - t0 < GPS_POLL_MS) return;
      static const uint8_t ask[] = { 0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 };
      if (fastBus) Wire.setClock(400000);
      bool ok = writeCmd(ask, sizeof(ask));
      if (fastBus) Wire.setClock(100000);
      t0 = now;
      if (ok) step = 1;
      return;
    }
    case 1: {  // read the length, then request the data
      if (now - t0 < GPS_STEP_MS) return;
      uint8_t b[4] = { 0 };
      if (Wire.requestFrom((uint16_t)GPS_ADDR_R, (uint8_t)4) != 4) { step = 0; t0 = now; return; }
      for (int i = 0; i < 4; i++) b[i] = Wire.read();
      pending = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
      if (pending == 0 || pending > 65536) { step = 0; t0 = now; return; }
      if (pending > GPS_MAX_READ) pending = GPS_MAX_READ;
      uint8_t req[8] = { 0x00, 0x20, 0x51, 0xAA, b[0], b[1], b[2], b[3] };
      if (!writeCmd(req, sizeof(req))) { step = 0; t0 = now; return; }
      t0 = now;
      step = 2;
      return;
    }
    default: {  // read the NMEA text
      if (now - t0 < GPS_STEP_MS) return;
      static char text[GPS_MAX_READ];
      if (fastBus) Wire.setClock(400000);
      size_t got = Wire.requestFrom((uint16_t)GPS_ADDR_R, (size_t)pending);
      if (fastBus) Wire.setClock(100000);
      if (got == 0 && fastBus) {
        fastBus = false;  // the module did not answer at 400 kHz: stay at 100 kHz
        Serial.println("GPS: using 100 kHz");
      }
      size_t n = 0;
      while (Wire.available() && n < sizeof(text)) text[n++] = (char)Wire.read();
      if (n) {
        parser.feed(text, n);
        gen++;
      }
      logState(now);
      step = 0;
      t0 = now;
      return;
    }
  }
}
