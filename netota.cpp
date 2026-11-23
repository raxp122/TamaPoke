// Wi-Fi and OTA updates. See netota.h for the overview.
#include "netota.h"

#include <FS.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <sys/time.h>
#include <time.h>

#include "sdmon.h"

#define OTA_REPO "raxp122/TamaPoke"
#define OTA_BASE "https://github.com/" OTA_REPO "/releases/latest/download/"
#define URL_VERSION OTA_BASE "version.json"
#define URL_APP OTA_BASE "tamapoke-app.bin"
#define URL_SPRITES OTA_BASE "tamapoke-sprites.pak"

#define NET_NS "tpnet"        // NVS namespace: saved networks and settings (separate from the save game)
#define SPRITE_TMP_DIR "/tpota"
#define AUTO_FIRST_MS 15000UL
#define AUTO_EVERY_MS (12UL * 3600UL * 1000UL)
#define CONFIRM_AFTER_MS 45000UL      // a new firmware must run this long before it is confirmed
#define CONFIRM_MIN_HEAP 16000U       // ...and keep at least this much internal heap free
#define GUARD_CLEAR_MS (5UL * 60UL * 1000UL)  // crash counter is cleared after 5 min of normal running
#define GUARD_MAX_CRASHES 3
#define GUARD_MAGIC 0x7A3A0001UL
#define NET_BLOB_BYTES (6 + NET_MAX * (NET_SSID_LEN + 1 + NET_PASS_LEN + 1 + 4))

// CA bundle of ESP-IDF (part of the precompiled mbedTLS library of the core)
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// The core marks a new firmware as valid right at boot unless this returns true.
// We confirm it ourselves after it has been running for a while (netLoop), so a
// firmware that crashes during start-up is rolled back by the bootloader.
extern "C" bool verifyRollbackLater() { return true; }

static char curVersion[32];
static char badVersion[24];  // version rolled back after crashes: not offered automatically again

// Survive resets (not power cycles): consecutive crash counter for the boot guard.
RTC_NOINIT_ATTR static uint32_t guardMagic;
RTC_NOINIT_ATTR static uint32_t guardCrashes;
static bool guardCleared = false;

void otaBootGuard(const char *currentVersion) {
  esp_reset_reason_t r = esp_reset_reason();
  bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
  if (guardMagic != GUARD_MAGIC || r == ESP_RST_POWERON) {
    guardMagic = GUARD_MAGIC;
    guardCrashes = 0;
  }
  if (crash) guardCrashes++;
  else if (r != ESP_RST_SW && r != ESP_RST_DEEPSLEEP) guardCrashes = 0;  // our own restarts are neutral
  Serial.printf("BOOT reset=%d crashes=%lu\n", (int)r, (unsigned long)guardCrashes);
  if (guardCrashes < GUARD_MAX_CRASHES) return;
  guardCrashes = 0;
  // Three crashes in a row without 5 minutes of normal running: go back to the
  // previous firmware, if there is one, and remember this version as bad.
  if (!esp_ota_check_rollback_is_possible()) {
    Serial.println("BOOT crash loop, no previous firmware to go back to");
    return;
  }
  Preferences p;
  p.begin(NET_NS, false);
  p.putString("badver", currentVersion);
  p.end();
  Serial.printf("BOOT crash loop: rolling back from %s\n", currentVersion);
  delay(100);
  esp_ota_mark_app_invalid_rollback_and_reboot();
}
static NetList saved;
static bool autoCheck = true;

static volatile bool busy = false;
static volatile uint32_t gen = 1;

static volatile NetScanStatus scanSt = SCAN_NONE;
static NetScanItem scanItems[NET_SCAN_MAX];
static volatile int scanCount = 0;

static volatile NetJoinStatus joinSt = JOIN_NONE;
static char joinSsid[NET_SSID_LEN + 1];
static char joinPass[NET_PASS_LEN + 1];

static volatile OtaStatus otaSt = OTA_IDLE;
static volatile OtaError otaErr = OTA_ERR_NONE;
static volatile uint8_t otaPct = 0;
static char latest[24];
static volatile bool fwNewer = false, sprNeeded = false, available = false;
static volatile bool sdBusy = false, notice = false, spritesChanged = false;
static bool checkManual = false;
static uint32_t nextAuto = AUTO_FIRST_MS;

