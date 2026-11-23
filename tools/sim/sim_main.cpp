// TamaPoke screen simulator: runs the real sketch on a PC, plays a script of
// touches and saves screenshots plus a layout report for each one.
//
//   tools/sim/run.sh                      # all scenes in tools/sim/scenes/*.txt
//   tools/sim/run.sh tools/sim/scenes/main.txt
//
// Script commands (one per line, '#' comments):
//   wait MS              run the firmware loop for MS milliseconds
//   tap X Y              finger down and up at X,Y
//   hold X Y MS          finger held still for MS
//   swipe X0 Y0 X1 Y1 [MS]
//   rotate CX CY R DEG0 DEG1 [MS]  finger moving along an arc (DEG1 > DEG0: clockwise)
//   gps none|search|fix  fake LC76G on the I2C bus (set it before the first command)
//   tilt AX AY           board tilt in g (accelerometer x/y)
//   hour H               set the clock to H:00 (pet sees it within 30 s: add a wait)
//   pwr                  short press of the power button
//   ota STATE            fake update state: idle | checking | uptodate | available | sprites | firmware | error
//   shot NAME            save out/NAME.png and add NAME to the layout report
#include <zlib.h>

#include <fstream>
#include <sstream>
#include <vector>

#include "sim.h"
#include "sim_sketch.cpp"  // TamaPoke.ino turned into C++ by ino2cpp.py

static std::vector<uint16_t> frame(LCD_WIDTH *LCD_HEIGHT);
static std::vector<SimShape> frameShapes;
static std::string outDir = "tools/sim/out";
static FILE *report = nullptr;
static int totalOutside = 0, totalSmall = 0;

void Arduino_Canvas::flush() {
  memcpy(frame.data(), getFramebuffer(), frame.size() * 2);
  frameShapes = shapes;
  shapes.clear();  // every frame is drawn completely before flush()
}

/* ---------------- PNG ---------------- */

static void pngChunk(FILE *f, const char *type, const std::vector<uint8_t> &data) {
  uint8_t len[4] = { (uint8_t)(data.size() >> 24), (uint8_t)(data.size() >> 16), (uint8_t)(data.size() >> 8), (uint8_t)data.size() };
  fwrite(len, 1, 4, f);
  fwrite(type, 1, 4, f);
  if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
  uLong crc = crc32(0, (const Bytef *)type, 4);
  if (!data.empty()) crc = crc32(crc, data.data(), data.size());
  uint8_t c[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
  fwrite(c, 1, 4, f);
}

static void writePng(const std::string &path, const std::vector<uint8_t> &rgb, int w, int h) {
  std::vector<uint8_t> raw;
  raw.reserve((w * 3 + 1) * h);
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
  }
  uLongf zlen = compressBound(raw.size());
  std::vector<uint8_t> z(zlen);
  compress2(z.data(), &zlen, raw.data(), raw.size(), 6);
  z.resize(zlen);
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return; }
  const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
  pngChunk(f, "IHDR", ihdr);
  pngChunk(f, "IDAT", z);
  pngChunk(f, "IEND", {});
  fclose(f);
}

/* ---------------- layout checks ---------------- */

static const double R = LCD_WIDTH / 2.0, CXF = (LCD_WIDTH - 1) / 2.0, CYF = (LCD_HEIGHT - 1) / 2.0;

static double dist(double x, double y) { return std::sqrt((x - CXF) * (x - CXF) + (y - CYF) * (y - CYF)); }

// how many pixels the shape reaches beyond the round panel (<= 0: inside)
static double overflow(const SimShape &s) {
  double far = 0;
  if (s.kind == 'C') {
    far = dist(s.x + s.r, s.y + s.r) + s.r;
  } else {
    double r = s.kind == 'r' ? std::min<int>(s.r, std::min(s.w, s.h) / 2) : 0;
    double xs[2] = { s.x + r, s.x + s.w - 1 - r }, ys[2] = { s.y + r, s.y + s.h - 1 - r };
    for (double x : xs)
      for (double y : ys) far = std::max(far, dist(x, y) + r);
  }
  return far - R;
}

// a button is a filled rounded box with an outline drawn on the same rectangle
static bool hasOutline(const SimShape &f) {
  for (const SimShape &s : frameShapes)
    if (s.kind == 'r' && !s.filled && s.x == f.x && s.y == f.y && s.w == f.w && s.h == f.h) return true;
  return false;
}

