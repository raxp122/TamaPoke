#include "netlogic.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Saved networks
// ---------------------------------------------------------------------------

void netListInit(NetList &l) { memset(&l, 0, sizeof(l)); }

int netListFind(const NetList &l, const char *ssid) {
  if (!ssid) return -1;
  for (int i = 0; i < l.count && i < NET_MAX; i++)
    if (!strcmp(l.e[i].ssid, ssid)) return i;
  return -1;
}

void netListTouch(NetList &l, int idx) {
  if (idx < 0 || idx >= l.count) return;
  l.e[idx].lastUsed = ++l.clock;
}

int netListUpsert(NetList &l, const char *ssid, const char *pass) {
  if (!ssid || !ssid[0] || strlen(ssid) > NET_SSID_LEN) return -1;
  if (!pass) pass = "";
  if (strlen(pass) > NET_PASS_LEN) return -1;
  int idx = netListFind(l, ssid);
  if (idx < 0) {
    if (l.count < NET_MAX) {
      idx = l.count++;
    } else {
      idx = 0;  // full: replace the least recently used
      for (int i = 1; i < NET_MAX; i++)
        if (l.e[i].lastUsed < l.e[idx].lastUsed) idx = i;
    }
    memset(&l.e[idx], 0, sizeof(NetEntry));
    strcpy(l.e[idx].ssid, ssid);
  }
  memset(l.e[idx].pass, 0, sizeof(l.e[idx].pass));
  strcpy(l.e[idx].pass, pass);
  netListTouch(l, idx);
  return idx;
}

bool netListRemove(NetList &l, int idx) {
  if (idx < 0 || idx >= l.count) return false;
  for (int i = idx; i + 1 < l.count; i++) l.e[i] = l.e[i + 1];
  l.count--;
  memset(&l.e[l.count], 0, sizeof(NetEntry));
  return true;
}

int netListOrder(const NetList &l, int *out) {
  int n = 0;
  for (int i = 0; i < l.count && i < NET_MAX; i++) out[n++] = i;
  // insertion sort, most recently used first
  for (int i = 1; i < n; i++) {
    int v = out[i], j = i - 1;
    while (j >= 0 && l.e[out[j]].lastUsed < l.e[v].lastUsed) { out[j + 1] = out[j]; j--; }
    out[j + 1] = v;
  }
  return n;
}

int netListCandidates(const NetList &l, const char *const *ssids, const int8_t *rssi,
                      int n, int *out, int maxOut) {
  int best[NET_MAX];
  int8_t bestRssi[NET_MAX];
  int cnt = 0;
  for (int k = 0; k < l.count && k < NET_MAX; k++) {
    // strongest scan result for this saved network (several access points can share an SSID)
    bool found = false;
    int8_t r = -128;
    for (int i = 0; i < n; i++) {
      if (ssids[i] && !strcmp(ssids[i], l.e[k].ssid) && (!found || rssi[i] > r)) {
        r = rssi[i];
        found = true;
      }
    }
    if (!found) continue;
    int j = cnt++;
    while (j > 0 && bestRssi[j - 1] < r) { best[j] = best[j - 1]; bestRssi[j] = bestRssi[j - 1]; j--; }
    best[j] = k;
    bestRssi[j] = r;
  }
  int w = cnt < maxOut ? cnt : maxOut;
  for (int i = 0; i < w; i++) out[i] = best[i];
  return w;
}

// blob: [version][count][clock u32 LE][NET_MAX x (ssid 33, pass 65, lastUsed u32 LE)]
static const size_t NET_ENTRY_BLOB = (NET_SSID_LEN + 1) + (NET_PASS_LEN + 1) + 4;

size_t netListBlobSize() { return 2 + 4 + NET_MAX * NET_ENTRY_BLOB; }