// internet clock
#define TIME_FIRST_MS 20000UL
#define TIME_EVERY_MS (60UL * 60UL * 1000UL)
static uint8_t tzZone = TZ_DEFAULT;
static uint8_t dstMode = DSTM_AUTO;
static uint32_t nextTime = TIME_FIRST_MS;
static volatile bool timePending = false;
static volatile uint32_t timeLocal = 0;   // local epoch waiting to be written to the RTC
static volatile uint32_t lastSyncLocal = 0;
static bool confirmed = false;

static void bump() { gen = gen + 1; }

uint32_t netGen() { return gen; }
bool netBusy() { return busy; }
bool netAutoCheck() { return autoCheck; }
const NetList &netSaved() { return saved; }
NetScanStatus netScanStatus() { return scanSt; }
int netScanCount() { return scanCount; }
const NetScanItem &netScanItem(int i) { return scanItems[i < 0 ? 0 : (i >= NET_SCAN_MAX ? NET_SCAN_MAX - 1 : i)]; }
NetJoinStatus netJoinStatus() { return joinSt; }
const char *netJoinSsid() { return joinSsid; }
OtaStatus otaStatus() { return otaSt; }
OtaError otaError() { return otaErr; }
uint8_t otaProgress() { return otaPct; }
const char *otaLatest() { return latest; }
bool otaFirmwareNewer() { return fwNewer; }
bool otaSpritesNeeded() { return sprNeeded; }
bool otaUpdateAvailable() { return available; }
bool otaSdBusy() { return sdBusy; }

bool otaTakeNotice() {
  if (!notice) return false;
  notice = false;
  return true;
}

bool otaTakeSpritesChanged() {
  if (!spritesChanged) return false;
  spritesChanged = false;
  return true;
}

void netClearJoin() {
  if (busy) return;
  joinSt = JOIN_NONE;
  bump();
}

/* ---------------- persistence ---------------- */

static void saveList() {
  uint8_t blob[NET_BLOB_BYTES];
  netListToBlob(saved, blob);
  Preferences p;
  p.begin(NET_NS, false);
  p.putBytes("list", blob, netListBlobSize());
  p.end();
}

static void loadSpriteSha(char *out, size_t len) {
  Preferences p;
  p.begin(NET_NS, true);
  String s = p.getString("sprsha", "");
  p.end();
  snprintf(out, len, "%s", s.c_str());
}

static void storeSpriteSha(const char *sha) {
  Preferences p;
  p.begin(NET_NS, false);
  p.putString("sprsha", sha);
  p.end();
}

void netBegin(const char *currentVersion) {
  snprintf(curVersion, sizeof(curVersion), "%s", currentVersion);
  netListInit(saved);
  Preferences p;
  if (p.begin(NET_NS, true)) {  // read-only open fails on the very first start (namespace missing)
    size_t n = p.getBytesLength("list");
    if (n == netListBlobSize()) {
      uint8_t blob[NET_BLOB_BYTES];
      p.getBytes("list", blob, n);
      netListFromBlob(saved, blob, n);
    }
    autoCheck = p.getBool("auto", true);
    snprintf(badVersion, sizeof(badVersion), "%s", p.getString("badver", "").c_str());
    tzZone = p.getUChar("tz", TZ_DEFAULT);
    if (tzZone >= TZ_COUNT) tzZone = TZ_DEFAULT;
    dstMode = p.getUChar("dst", DSTM_AUTO);
    if (dstMode >= DSTM_COUNT) dstMode = DSTM_AUTO;
    p.end();
  }
  WiFi.persistent(false);  // credentials live in our own list, not in the driver's NVS area
  Serial.printf("NET saved=%u auto=%u\n", saved.count, autoCheck);
}

void netSetAutoCheck(bool on) {
  autoCheck = on;
  Preferences p;
  p.begin(NET_NS, false);
  p.putBool("auto", on);
  p.end();
  if (on) nextAuto = millis() + 3000;
  bump();
}

