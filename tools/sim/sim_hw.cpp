// Host implementations of the hardware modules for the screen simulator.
#include <Arduino.h>
#include <FS.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <TouchDrvCSTXXX.hpp>
#include <Wire.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>

#include "../../audio.h"
#include "../../imu.h"
#include "../../netota.h"
#include "../../rtcbat.h"
#include "sim.h"

uint32_t gSimMillis = 1000;
SimSerial Serial;
SimEsp ESP;
TwoWire Wire;
SDMMCFS SD_MMC;
SimTouchState gSimTouch;
SimConfig gSim;

static uint32_t rngState = 12345;
void randomSeed(unsigned long s) { rngState = (uint32_t)s ? (uint32_t)s : 1; }
long random(long max) {
  if (max <= 0) return 0;
  rngState = rngState * 1103515245u + 12345u;
  return (long)((rngState >> 8) % (uint32_t)max);
}
long random(long min, long max) { return max <= min ? min : min + random(max - min); }

size_t SimSerial::write(uint8_t c) {
  if (gSim.verbose) fputc(c, stderr);
  return 1;
}
void SimEsp::restart() {
  fprintf(stderr, "[sim] ESP.restart()\n");
  exit(0);
}

/* ---------------- NVS ---------------- */

static std::map<std::string, std::map<std::string, std::vector<uint8_t>>> nvs;

bool Preferences::begin(const char *ns, bool) { ns_ = ns; return true; }
bool Preferences::clear() { nvs[ns_].clear(); return true; }
bool Preferences::remove(const char *k) { return nvs[ns_].erase(k) > 0; }
bool Preferences::isKey(const char *k) { return nvs[ns_].count(k) > 0; }
size_t Preferences::put(const char *k, long long v) {
  std::vector<uint8_t> b(sizeof(v));
  memcpy(b.data(), &v, sizeof(v));
  nvs[ns_][k] = b;
  return sizeof(v);
}
long long Preferences::get(const char *k, long long d) {
  auto &m = nvs[ns_];
  auto it = m.find(k);
  if (it == m.end() || it->second.size() != sizeof(long long)) return d;
  long long v;
  memcpy(&v, it->second.data(), sizeof(v));
  return v;
}
size_t Preferences::putBytes(const char *k, const void *v, size_t n) {
  nvs[ns_][k] = std::vector<uint8_t>((const uint8_t *)v, (const uint8_t *)v + n);
  return n;
}
size_t Preferences::getBytesLength(const char *k) {
  auto &m = nvs[ns_];
  auto it = m.find(k);
  return it == m.end() ? 0 : it->second.size();
}
size_t Preferences::getBytes(const char *k, void *buf, size_t n) {
  auto &m = nvs[ns_];
  auto it = m.find(k);
  if (it == m.end()) return 0;
  size_t c = std::min(n, it->second.size());
  memcpy(buf, it->second.data(), c);
  return c;
}
size_t Preferences::putString(const char *k, const char *v) { return putBytes(k, v, strlen(v) + 1) ; }
String Preferences::getString(const char *k, const String &d) {
  auto &m = nvs[ns_];
  auto it = m.find(k);
  if (it == m.end() || it->second.empty()) return d;
  return String((const char *)it->second.data());
}

size_t Preferences::getString(const char *k, char *buf, size_t n) {
  auto &m = nvs[ns_];
  auto it = m.find(k);
  if (it == m.end() || !n) return 0;
  size_t c = std::min(n - 1, strlen((const char *)it->second.data()));
  memcpy(buf, it->second.data(), c);
  buf[c] = 0;
  return c + 1;
}

/* ---------------- microSD on a host folder ---------------- */

struct fs::File::Handle {
  FILE *f = nullptr;
  bool dir = false;
  ~Handle() { if (f) fclose(f); }
};

static std::string hostPath(const std::string &p) { return gSim.sdRoot + p; }

fs::File::File(const std::string &path, const char *mode) : path_(path) {
  size_t s = path.find_last_of('/');
  name_ = s == std::string::npos ? path : path.substr(s + 1);
  struct stat st;
  std::string hp = hostPath(path);
  if (stat(hp.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    h_ = std::make_shared<Handle>();
    h_->dir = true;
    return;
  }
  const char *m = !strcmp(mode, "w") ? "wb" : (!strcmp(mode, "a") ? "ab" : "rb");
  FILE *f = fopen(hp.c_str(), m);
  if (!f) return;
  h_ = std::make_shared<Handle>();
  h_->f = f;
}
size_t fs::File::size() const {
  if (!h_ || !h_->f) return 0;
  long cur = ftell(h_->f);
  fseek(h_->f, 0, SEEK_END);
  long n = ftell(h_->f);
  fseek(h_->f, cur, SEEK_SET);
  return (size_t)n;
}
size_t fs::File::read(uint8_t *buf, size_t n) { return h_ && h_->f ? fread(buf, 1, n, h_->f) : 0; }
int fs::File::read() { uint8_t c; return read(&c, 1) == 1 ? c : -1; }
size_t fs::File::write(const uint8_t *buf, size_t n) { return h_ && h_->f ? fwrite(buf, 1, n, h_->f) : 0; }
bool fs::File::seek(uint32_t pos) { return h_ && h_->f && fseek(h_->f, pos, SEEK_SET) == 0; }
size_t fs::File::position() const { return h_ && h_->f ? (size_t)ftell(h_->f) : 0; }
int fs::File::available() const { return (int)(size() - position()); }
bool fs::File::isDirectory() const { return h_ && h_->dir; }
fs::File fs::File::openNextFile(const char *mode) {
  if (!h_ || !h_->dir) return File();
  DIR *d = opendir(hostPath(path_).c_str());
  if (!d) return File();
  size_t i = 0;
  struct dirent *e;
  std::string found;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    if (i++ == dirPos_) { found = e->d_name; break; }
  }
  closedir(d);
  if (found.empty()) return File();
  dirPos_++;
  return File(path_ + "/" + found, mode);
}

