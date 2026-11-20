#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "netlogic.h"

static int failures = 0;

#define EXPECT_TRUE(expr) do { \
  if (!(expr)) { \
    std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": " << #expr << "\n"; \
    failures++; \
  } \
} while (0)

#define EXPECT_EQ(actual, expected) do { \
  auto a = (actual); \
  auto e = (expected); \
  if (!(a == e)) { \
    std::cerr << "FAIL: " << __FILE__ << ":" << __LINE__ << ": " << #actual \
              << " was " << +a << ", expected " << +e << "\n"; \
    failures++; \
  } \
} while (0)

static void testUpsertAndFind() {
  NetList l;
  netListInit(l);
  EXPECT_EQ(netListUpsert(l, "Home", "secret123"), 0);
  EXPECT_EQ(netListUpsert(l, "Work", "pw"), 1);
  EXPECT_EQ(l.count, 2);
  EXPECT_EQ(netListFind(l, "Work"), 1);
  EXPECT_EQ(netListFind(l, "Nope"), -1);
  // same SSID updates the password, no duplicate
  EXPECT_EQ(netListUpsert(l, "Home", "newpass"), 0);
  EXPECT_EQ(l.count, 2);
  EXPECT_TRUE(!strcmp(l.e[0].pass, "newpass"));
  // open network (empty password) is allowed, empty SSID is not
  EXPECT_EQ(netListUpsert(l, "Cafe", ""), 2);
  EXPECT_EQ(netListUpsert(l, "", "x"), -1);
  std::string longSsid(33, 'a');
  EXPECT_EQ(netListUpsert(l, longSsid.c_str(), "x"), -1);
  std::string longPass(65, 'b');
  EXPECT_EQ(netListUpsert(l, "Ok", longPass.c_str()), -1);
  std::string maxPass(64, 'c');
  EXPECT_TRUE(netListUpsert(l, "Max", maxPass.c_str()) >= 0);
}

static void testEvictsLeastRecentlyUsed() {
  NetList l;
  netListInit(l);
  char name[8];
  for (int i = 0; i < NET_MAX; i++) {
    snprintf(name, sizeof(name), "N%d", i);
    netListUpsert(l, name, "pw");
  }
  EXPECT_EQ(l.count, NET_MAX);
  // N0 was used recently, so N1 is now the oldest
  netListTouch(l, netListFind(l, "N0"));
  netListUpsert(l, "Hotspot", "pw");
  EXPECT_EQ(l.count, NET_MAX);
  EXPECT_TRUE(netListFind(l, "N0") >= 0);
  EXPECT_EQ(netListFind(l, "N1"), -1);
  EXPECT_TRUE(netListFind(l, "Hotspot") >= 0);
}

static void testRemoveAndOrder() {
  NetList l;
  netListInit(l);
  netListUpsert(l, "A", "1");
  netListUpsert(l, "B", "2");
  netListUpsert(l, "C", "3");
  netListTouch(l, netListFind(l, "A"));
  int order[NET_MAX];
  EXPECT_EQ(netListOrder(l, order), 3);
  EXPECT_TRUE(!strcmp(l.e[order[0]].ssid, "A"));
  EXPECT_TRUE(!strcmp(l.e[order[1]].ssid, "C"));
  EXPECT_TRUE(!strcmp(l.e[order[2]].ssid, "B"));
  EXPECT_TRUE(netListRemove(l, netListFind(l, "C")));
  EXPECT_EQ(l.count, 2);
  EXPECT_EQ(netListFind(l, "C"), -1);
  EXPECT_TRUE(!strcmp(l.e[netListFind(l, "B")].pass, "2"));
  EXPECT_TRUE(!netListRemove(l, 5));
}

static void testCandidatesByRssi() {
  NetList l;
  netListInit(l);
  netListUpsert(l, "Home", "1");
  netListUpsert(l, "Work", "2");
  netListUpsert(l, "Phone", "3");
  const char *ssids[] = { "Neighbour", "Work", "Home", "Work", nullptr };
  int8_t rssi[] = { -30, -80, -60, -50, -10 };
  int out[NET_MAX];
  int n = netListCandidates(l, ssids, rssi, 5, out, NET_MAX);
  EXPECT_EQ(n, 2);
  // the stronger of the two "Work" access points wins over Home
  EXPECT_TRUE(!strcmp(l.e[out[0]].ssid, "Work"));
  EXPECT_TRUE(!strcmp(l.e[out[1]].ssid, "Home"));
  EXPECT_EQ(netListCandidates(l, ssids, rssi, 5, out, 1), 1);
  EXPECT_EQ(netListCandidates(l, ssids, rssi, 0, out, NET_MAX), 0);
}