uint8_t netTimeZone() { return tzZone; }

void netSetTimeZone(uint8_t zone) {
  if (zone >= TZ_COUNT) return;
  tzZone = zone;
  Preferences p;
  p.begin(NET_NS, false);
  p.putUChar("tz", zone);
  p.end();
  lastSyncLocal = 0;
  if (zone != TZ_OFF) nextTime = millis() + 2000;  // apply the new zone soon
  bump();
}

uint32_t netLastTimeSync() { return lastSyncLocal; }

uint8_t netDstMode() { return dstMode; }

void netSetDstMode(uint8_t mode) {
  if (mode >= DSTM_COUNT) return;
  dstMode = mode;
  Preferences p;
  p.begin(NET_NS, false);
  p.putUChar("dst", mode);
  p.end();
  lastSyncLocal = 0;
  if (tzZone != TZ_OFF) nextTime = millis() + 2000;  // apply it soon
  bump();
}

bool netTakeTime(uint32_t *localEpoch) {
  if (!timePending) return false;
  timePending = false;
  *localEpoch = timeLocal;
  return true;
}

bool netForget(int idx) {
  if (busy || !netListRemove(saved, idx)) return false;
  saveList();
  bump();
  return true;
}

/* ---------------- radio ---------------- */

static void radioOn() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  delay(100);
}

static void radioOff() {
  WiFi.disconnect(true, false);
  delay(50);
  WiFi.mode(WIFI_OFF);
}

// waits for the connection; returns the final status
static wl_status_t waitConnected(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  wl_status_t s = WiFi.status();
  while (millis() - t0 < timeoutMs) {
    s = WiFi.status();
    if (s == WL_CONNECTED) return s;
    // "failed" and "not found" are only trusted after a few seconds (stale status right after begin)
    if (millis() - t0 > 4000 && (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL)) return s;
    delay(200);
  }
  return s;
}

static int doScanRaw() {
  int n = WiFi.scanNetworks(false, false);
  return n < 0 ? 0 : n;
}

// scans and connects to the strongest saved network in range
static bool connectKnown() {
  if (!saved.count) return false;
  int n = doScanRaw();
  if (n > 40) n = 40;
  const char *ssids[40];
  int8_t rssi[40];
  static String names[40];
  for (int i = 0; i < n; i++) {
    names[i] = WiFi.SSID(i);
    ssids[i] = names[i].c_str();
    rssi[i] = (int8_t)WiFi.RSSI(i);
  }
  int cand[NET_MAX];
  int c = netListCandidates(saved, ssids, rssi, n, cand, 3);
  WiFi.scanDelete();
  for (int i = 0; i < c; i++) {
    const NetEntry &e = saved.e[cand[i]];
    Serial.printf("NET connecting to %s\n", e.ssid);
    WiFi.disconnect(false, false);
    WiFi.begin(e.ssid, e.pass[0] ? e.pass : nullptr);
    if (waitConnected(15000) == WL_CONNECTED) {
      Serial.printf("NET connected %s ip=%s\n", e.ssid, WiFi.localIP().toString().c_str());
      int order[NET_MAX];
      netListOrder(saved, order);
      if (order[0] != cand[i]) {  // only write NVS when the most recent network changes
        netListTouch(saved, cand[i]);
        saveList();
      }
      return true;
    }
  }
  return false;
}

/* ---------------- HTTP helpers ---------------- */

struct Http {
  NetworkClientSecure cli;
  HTTPClient http;
  bool open(const char *url) {
    cli.setCACertBundle(x509_crt_bundle_start, x509_crt_bundle_end - x509_crt_bundle_start);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setConnectTimeout(15000);
    http.setTimeout(20000);
    http.setReuse(false);
    if (!http.begin(cli, url)) return false;
    code = http.GET();
    return code > 0;
  }
  ~Http() { http.end(); }
  int code = 0;
};

static void fail(OtaError e) {
  otaErr = e;
  otaSt = OTA_ERROR;
  bump();
  Serial.printf("OTA error %u\n", (unsigned)e);
}