static void putU32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static uint32_t getU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void netListToBlob(const NetList &l, uint8_t *blob) {
  memset(blob, 0, netListBlobSize());
  blob[0] = NET_BLOB_VERSION;
  blob[1] = l.count;
  putU32(blob + 2, l.clock);
  uint8_t *p = blob + 6;
  for (int i = 0; i < NET_MAX; i++, p += NET_ENTRY_BLOB) {
    if (i >= l.count) continue;
    memcpy(p, l.e[i].ssid, NET_SSID_LEN + 1);
    memcpy(p + NET_SSID_LEN + 1, l.e[i].pass, NET_PASS_LEN + 1);
    putU32(p + NET_SSID_LEN + 1 + NET_PASS_LEN + 1, l.e[i].lastUsed);
  }
}

bool netListFromBlob(NetList &l, const uint8_t *blob, size_t len) {
  netListInit(l);
  if (!blob || len != netListBlobSize() || blob[0] != NET_BLOB_VERSION || blob[1] > NET_MAX) return false;
  const uint8_t *p = blob + 6;
  uint8_t n = blob[1];
  for (int i = 0; i < n; i++, p += NET_ENTRY_BLOB) {
    NetEntry &e = l.e[l.count];
    memcpy(e.ssid, p, NET_SSID_LEN + 1);
    memcpy(e.pass, p + NET_SSID_LEN + 1, NET_PASS_LEN + 1);
    e.ssid[NET_SSID_LEN] = 0;
    e.pass[NET_PASS_LEN] = 0;
    e.lastUsed = getU32(p + NET_SSID_LEN + 1 + NET_PASS_LEN + 1);
    if (!e.ssid[0]) { memset(&e, 0, sizeof(e)); continue; }  // damaged entry: skip it
    l.count++;
  }
  l.clock = getU32(blob + 2);
  for (int i = 0; i < l.count; i++)
    if (l.e[i].lastUsed > l.clock) l.clock = l.e[i].lastUsed;
  return true;
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

static void parseVer(const char *s, long v[3]) {
  v[0] = v[1] = v[2] = 0;
  if (!s) return;
  if (*s == 'v' || *s == 'V') s++;
  for (int i = 0; i < 3 && *s >= '0' && *s <= '9'; i++) {
    char *end;
    v[i] = strtol(s, &end, 10);
    s = end;
    if (*s != '.') break;
    s++;
  }
}

int otaCompareVersions(const char *a, const char *b) {
  long x[3], y[3];
  parseVer(a, x);
  parseVer(b, y);
  for (int i = 0; i < 3; i++)
    if (x[i] != y[i]) return x[i] > y[i] ? 1 : -1;
  return 0;
}

// points just after `"key"` followed by optional spaces and ':'; NULL if missing
static const char *jsonFindValue(const char *json, const char *key) {
  if (!json || !key) return nullptr;
  size_t kl = strlen(key);
  for (const char *p = json; (p = strchr(p, '"')) != nullptr; p++) {
    if (!strncmp(p + 1, key, kl) && p[1 + kl] == '"') {
      const char *q = p + 2 + kl;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
      if (*q != ':') continue;
      q++;
      while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
      return q;
    }
  }
  return nullptr;
}

bool jsonGetString(const char *json, const char *key, char *out, size_t outLen) {
  const char *v = jsonFindValue(json, key);
  if (!v || *v != '"' || outLen == 0) return false;
  v++;
  const char *e = strchr(v, '"');
  if (!e || (size_t)(e - v) >= outLen) return false;
  memcpy(out, v, e - v);
  out[e - v] = 0;
  return true;
}

bool jsonGetUInt(const char *json, const char *key, uint32_t *out) {
  const char *v = jsonFindValue(json, key);
  if (!v || *v < '0' || *v > '9') return false;
  char *end;
  unsigned long n = strtoul(v, &end, 10);
  *out = (uint32_t)n;
  return true;
}

static bool isHex64(const char *s) {
  if (strlen(s) != 64) return false;
  for (const char *p = s; *p; p++)
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
  return true;
}

bool otaParseManifest(const char *json, OtaManifest &m) {
  memset(&m, 0, sizeof(m));
  if (!jsonGetString(json, "version", m.version, sizeof(m.version)) || !m.version[0]) return false;
  if (m.version[0] < '0' || m.version[0] > '9') return false;
  if (!jsonGetUInt(json, "app_size", &m.appSize) || m.appSize == 0) return false;
  if (!jsonGetString(json, "app_sha256", m.appSha, sizeof(m.appSha)) || !isHex64(m.appSha)) return false;
  // the sprite pack is optional: without it only the firmware is updated
  if (jsonGetUInt(json, "sprites_size", &m.spritesSize) && m.spritesSize) {
    if (!jsonGetString(json, "sprites_sha256", m.spritesSha, sizeof(m.spritesSha)) || !isHex64(m.spritesSha)) return false;
  } else {
    m.spritesSize = 0;
    m.spritesSha[0] = 0;
  }
  return true;
}

// ---------------------------------------------------------------------------
// TPAK stream parser
// ---------------------------------------------------------------------------

bool tpakNameValid(const char *name) {
  if (!name || strncmp(name, "mons/", 5)) return false;
  const char *f = name + 5;
  if (!*f || strstr(f, "..")) return false;
  for (const char *p = f; *p; p++) {
    char c = *p;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
      return false;
  }
  return true;
}

TpakParser::TpakParser(TpakEntry *entries, TpakSink *s)
    : ents(entries), sink(s), st(HEADER), hdrLen(0), total(0), cur(0), idxStage(0),
      nameLen(0), namePos(0), sizePos(0), sizeAcc(0), remaining(0), err("") {}

bool TpakParser::fail(const char *e) {
  err = e;
  st = ERROR;
  return false;
}

bool TpakParser::feed(const uint8_t *data, size_t len) {
  size_t i = 0;
  while (i < len) {
    switch (st) {
      case HEADER:
        hdr[hdrLen++] = data[i++];
        if (hdrLen == 6) {
          if (memcmp(hdr, "TPAK", 4)) return fail("bad magic");
          total = (uint16_t)(hdr[4] | (hdr[5] << 8));
          if (total == 0 || total > TPAK_MAX_FILES) return fail("bad count");
          st = INDEX;
          cur = 0;
          idxStage = 0;
        }
        break;
      case INDEX: {
        uint8_t b = data[i++];
        if (idxStage == 0) {
          nameLen = b;
          if (nameLen == 0 || nameLen > TPAK_NAME_MAX) return fail("bad name");
          namePos = 0;
          idxStage = 1;
        } else if (idxStage == 1) {
          ents[cur].name[namePos++] = (char)b;
          if (namePos == nameLen) {
            ents[cur].name[namePos] = 0;
            if (!tpakNameValid(ents[cur].name)) return fail("bad name");
            idxStage = 2;
            sizePos = 0;
            sizeAcc = 0;
          }
        } else {
          sizeAcc |= (uint32_t)b << (8 * sizePos);
          if (++sizePos == 4) {
            ents[cur].size = sizeAcc;
            idxStage = 0;
            if (++cur == total) {
              if (!sink->onIndex(ents, total)) { st = STOPPED; return false; }
              cur = 0;
              remaining = ents[0].size;
              st = DATA;
              // empty files have no data bytes: close them right away
              while (st == DATA && remaining == 0) {
                if (!sink->onFileEnd(cur)) { st = STOPPED; return false; }
                if (++cur == total) st = DONE;
                else remaining = ents[cur].size;
              }
            }
          }
        }
        break;
      }
      case DATA: {
        size_t n = len - i;
        if (n > remaining) n = remaining;
        if (!sink->onFileData(cur, data + i, n)) { st = STOPPED; return false; }
        i += n;
        remaining -= n;
        while (st == DATA && remaining == 0) {
          if (!sink->onFileEnd(cur)) { st = STOPPED; return false; }
          if (++cur == total) st = DONE;
          else remaining = ents[cur].size;
        }
        break;
      }
      case DONE:
        return fail("trailing data");
      default:
        return false;
    }
  }
  return st != ERROR && st != STOPPED;
}

// ---------------------------------------------------------------------------
// Time zones
// ---------------------------------------------------------------------------

const TzZone TZ_ZONES[] = {
  { "OFF", 0, TZDST_NONE },
  { "UTC+0 UK/PT", 0, TZDST_EU },
  { "UTC+1 CET", 60, TZDST_EU },
  { "UTC+2 EET", 120, TZDST_EU },
  { "UTC+3 MSK/TR", 180, TZDST_NONE },
  { "UTC", 0, TZDST_NONE },
  { "UTC-3 BR/AR", -180, TZDST_NONE },
  { "UTC-5 US EAST", -300, TZDST_US },
  { "UTC-6 US CENT", -360, TZDST_US },
  { "UTC-7 US MOUNT", -420, TZDST_US },
  { "UTC-8 US PACIF", -480, TZDST_US },
  { "UTC+4 DUBAI", 240, TZDST_NONE },
  { "UTC+5:30 INDIA", 330, TZDST_NONE },
  { "UTC+7 BANGKOK", 420, TZDST_NONE },
  { "UTC+8 CN/SG", 480, TZDST_NONE },
  { "UTC+9 JP/KR", 540, TZDST_NONE },
  { "UTC+10 SYDNEY", 600, TZDST_AU },
};
const uint8_t TZ_COUNT = sizeof(TZ_ZONES) / sizeof(TZ_ZONES[0]);

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
static int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = (uint32_t)(y - era * 400);
  uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

static int32_t yearOf(uint32_t epoch) {
  int32_t z = (int32_t)(epoch / 86400) + 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  uint32_t m = mp < 10 ? mp + 3 : mp - 9;
  return (int32_t)yoe + era * 400 + (m <= 2);
}

// n-th (1..5, 5 = last) Sunday of a month, as days since 1970
static int32_t nthSunday(int32_t y, uint32_t m, int n) {
  int32_t first = daysFromCivil(y, m, 1);
  int32_t wd = (first + 4) % 7;  // 1970-01-01 was a Thursday; 0 = Sunday
  if (wd < 0) wd += 7;
  int32_t sunday = first + (7 - wd) % 7 + (n - 1) * 7;
  if (n == 5) {
    int32_t next = daysFromCivil(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1);
    while (sunday >= next) sunday -= 7;
  }
  return sunday;
}

int32_t tzOffsetSeconds(uint8_t zone, uint32_t utc) {
  if (zone == TZ_OFF || zone >= TZ_COUNT) return 0;
  const TzZone &z = TZ_ZONES[zone];
  int32_t std = z.stdOffsetMin * 60;
  int32_t y = yearOf(utc);
  int64_t t = utc;
  bool dst = false;
  switch (z.dst) {
    case TZDST_EU: {  // last Sunday of March 01:00 UTC to last Sunday of October 01:00 UTC
      int64_t a = (int64_t)nthSunday(y, 3, 5) * 86400 + 3600;
      int64_t b = (int64_t)nthSunday(y, 10, 5) * 86400 + 3600;
      dst = t >= a && t < b;
      break;
    }
    case TZDST_US: {  // second Sunday of March 02:00 local to first Sunday of November 02:00 local
      int64_t a = (int64_t)nthSunday(y, 3, 2) * 86400 + 7200 - std;
      int64_t b = (int64_t)nthSunday(y, 11, 1) * 86400 + 7200 - (std + 3600);
      dst = t >= a && t < b;
      break;
    }
    case TZDST_AU: {  // first Sunday of October 02:00 local to first Sunday of April 03:00 local (southern summer)
      int64_t end = (int64_t)nthSunday(y, 4, 1) * 86400 + 10800 - (std + 3600);
      int64_t start = (int64_t)nthSunday(y, 10, 1) * 86400 + 7200 - std;
      dst = t < end || t >= start;
      break;
    }
    default: break;
  }
  return std + (dst ? 3600 : 0);
}

int32_t tzOffsetSecondsMode(uint8_t zone, uint32_t utc, uint8_t dstMode) {
  if (zone == TZ_OFF || zone >= TZ_COUNT) return 0;
  int32_t std = TZ_ZONES[zone].stdOffsetMin * 60;
  if (dstMode == DSTM_ON) return std + 3600;
  if (dstMode == DSTM_OFF) return std;
  return tzOffsetSeconds(zone, utc);
}