static bool isBackground(const SimShape &s) { return s.w >= 400 || s.h >= 400; }

static void markShape(std::vector<uint8_t> &rgb, const SimShape &s, uint8_t r, uint8_t g, uint8_t b) {
  auto px = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    uint8_t *p = &rgb[(y * LCD_WIDTH + x) * 3];
    p[0] = r; p[1] = g; p[2] = b;
  };
  for (int i = 0; i < s.w; i++) { px(s.x + i, s.y); px(s.x + i, s.y + s.h - 1); px(s.x + i, s.y + 1); px(s.x + i, s.y + s.h - 2); }
  for (int j = 0; j < s.h; j++) { px(s.x, s.y + j); px(s.x + s.w - 1, s.y + j); px(s.x + 1, s.y + j); px(s.x + s.w - 2, s.y + j); }
}

static void shot(const std::string &name) {
  std::vector<uint8_t> rgb(LCD_WIDTH * LCD_HEIGHT * 3);
  for (int y = 0; y < LCD_HEIGHT; y++) {
    for (int x = 0; x < LCD_WIDTH; x++) {
      uint16_t c = frame[y * LCD_WIDTH + x];
      uint8_t *p = &rgb[(y * LCD_WIDTH + x) * 3];
      p[0] = ((c >> 11) & 31) * 255 / 31;
      p[1] = ((c >> 5) & 63) * 255 / 63;
      p[2] = (c & 31) * 255 / 31;
      double d = dist(x, y);
      if (d > R) { p[0] = p[1] = p[2] = 24; }  // outside the round panel
    }
  }
  writePng(outDir + "/" + name + ".png", rgb, LCD_WIDTH, LCD_HEIGHT);

  // report + a copy with problems outlined (red: outside the circle, orange: small target)
  std::vector<uint8_t> chk = rgb;
  int outside = 0, small = 0;
  fprintf(report, "## %s\n", name.c_str());
  for (const SimShape &s : frameShapes) {
    if (isBackground(s)) continue;
    // only interface elements: text and rounded boxes (scenery and pixel art use plain rects/circles)
    if (s.kind != 'T' && s.kind != 'r') continue;
    double o = overflow(s);
    if (o > 1.0) {
      outside++;
      markShape(chk, s, 255, 0, 0);
      fprintf(report, "- OUTSIDE %.0fpx  %s %d,%d %dx%d%s%s\n", o, s.kind == 'T' ? "text" : (s.kind == 'C' ? "circle" : "box"),
              s.x, s.y, s.w, s.h, s.text[0] ? "  \"" : "", s.text[0] ? (std::string(s.text) + "\"").c_str() : "");
    }
    // filled rounded boxes with an outline are what the sketch uses for buttons
    if (s.kind == 'r' && s.filled && (s.h < 44 || s.w < 44) && s.w >= 16 && s.h >= 16 && hasOutline(s)) {
      small++;
      markShape(chk, s, 255, 150, 0);
      fprintf(report, "- SMALL  %dx%d at %d,%d\n", s.w, s.h, s.x, s.y);
    }
  }
  if (!outside && !small) fprintf(report, "- ok\n");
  fprintf(report, "\n");
  if (outside || small) writePng(outDir + "/" + name + ".check.png", chk, LCD_WIDTH, LCD_HEIGHT);
  totalOutside += outside;
  totalSmall += small;
  printf("%-28s outside=%d small=%d\n", name.c_str(), outside, small);
}

/* ---------------- driving the sketch ---------------- */

static void step(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 10) {
    gSimMillis += 10;
    if (gSimTouch.down) gTouchIrq = true;
    loop();
  }
}

static void touchPath(int x0, int y0, int x1, int y1, uint32_t ms) {
  int steps = std::max<int>(1, ms / 20);
  gSimTouch.down = true;
  for (int i = 0; i <= steps; i++) {
    gSimTouch.x = x0 + (x1 - x0) * i / steps;
    gSimTouch.y = y0 + (y1 - y0) * i / steps;
    gTouchIrq = true;
    step(20);
  }
  gSimTouch.down = false;
  gTouchIrq = true;
  step(60);
}

static int otaFromName(const std::string &s) {
  if (s == "checking") return OTA_CHECKING;
  if (s == "uptodate") return OTA_UP_TO_DATE;
  if (s == "available") return OTA_AVAILABLE;
  if (s == "sprites") return OTA_SPRITES;
  if (s == "firmware") return OTA_FIRMWARE;
  if (s == "error") return OTA_ERROR;
  return OTA_IDLE;
}