static bool fetchManifest(OtaManifest &m) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt) delay(2000);
    Http h;
    if (!h.open(URL_VERSION)) { Serial.printf("OTA version.json: %d\n", h.code); continue; }
    if (h.code == 404) { fail(OTA_ERR_NO_RELEASE); return false; }
    if (h.code != 200) { Serial.printf("OTA version.json HTTP %d\n", h.code); continue; }
    String body = h.http.getString();
    if (body.length() > 2048) { fail(OTA_ERR_BAD_FILE); return false; }
    if (!otaParseManifest(body.c_str(), m)) { fail(OTA_ERR_BAD_FILE); return false; }
    return true;
  }
  fail(OTA_ERR_DOWNLOAD);
  return false;
}

// Streams `total` bytes of the body to fn(buf, n). Computes the SHA-256 on the way.
// fn returns false to stop early (counts as success when stopOk).
template <typename F>
static bool streamBody(Http &h, uint32_t total, uint8_t *sha, F fn, bool *stopped) {
  NetworkClient *s = h.http.getStreamPtr();
  if (!s) return false;
  static uint8_t buf[4096];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint32_t got = 0, last = millis();
  bool ok = true;
  *stopped = false;
  while (got < total) {
    size_t av = s->available();
    if (!av) {
      if (!s->connected() || millis() - last > 20000) { ok = false; break; }
      delay(2);
      continue;
    }
    size_t want = total - got;
    if (want > sizeof(buf)) want = sizeof(buf);
    if (want > av) want = av;
    int n = s->read(buf, want);
    if (n <= 0) { delay(2); continue; }
    last = millis();
    got += n;
    mbedtls_sha256_update(&ctx, buf, n);
    if (!fn(buf, (size_t)n, got)) { *stopped = true; break; }
  }
  mbedtls_sha256_finish(&ctx, sha);
  mbedtls_sha256_free(&ctx);
  return ok;
}

static bool shaMatches(const uint8_t *sha, const char *hex) {
  char s[65];
  for (int i = 0; i < 32; i++) snprintf(s + i * 2, 3, "%02x", sha[i]);
  return !strcmp(s, hex);
}

/* ---------------- sprites ---------------- */

static TpakEntry *entries = nullptr;  // TPAK_MAX_FILES, in PSRAM

static bool ensureEntries() {
  if (!entries) entries = (TpakEntry *)ps_malloc(sizeof(TpakEntry) * TPAK_MAX_FILES);
  return entries != nullptr;
}

static String sdPath(const char *dir, const char *pakName) {  // "mons/p001.bin" -> dir + "/p001.bin"
  return String(dir) + "/" + (pakName + 5);
}

// Only reads the index: are all files already on the card with the right size?
struct VerifySink : TpakSink {
  bool match = false;
  bool onIndex(const TpakEntry *e, uint16_t count) override {
    match = true;
    for (int i = 0; i < count && match; i++) {
      File f = SD_MMC.open(sdPath("/mons", e[i].name), FILE_READ);
      if (!f || f.isDirectory() || f.size() != e[i].size) match = false;
      if (f) f.close();
    }
    return false;  // stop: nothing to download
  }
  bool onFileData(uint16_t, const uint8_t *, size_t) override { return false; }
  bool onFileEnd(uint16_t) override { return false; }
};

// Writes every file to SPRITE_TMP_DIR; the caller moves them to /mons afterwards.
struct WriteSink : TpakSink {
  File f;
  int openIdx = -1;
  const TpakEntry *ents = nullptr;
  bool open(uint16_t idx) {
    if (openIdx == idx) return true;
    if (f) f.close();
    f = SD_MMC.open(sdPath(SPRITE_TMP_DIR, ents[idx].name), FILE_WRITE);
    openIdx = idx;
    return (bool)f;
  }
  bool onIndex(const TpakEntry *e, uint16_t) override { ents = e; return true; }
  bool onFileData(uint16_t idx, const uint8_t *d, size_t len) override {
    return open(idx) && f.write(d, len) == len;
  }
  bool onFileEnd(uint16_t idx) override {
    if (!open(idx)) return false;
    f.close();
    openIdx = -1;
    return true;
  }
};

