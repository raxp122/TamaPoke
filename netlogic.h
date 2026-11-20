#pragma once
// Pure logic for Wi-Fi networks and OTA updates. No Arduino dependencies, so it
// is covered by the native tests in tests/net_tests.cpp.
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Saved Wi-Fi networks (up to NET_MAX, least recently used is evicted)
// ---------------------------------------------------------------------------
#define NET_MAX 10
#define NET_SSID_LEN 32
#define NET_PASS_LEN 64

struct NetEntry {
  char ssid[NET_SSID_LEN + 1];
  char pass[NET_PASS_LEN + 1];
  uint32_t lastUsed;  // logical clock; higher = more recently used
};

struct NetList {
  uint8_t count;
  uint32_t clock;
  NetEntry e[NET_MAX];
};

void netListInit(NetList &l);
int netListFind(const NetList &l, const char *ssid);
// Adds or updates a network and marks it as most recently used.
// When the list is full the least recently used entry is replaced.
// Returns the index of the entry, or -1 for an invalid SSID.
int netListUpsert(NetList &l, const char *ssid, const char *pass);
void netListTouch(NetList &l, int idx);
bool netListRemove(NetList &l, int idx);
// Order of the saved networks for display: most recently used first.
// Fills out[] with indices, returns count.
int netListOrder(const NetList &l, int *out);
// Among the scanned networks (ssids[i], rssi[i]) returns the saved indices
// sorted by signal strength (strongest first). Returns count written to out.
int netListCandidates(const NetList &l, const char *const *ssids, const int8_t *rssi,
                      int n, int *out, int maxOut);

// Serialisation for NVS (fixed-size blob with a version byte)
#define NET_BLOB_VERSION 1
size_t netListBlobSize();
void netListToBlob(const NetList &l, uint8_t *blob);
bool netListFromBlob(NetList &l, const uint8_t *blob, size_t len);

// ---------------------------------------------------------------------------
// Versions and version.json
// ---------------------------------------------------------------------------
// "1.37.12", "v1.36.2-hoenn-walk-local": first three numbers, the rest is ignored.
// > 0 when a is newer than b.
int otaCompareVersions(const char *a, const char *b);

// Minimal extraction from a flat JSON object ({"key":"value","n":123}).
bool jsonGetString(const char *json, const char *key, char *out, size_t outLen);
bool jsonGetUInt(const char *json, const char *key, uint32_t *out);

struct OtaManifest {
  char version[24];
  uint32_t appSize;
  char appSha[65];
  uint32_t spritesSize;
  char spritesSha[65];
};
bool otaParseManifest(const char *json, OtaManifest &m);

// ---------------------------------------------------------------------------
// TPAK stream parser (format of tools/pack_bundle.py)
//   char[4] "TPAK", uint16 count, count x {uint8 nameLen, name, uint32 size}, data...
// Feed bytes as they arrive; callbacks receive the index and the file data.
// ---------------------------------------------------------------------------
#define TPAK_MAX_FILES 1024
#define TPAK_NAME_MAX 47

struct TpakEntry {
  char name[TPAK_NAME_MAX + 1];
  uint32_t size;
};

class TpakSink {
 public:
  virtual ~TpakSink() {}
  // index complete; return false to stop parsing (for example when nothing has to be written)
  virtual bool onIndex(const TpakEntry *entries, uint16_t count) = 0;
  virtual bool onFileData(uint16_t idx, const uint8_t *data, size_t len) = 0;
  virtual bool onFileEnd(uint16_t idx) = 0;
};

class TpakParser {
 public:
  enum State : uint8_t { HEADER, INDEX, DATA, DONE, STOPPED, ERROR };
  // entries must hold TPAK_MAX_FILES elements (on the device it lives in PSRAM)
  TpakParser(TpakEntry *entries, TpakSink *sink);
  // returns false on a format error or when the sink stopped
  bool feed(const uint8_t *data, size_t len);
  State state() const { return st; }
  uint16_t count() const { return total; }
  const char *error() const { return err; }

 private:
  bool fail(const char *e);
  TpakEntry *ents;
  TpakSink *sink;
  State st;
  uint8_t hdr[6];
  uint8_t hdrLen;
  uint16_t total;
  uint16_t cur;
  // index entry being read
  uint8_t idxStage;  // 0 nameLen, 1 name, 2 size
  uint8_t nameLen, namePos, sizePos;
  uint32_t sizeAcc;
  uint32_t remaining;
  const char *err;
};

// Valid target name inside a pak: "mons/<file>", only [A-Za-z0-9._-], no "..".
bool tpakNameValid(const char *name);