static std::string shotName;
static bool booted = false;

static void runScript(const std::string &path) {
  std::ifstream in(path);
  if (!in) { fprintf(stderr, "cannot open %s\n", path.c_str()); exit(2); }
  std::string line;
  int ln = 0;
  while (std::getline(in, line)) {
    ln++;
    size_t h = line.find('#');
    if (h != std::string::npos) line = line.substr(0, h);
    std::istringstream ss(line);
    std::string cmd;
    if (!(ss >> cmd)) continue;
    if (!booted && cmd != "gps") { setup(); booted = true; step(1500); }
    int a = 0, b = 0, c = 0, d = 0, e = 0;
    if (cmd == "wait") { ss >> a; step(a); }
    else if (cmd == "tap") { ss >> a >> b; touchPath(a, b, a, b, 80); step(250); }
    else if (cmd == "hold") { ss >> a >> b >> c; touchPath(a, b, a, b, c); step(250); }
    else if (cmd == "swipe") { ss >> a >> b >> c >> d; e = 240; ss >> e; touchPath(a, b, c, d, e); step(400); }
    else if (cmd == "rotate") {
      int r0 = 0, d0 = 0, d1 = 0;
      ss >> a >> b >> r0 >> d0 >> d1; e = 800; ss >> e;
      int steps = std::max(2, e / 20);
      gSimTouch.down = true;
      for (int i = 0; i <= steps; i++) {
        double th = (d0 + (d1 - d0) * (double)i / steps) * M_PI / 180.0;
        gSimTouch.x = (int16_t)lround(a + r0 * cos(th));
        gSimTouch.y = (int16_t)lround(b + r0 * sin(th));
        gTouchIrq = true;
        step(20);
        if (i == steps * 3 / 4 && !shotName.empty()) { shot(shotName); shotName.clear(); }
      }
      gSimTouch.down = false;
      gTouchIrq = true;
      step(400);
    }
    else if (cmd == "rotshot") { ss >> shotName; }  // next rotate takes a shot while the finger is still turning
    else if (cmd == "gps") { std::string m; ss >> m; gSimGpsFix = m == "fix" ? 1 : (m == "search" ? 0 : -1); if (!booted) { setup(); booted = true; step(1500); } }
    else if (cmd == "hour") {  // local clock to HH:00 of the same day (the sketch reads it within 30 s)
      extern uint32_t rtcEpoch(); extern void rtcSetEpoch(uint32_t);
      int h = 0; ss >> h; rtcSetEpoch((rtcEpoch() / 86400UL) * 86400UL + (uint32_t)h * 3600UL); }
    else if (cmd == "tilt") { extern float gSimAx, gSimAy; float fx = 0, fy = 0; ss >> fx >> fy; gSimAx = fx; gSimAy = fy; }
    else if (cmd == "pwr") { gSim.pwrPressed = true; step(600); }
    else if (cmd == "ota") { std::string s; ss >> s; gSim.otaStatus = otaFromName(s); step(400); }
    else if (cmd == "shot") { std::string n; ss >> n; step(700); shot(n); }
    else { fprintf(stderr, "%s:%d: unknown command %s\n", path.c_str(), ln, cmd.c_str()); exit(2); }
  }
}

int main(int argc, char **argv) {
  std::vector<std::string> scripts;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-v") gSim.verbose = true;
    else if (a == "-o" && i + 1 < argc) outDir = argv[++i];
    else if (a == "-sd" && i + 1 < argc) gSim.sdRoot = argv[++i];
    else scripts.push_back(a);
  }
  report = fopen((outDir + "/layout-report.md").c_str(), "w");
  if (!report) { fprintf(stderr, "output folder %s missing\n", outDir.c_str()); return 2; }
  fprintf(report, "# Layout report\n\nOUTSIDE: text or box drawn beyond the round panel. SMALL: button (outlined rounded box) under 44 px, hard to tap.\n\n");
  for (auto &s : scripts) runScript(s);
  fprintf(report, "Total: %d outside, %d small\n", totalOutside, totalSmall);
  fclose(report);
  printf("total outside=%d small=%d (report: %s/layout-report.md)\n", totalOutside, totalSmall, outDir.c_str());
  return 0;
}