static void clearTmpDir() {
  File dir = SD_MMC.open(SPRITE_TMP_DIR);
  if (dir && dir.isDirectory()) {
    String names[16];
    // delete in batches: removing while iterating is not reliable on FAT
    for (;;) {
      int n = 0;
      dir.rewindDirectory();
      File e;
      while (n < 16 && (e = dir.openNextFile())) {
        names[n++] = String(SPRITE_TMP_DIR) + "/" + e.name();
        e.close();
      }
      if (!n) break;
      for (int i = 0; i < n; i++) SD_MMC.remove(names[i]);
    }
  }
  if (dir) dir.close();
  SD_MMC.rmdir(SPRITE_TMP_DIR);
}

// Quick check without downloading the sprites: header only.
static bool spritesAlreadyOnCard(const OtaManifest &m, bool *match) {
  *match = false;
  if (!ensureEntries()) return false;
  Http h;
  if (!h.open(URL_SPRITES) || h.code != 200) return false;
  VerifySink sink;
  TpakParser parser(entries, &sink);
  uint8_t sha[32];
  bool stopped;
  streamBody(h, m.spritesSize, sha, [&](const uint8_t *b, size_t n, uint32_t) { return parser.feed(b, n); }, &stopped);
  if (parser.state() != TpakParser::STOPPED) return false;
  *match = sink.match;
  return true;
}

// true when the sprites must be downloaded; resolves the first-time case (no
// hash stored yet) by comparing the pack index with the card.
static bool decideSprites(const OtaManifest &m) {
  if (!m.spritesSize || !sdReady) return false;
  char stored[65];
  loadSpriteSha(stored, sizeof(stored));
  if (!strcmp(stored, m.spritesSha)) return false;
  if (stored[0]) return true;  // a different set is installed
  bool match = false;
  if (spritesAlreadyOnCard(m, &match) && match) {
    storeSpriteSha(m.spritesSha);
    Serial.println("OTA sprites already on the card");
    return false;
  }
  return true;
}

static bool installSprites(const OtaManifest &m) {
  if (!sdReady) { fail(OTA_ERR_SD); return false; }
  if (!ensureEntries()) { fail(OTA_ERR_DOWNLOAD); return false; }
  uint64_t freeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
  if (freeBytes < (uint64_t)m.spritesSize + 8ULL * 1024 * 1024) { fail(OTA_ERR_SD_FULL); return false; }
  otaSt = OTA_SPRITES;
  otaPct = 0;
  bump();
  sdBusy = true;
  clearTmpDir();
  SD_MMC.mkdir(SPRITE_TMP_DIR);
  SD_MMC.mkdir("/mons");
  Http h;
  if (!h.open(URL_SPRITES) || h.code != 200 || (uint32_t)h.http.getSize() != m.spritesSize) {
    Serial.printf("OTA sprites HTTP %d size %d\n", h.code, h.http.getSize());
    sdBusy = false;
    fail(OTA_ERR_DOWNLOAD);
    return false;
  }
  WriteSink sink;
  TpakParser parser(entries, &sink);
  uint8_t sha[32];
  bool stopped = false;
  bool ok = streamBody(h, m.spritesSize, sha, [&](const uint8_t *b, size_t n, uint32_t got) {
    uint8_t p = (uint8_t)((uint64_t)got * 100 / m.spritesSize);
    if (p != otaPct) { otaPct = p; bump(); }
    return parser.feed(b, n);
  }, &stopped);
  if (sink.f) sink.f.close();
  if (!ok || stopped || parser.state() != TpakParser::DONE) {
    Serial.printf("OTA sprites stream ok=%d stopped=%d state=%d err=%s\n", ok, stopped, parser.state(), parser.error());
    clearTmpDir();
    sdBusy = false;
    fail(stopped && parser.state() != TpakParser::ERROR ? OTA_ERR_SD : (parser.state() == TpakParser::ERROR ? OTA_ERR_BAD_FILE : OTA_ERR_DOWNLOAD));
    return false;
  }
  if (!shaMatches(sha, m.spritesSha)) {
    clearTmpDir();
    sdBusy = false;
    fail(OTA_ERR_BAD_FILE);
    return false;
  }
  // everything arrived intact: move into place
  bool moved = true;
  for (int i = 0; i < parser.count(); i++) {
    String dst = sdPath("/mons", entries[i].name);
    if (SD_MMC.exists(dst)) SD_MMC.remove(dst);
    if (!SD_MMC.rename(sdPath(SPRITE_TMP_DIR, entries[i].name), dst)) moved = false;
  }
  clearTmpDir();
  sdBusy = false;
  if (!moved) { fail(OTA_ERR_SD); return false; }
  storeSpriteSha(m.spritesSha);
  spritesChanged = true;
  sprNeeded = false;
  Serial.printf("OTA sprites installed (%u files)\n", parser.count());
  return true;
}