static void testBlobRoundTrip() {
  NetList l;
  netListInit(l);
  netListUpsert(l, "Home", "secret");
  netListUpsert(l, "Work", "pa ss!\"#");
  netListTouch(l, 0);
  std::vector<uint8_t> blob(netListBlobSize());
  netListToBlob(l, blob.data());
  NetList r;
  EXPECT_TRUE(netListFromBlob(r, blob.data(), blob.size()));
  EXPECT_EQ(r.count, 2);
  EXPECT_TRUE(!strcmp(r.e[1].pass, "pa ss!\"#"));
  EXPECT_EQ(r.e[0].lastUsed, l.e[0].lastUsed);
  EXPECT_EQ(r.clock, l.clock);
  // newly added entries stay most recent after reload
  netListUpsert(r, "New", "x");
  EXPECT_TRUE(r.e[2].lastUsed > r.e[0].lastUsed);
  // corrupted or foreign blobs are rejected and give an empty list
  blob[0] = 99;
  EXPECT_TRUE(!netListFromBlob(r, blob.data(), blob.size()));
  EXPECT_EQ(r.count, 0);
  EXPECT_TRUE(!netListFromBlob(r, blob.data(), blob.size() - 1));
}

static void testVersionCompare() {
  EXPECT_TRUE(otaCompareVersions("1.37.5", "1.36.2") > 0);
  EXPECT_TRUE(otaCompareVersions("1.37.10", "1.37.9") > 0);
  EXPECT_TRUE(otaCompareVersions("1.37.9", "1.37.10") < 0);
  EXPECT_EQ(otaCompareVersions("1.36.2", "1.36.2-hoenn-walk-local"), 0);
  EXPECT_TRUE(otaCompareVersions("v2.0.0", "1.99.99") > 0);
  EXPECT_TRUE(otaCompareVersions("1.37.1", "") > 0);
}

static const char *MANIFEST =
  "{\n  \"version\": \"1.37.4\",\n  \"app_size\": 1543216,\n"
  "  \"app_sha256\": \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\n"
  "  \"sprites_size\": 112233445,\n"
  "  \"sprites_sha256\": \"fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210\"\n}\n";

static void testManifest() {
  OtaManifest m;
  EXPECT_TRUE(otaParseManifest(MANIFEST, m));
  EXPECT_TRUE(!strcmp(m.version, "1.37.4"));
  EXPECT_EQ(m.appSize, 1543216u);
  EXPECT_EQ(m.spritesSize, 112233445u);
  EXPECT_TRUE(!strncmp(m.spritesSha, "fedcba", 6));
  // firmware-only manifest
  EXPECT_TRUE(otaParseManifest("{\"version\":\"1.37.1\",\"app_size\":10,"
                               "\"app_sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}", m));
  EXPECT_EQ(m.spritesSize, 0u);
  // broken ones
  EXPECT_TRUE(!otaParseManifest("{\"version\":\"1.37.1\"}", m));
  EXPECT_TRUE(!otaParseManifest("{\"version\":\"x\",\"app_size\":10,"
                                "\"app_sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}", m));
  EXPECT_TRUE(!otaParseManifest("{\"version\":\"1.37.1\",\"app_size\":10,\"app_sha256\":\"XYZ\"}", m));
  EXPECT_TRUE(!otaParseManifest("<html>Not Found</html>", m));
}

// ---- TPAK ----

struct Collect : TpakSink {
  std::vector<std::string> names;
  std::vector<std::string> data;
  int ended = 0;
  bool stopAtIndex = false;
  bool onIndex(const TpakEntry *e, uint16_t count) override {
    for (int i = 0; i < count; i++) names.push_back(e[i].name);
    data.resize(count);
    return !stopAtIndex;
  }
  bool onFileData(uint16_t idx, const uint8_t *d, size_t len) override {
    data[idx].append((const char *)d, len);
    return true;
  }
  bool onFileEnd(uint16_t) override { ended++; return true; }
};

static std::vector<uint8_t> makePak(const std::vector<std::pair<std::string, std::string>> &files) {
  std::vector<uint8_t> p = { 'T', 'P', 'A', 'K', (uint8_t)files.size(), (uint8_t)(files.size() >> 8) };
  for (auto &f : files) {
    p.push_back((uint8_t)f.first.size());
    p.insert(p.end(), f.first.begin(), f.first.end());
    uint32_t s = f.second.size();
    for (int i = 0; i < 4; i++) p.push_back((s >> (8 * i)) & 0xFF);
  }
  for (auto &f : files) p.insert(p.end(), f.second.begin(), f.second.end());
  return p;
}

static TpakEntry gEntries[TPAK_MAX_FILES];