bool SDMMCFS::begin(const char *, bool, bool, int, uint8_t) {
  struct stat st;
  return stat(gSim.sdRoot.c_str(), &st) == 0;
}
File SDMMCFS::open(const char *path, const char *mode) { return File(path, mode); }
bool SDMMCFS::exists(const char *p) { struct stat st; return stat(hostPath(p).c_str(), &st) == 0; }
bool SDMMCFS::mkdir(const char *p) { return ::mkdir(hostPath(p).c_str(), 0755) == 0; }
bool SDMMCFS::remove(const char *p) { return ::unlink(hostPath(p).c_str()) == 0; }
bool SDMMCFS::rename(const char *a, const char *b) { return ::rename(hostPath(a).c_str(), hostPath(b).c_str()) == 0; }
bool SDMMCFS::rmdir(const char *p) { return ::rmdir(hostPath(p).c_str()) == 0; }

/* ---------------- RTC, battery, power button ---------------- */

static uint32_t rtcBase = 1767261600UL;  // 2026-01-01 10:00 UTC
static uint32_t rtcAtMillis = 0;
bool rtcBegin() { return true; }
uint32_t rtcEpoch() { return rtcBase + (millis() - rtcAtMillis) / 1000; }
void rtcSetEpoch(uint32_t e) { rtcBase = e; rtcAtMillis = millis(); }
bool batBegin() { return true; }
void pmuEnablePanel() {}
int batPercent() { return gSim.battery; }
bool batCharging() { return false; }
bool usbPresent() { return false; }
void setPowerCacheInterval(uint32_t) {}
void pwrSetup() {}
bool pwrShortPressed() {
  bool p = gSim.pwrPressed;
  gSim.pwrPressed = false;
  return p;
}

/* ---------------- IMU and audio ---------------- */

bool imuBegin() { return true; }
bool imuOk() { return true; }
void imuPoll(uint32_t, uint16_t) {}
bool imuShakeEdge() { return false; }
uint16_t imuTakeSteps() { return 0; }
float imuLastMagG() { return 1.0f; }
float imuLastGyroDps() { return 0; }
uint8_t imuAddr() { return 0x6B; }
uint32_t imuPedometer() { return 0; }
uint32_t imuSoftwarePedometer() { return 0; }

static uint8_t sndMode = SOUND_MED;
void audioBegin() {}
void sfxPlay(uint8_t id) { if (gSim.verbose) fprintf(stderr, "[sfx %u]\n", id); }
void audioSetEnabled(bool on) { sndMode = on ? SOUND_MED : SOUND_OFF; }
bool audioEnabled() { return sndMode != SOUND_OFF; }
void audioSetMode(uint8_t m) { sndMode = m; }
uint8_t audioMode() { return sndMode; }
bool audioBusy() { return false; }
void speciesChirpPlay(int16_t, bool) {}

/* ---------------- Wi-Fi / OTA (scripted states) ---------------- */

static NetList simSaved;
static bool simAuto = true;
static NetScanItem simScan[4] = {
  { "Casa", -48, false, true }, { "Ufficio-5G", -63, false, false },
  { "Hotspot di Mario", -71, false, false }, { "Bar Free WiFi", -82, true, false },
};
static uint32_t simGen = 1;

void netBegin(const char *) {
  netListInit(simSaved);
  netListUpsert(simSaved, "Casa", "password1");
  netListUpsert(simSaved, "Ufficio-5G", "secret99");
}
void netLoop(uint32_t) {}
bool netBusy() { return false; }
uint32_t netGen() { return simGen; }
bool netAutoCheck() { return simAuto; }
void netSetAutoCheck(bool on) { simAuto = on; simGen++; }
const NetList &netSaved() { return simSaved; }
bool netForget(int idx) { simGen++; return netListRemove(simSaved, idx); }
bool netStartScan() { simGen++; return true; }
NetScanStatus netScanStatus() { return SCAN_DONE; }
int netScanCount() { return 4; }
const NetScanItem &netScanItem(int i) { return simScan[i & 3]; }
bool netStartJoin(const char *, const char *) { simGen++; return true; }
NetJoinStatus netJoinStatus() { return JOIN_NONE; }
const char *netJoinSsid() { return ""; }
void netClearJoin() {}
bool otaStartCheck(bool) { return true; }
bool otaStartInstall() { return true; }
OtaStatus otaStatus() { return (OtaStatus)gSim.otaStatus; }
OtaError otaError() { return OTA_ERR_NONE; }
uint8_t otaProgress() { return 42; }
const char *otaLatest() { return gSim.otaStatus == OTA_IDLE ? "" : "1.37.9"; }
bool otaFirmwareNewer() { return gSim.otaStatus == OTA_AVAILABLE; }
bool otaSpritesNeeded() { return false; }
bool otaUpdateAvailable() { return gSim.otaStatus == OTA_AVAILABLE; }
bool otaSdBusy() { return false; }
bool otaTakeNotice() { return false; }
bool otaTakeSpritesChanged() { return false; }