/* ---------------- firmware ---------------- */

static bool installFirmware(const OtaManifest &m) {
  otaSt = OTA_FIRMWARE;
  otaPct = 0;
  bump();
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt) { Serial.printf("OTA firmware retry %d\n", attempt); delay(2000); }
    Http h;
    if (!h.open(URL_APP) || h.code != 200) { Serial.printf("OTA app HTTP %d\n", h.code); continue; }
    if ((uint32_t)h.http.getSize() != m.appSize) { fail(OTA_ERR_BAD_FILE); return false; }
    if (!Update.begin(m.appSize, U_FLASH)) { fail(OTA_ERR_FLASH); return false; }
    uint8_t sha[32];
    bool stopped = false;
    bool ok = streamBody(h, m.appSize, sha, [&](const uint8_t *b, size_t n, uint32_t got) {
      uint8_t p = (uint8_t)((uint64_t)got * 100 / m.appSize);
      if (p != otaPct) { otaPct = p; bump(); }
      return Update.write((uint8_t *)b, n) == n;
    }, &stopped);
    if (!ok || stopped) {
      Update.abort();
      if (stopped) { fail(OTA_ERR_FLASH); return false; }
      continue;  // connection lost: try again
    }
    if (!shaMatches(sha, m.appSha)) { Update.abort(); fail(OTA_ERR_BAD_FILE); return false; }
    if (!Update.end(true)) { Serial.printf("OTA Update.end: %s\n", Update.errorString()); fail(OTA_ERR_FLASH); return false; }
    Serial.printf("OTA firmware %s installed\n", m.version);
    return true;
  }
  fail(OTA_ERR_DOWNLOAD);
  return false;
}

/* ---------------- jobs ---------------- */

// Asks the time servers while connected; the main loop writes the RTC (netTakeTime).
static bool syncTime() {
  if (tzZone == TZ_OFF) return false;
  struct timeval zero = { 0, 0 };
  settimeofday(&zero, nullptr);  // so a fresh answer is recognisable
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");
  time_t now = 0;
  for (int i = 0; i < 50 && (now = time(nullptr)) < 1700000000; i++) delay(200);
  if (now < 1700000000) {
    Serial.println("TIME no answer from the time servers");
    return false;
  }
  uint32_t utc = (uint32_t)now;
  timeLocal = utc + tzOffsetSecondsMode(tzZone, utc, dstMode);
  lastSyncLocal = timeLocal;
  timePending = true;
  nextTime = millis() + TIME_EVERY_MS;
  Serial.printf("TIME utc=%lu local=%lu zone=%s\n", (unsigned long)utc, (unsigned long)timeLocal, TZ_ZONES[tzZone].label);
  bump();
  return true;
}

static void jobTime() {
  if (connectKnown()) syncTime();
  else nextTime = millis() + 10UL * 60UL * 1000UL;  // no network around: try again in 10 minutes
}

enum Job : uint8_t { J_SCAN, J_JOIN, J_CHECK, J_INSTALL, J_TIME };