static void testTpakChunks() {
  auto pak = makePak({ { "mons/p001.bin", "hello" }, { "mons/empty.bin", "" }, { "mons/thumbs.bin", std::string(5000, 'z') } });
  // every chunk size, including byte by byte
  for (size_t chunk : { (size_t)1, (size_t)3, (size_t)7, (size_t)4096, pak.size() }) {
    Collect c;
    TpakParser p(gEntries, &c);
    bool ok = true;
    for (size_t i = 0; i < pak.size() && ok; i += chunk)
      ok = p.feed(pak.data() + i, std::min(chunk, pak.size() - i));
    EXPECT_TRUE(ok);
    EXPECT_EQ(p.state(), TpakParser::DONE);
    EXPECT_EQ(c.ended, 3);
    EXPECT_EQ(c.names.size(), (size_t)3);
    EXPECT_TRUE(c.data[0] == "hello");
    EXPECT_TRUE(c.data[1].empty());
    EXPECT_EQ(c.data[2].size(), (size_t)5000);
  }
}

static void testTpakRejects() {
  {
    Collect c;
    TpakParser p(gEntries, &c);
    const uint8_t bad[] = { 'X', 'P', 'A', 'K', 1, 0 };
    EXPECT_TRUE(!p.feed(bad, sizeof(bad)));
    EXPECT_EQ(p.state(), TpakParser::ERROR);
  }
  for (const char *name : { "../etc/x", "mons/../x", "other/p001.bin", "mons/a b", "mons/" }) {
    auto pak = makePak({ { name, "x" } });
    Collect c;
    TpakParser p(gEntries, &c);
    EXPECT_TRUE(!p.feed(pak.data(), pak.size()));
    EXPECT_EQ(p.state(), TpakParser::ERROR);
  }
  {
    auto pak = makePak({ { "mons/a.bin", "x" } });
    pak.push_back(0);
    Collect c;
    TpakParser p(gEntries, &c);
    EXPECT_TRUE(!p.feed(pak.data(), pak.size()));
  }
  {
    // the sink can stop right after the index (nothing to download)
    auto pak = makePak({ { "mons/a.bin", "abc" } });
    Collect c;
    c.stopAtIndex = true;
    TpakParser p(gEntries, &c);
    EXPECT_TRUE(!p.feed(pak.data(), pak.size()));
    EXPECT_EQ(p.state(), TpakParser::STOPPED);
    EXPECT_EQ(c.names.size(), (size_t)1);
    EXPECT_EQ(c.ended, 0);
  }
}

static void testTimeZones() {
  EXPECT_EQ(tzOffsetSeconds(TZ_OFF, 1767225600u), 0);
  // Italy: 2026-01-15 12:00 UTC winter +1h; 2026-07-15 summer +2h
  EXPECT_EQ(tzOffsetSeconds(2, 1768478400u), 3600);
  EXPECT_EQ(tzOffsetSeconds(2, 1784116800u), 7200);
  // EU switch 2026: Sunday 29 March 01:00 UTC (1774746000), Sunday 25 October 01:00 UTC (1792890000)
  EXPECT_EQ(tzOffsetSeconds(2, 1774746000u - 1), 3600);
  EXPECT_EQ(tzOffsetSeconds(2, 1774746000u), 7200);
  EXPECT_EQ(tzOffsetSeconds(2, 1792890000u - 1), 7200);
  EXPECT_EQ(tzOffsetSeconds(2, 1792890000u), 3600);
  // US Eastern 2026: DST from 8 March 07:00 UTC (1772953200) to 1 November 06:00 UTC (1793512800)
  EXPECT_EQ(tzOffsetSeconds(7, 1772953200u - 1), -5 * 3600);
  EXPECT_EQ(tzOffsetSeconds(7, 1772953200u), -4 * 3600);
  EXPECT_EQ(tzOffsetSeconds(7, 1793512800u - 1), -4 * 3600);
  EXPECT_EQ(tzOffsetSeconds(7, 1793512800u), -5 * 3600);
  // Sydney: January is summer (+11), July winter (+10)
  EXPECT_EQ(tzOffsetSeconds(16, 1768478400u), 11 * 3600);
  EXPECT_EQ(tzOffsetSeconds(16, 1784116800u), 10 * 3600);
  // India has half hours, no DST
  EXPECT_EQ(tzOffsetSeconds(12, 1784116800u), 330 * 60);
  EXPECT_EQ(tzOffsetSeconds(200, 1784116800u), 0);
}

int main() {
  testTimeZones();
  testUpsertAndFind();
  testEvictsLeastRecentlyUsed();
  testRemoveAndOrder();
  testCandidatesByRssi();
  testBlobRoundTrip();
  testVersionCompare();
  testManifest();
  testTpakChunks();
  testTpakRejects();
  if (failures) {
    std::cerr << failures << " net test(s) failed\n";
    return 1;
  }
  std::cout << "net tests OK\n";
  return 0;
}
