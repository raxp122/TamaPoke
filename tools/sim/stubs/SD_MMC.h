#pragma once
#include <FS.h>
class SDMMCFS : public fs::FS {
 public:
  bool setPins(int, int, int, int = -1, int = -1, int = -1) { return true; }
  bool begin(const char * = "/sdcard", bool = false, bool = false, int = 0, uint8_t = 5);
  File open(const char *path, const char *mode = FILE_READ);
  File open(const String &path, const char *mode = FILE_READ) { return open(path.c_str(), mode); }
  bool exists(const char *path);
  bool exists(const String &p) { return exists(p.c_str()); }
  bool mkdir(const char *path);
  bool mkdir(const String &p) { return mkdir(p.c_str()); }
  bool remove(const char *path);
  bool remove(const String &p) { return remove(p.c_str()); }
  bool rename(const char *a, const char *b);
  bool rename(const String &a, const String &b) { return rename(a.c_str(), b.c_str()); }
  bool rmdir(const char *path);
  bool rmdir(const String &p) { return rmdir(p.c_str()); }
  uint64_t cardSize() { return 32ULL << 30; }
  uint64_t totalBytes() { return 32ULL << 30; }
  uint64_t usedBytes() { return 1ULL << 30; }
};
extern SDMMCFS SD_MMC;