static void jobScan() {
  scanSt = SCAN_BUSY;
  bump();
  int n = doScanRaw();
  int cnt = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || s.length() > NET_SSID_LEN) continue;  // hidden networks
    int8_t r = (int8_t)WiFi.RSSI(i);
    int k = 0;
    while (k < cnt && strcmp(scanItems[k].ssid, s.c_str())) k++;
    if (k < cnt) {  // same SSID from another access point: keep the strongest
      if (r > scanItems[k].rssi) scanItems[k].rssi = r;
      continue;
    }
    if (cnt >= NET_SCAN_MAX) continue;
    NetScanItem &it = scanItems[cnt++];
    snprintf(it.ssid, sizeof(it.ssid), "%s", s.c_str());
    it.rssi = r;
    it.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    it.saved = netListFind(saved, it.ssid) >= 0;
  }
  WiFi.scanDelete();
  // strongest first
  for (int i = 1; i < cnt; i++) {
    NetScanItem v = scanItems[i];
    int j = i - 1;
    while (j >= 0 && scanItems[j].rssi < v.rssi) { scanItems[j + 1] = scanItems[j]; j--; }
    scanItems[j + 1] = v;
  }
  scanCount = cnt;
  scanSt = n >= 0 ? SCAN_DONE : SCAN_FAILED;
  bump();
}

static void jobJoin() {
  joinSt = JOIN_BUSY;
  bump();
  WiFi.disconnect(false, false);
  WiFi.begin(joinSsid, joinPass[0] ? joinPass : nullptr);
  wl_status_t s = waitConnected(20000);
  if (s == WL_CONNECTED) {
    netListUpsert(saved, joinSsid, joinPass);
    saveList();
    for (int i = 0; i < scanCount; i++)
      if (!strcmp(scanItems[i].ssid, joinSsid)) scanItems[i].saved = true;
    joinSt = JOIN_OK;
  } else {
    joinSt = s == WL_NO_SSID_AVAIL ? JOIN_NOT_FOUND : (s == WL_CONNECT_FAILED ? JOIN_WRONG_PASS : JOIN_FAILED);
  }
  memset(joinPass, 0, sizeof(joinPass));
  Serial.printf("NET join %s -> %u\n", joinSsid, (unsigned)joinSt);
  bump();
}

static void jobCheck(bool manual) {
  otaErr = OTA_ERR_NONE;
  otaSt = OTA_CONNECTING;
  bump();
  if (!connectKnown()) {
    if (manual) fail(OTA_ERR_NO_NETWORK);
    else { otaSt = available ? OTA_AVAILABLE : OTA_IDLE; bump(); }
    return;
  }
  syncTime();  // already connected: refresh the clock too
  otaSt = OTA_CHECKING;
  bump();
  delay(500);
  OtaManifest m;
  if (!fetchManifest(m)) {
    if (!manual) { otaSt = available ? OTA_AVAILABLE : OTA_IDLE; otaErr = OTA_ERR_NONE; bump(); }
    return;
  }
  snprintf(latest, sizeof(latest), "%s", m.version);
  fwNewer = otaCompareVersions(m.version, curVersion) > 0;
  sprNeeded = decideSprites(m);
  bool was = available;
  available = fwNewer || sprNeeded;
  otaSt = available ? OTA_AVAILABLE : OTA_UP_TO_DATE;
  // a version that crashed here and was rolled back is only installed on request
  if (available && !was && !manual && !(fwNewer && !strcmp(m.version, badVersion))) notice = true;
  Serial.printf("OTA running %s, latest %s, fw=%d sprites=%d\n", curVersion, m.version, fwNewer, sprNeeded);
  bump();
}

static void jobInstall() {
  otaErr = OTA_ERR_NONE;
  otaSt = OTA_CONNECTING;
  bump();
  if (!connectKnown()) { fail(OTA_ERR_NO_NETWORK); return; }
  otaSt = OTA_CHECKING;
  bump();
  OtaManifest m;
  if (!fetchManifest(m)) return;
  snprintf(latest, sizeof(latest), "%s", m.version);
  fwNewer = otaCompareVersions(m.version, curVersion) > 0;
  sprNeeded = decideSprites(m);
  // sprites first: a new firmware may already rely on them
  if (sprNeeded && !installSprites(m)) return;
  if (fwNewer) {
    if (!installFirmware(m)) return;
    otaPct = 100;
    otaSt = OTA_REBOOT;  // the main loop saves the game and restarts
    bump();
    return;
  }
  available = false;
  otaSt = spritesChanged ? OTA_SPRITES_DONE : OTA_UP_TO_DATE;
  bump();
}

static void task(void *arg) {
  Job j = (Job)(uintptr_t)arg;
  radioOn();
  switch (j) {
    case J_SCAN: jobScan(); break;
    case J_JOIN: jobJoin(); break;
    case J_CHECK: jobCheck(checkManual); break;
    case J_INSTALL: jobInstall(); break;
    case J_TIME: jobTime(); break;
  }
  if (otaSt != OTA_REBOOT) radioOff();
  busy = false;
  bump();
  vTaskDelete(nullptr);
}

static bool startJob(Job j) {
  if (__atomic_exchange_n(&busy, true, __ATOMIC_ACQ_REL)) return false;
  // TLS needs a large stack; the task only lives for the duration of the job
  if (xTaskCreatePinnedToCore(task, "net", 16384, (void *)(uintptr_t)j, 1, nullptr, 0) != pdPASS) {
    busy = false;
    return false;
  }
  return true;
}

bool netStartScan() {
  if (busy) return false;
  scanSt = SCAN_BUSY;
  bump();
  if (!startJob(J_SCAN)) { scanSt = SCAN_FAILED; return false; }
  return true;
}

bool netStartJoin(const char *ssid, const char *pass) {
  if (busy || !ssid || !ssid[0] || strlen(ssid) > NET_SSID_LEN || (pass && strlen(pass) > NET_PASS_LEN)) return false;
  snprintf(joinSsid, sizeof(joinSsid), "%s", ssid);
  snprintf(joinPass, sizeof(joinPass), "%s", pass ? pass : "");
  joinSt = JOIN_BUSY;
  bump();
  if (!startJob(J_JOIN)) { joinSt = JOIN_FAILED; return false; }
  return true;
}

bool otaStartCheck(bool manual) {
  if (busy || otaSt == OTA_REBOOT) return false;
  if (!saved.count) {
    if (manual) fail(OTA_ERR_NO_NETWORK);
    return false;
  }
  checkManual = manual;
  otaSt = OTA_CONNECTING;
  bump();
  return startJob(J_CHECK);
}

bool otaStartInstall() {
  if (busy || otaSt == OTA_REBOOT) return false;
  if (!saved.count) { fail(OTA_ERR_NO_NETWORK); return false; }
  otaSt = OTA_CONNECTING;
  otaPct = 0;
  bump();
  return startJob(J_INSTALL);
}

void netLoop(uint32_t now) {
  // A new firmware is confirmed only once it has run for a while with enough
  // memory left; until then a reset or crash brings back the previous one.
  if (!confirmed && now > CONFIRM_AFTER_MS) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
      confirmed = true;
    } else if (heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) >= CONFIRM_MIN_HEAP) {
      confirmed = true;
      esp_ota_mark_app_valid_cancel_rollback();
      Serial.printf("OTA firmware %s confirmed\n", curVersion);
    } else if (now > CONFIRM_AFTER_MS * 4) {
      // memory stays too low: refuse this firmware and go back
      Serial.printf("OTA firmware %s: low memory, rolling back\n", curVersion);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
  if (!guardCleared && now > GUARD_CLEAR_MS) {
    guardCleared = true;
    guardCrashes = 0;
  }
  if (tzZone != TZ_OFF && saved.count && !busy && (int32_t)(now - nextTime) >= 0) {
    nextTime = now + TIME_EVERY_MS;
    // the automatic update check (due right after start-up) syncs the clock as well
    bool checkDue = autoCheck && (int32_t)(now + 10000 - nextAuto) >= 0;
    if (!checkDue) startJob(J_TIME);
  }
  if (autoCheck && saved.count && !busy && (int32_t)(now - nextAuto) >= 0) {
    nextAuto = now + AUTO_EVERY_MS;
    if (otaSt != OTA_SPRITES && otaSt != OTA_FIRMWARE && otaSt != OTA_REBOOT) otaStartCheck(false);
  }
}
