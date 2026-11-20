#pragma once
// In-memory NVS: one map per namespace, shared by every Preferences object.
#include <Arduino.h>
#include <map>
#include <string>
#include <vector>

class Preferences {
 public:
  bool begin(const char *ns, bool readOnly = false);
  void end() {}
  bool clear();
  bool remove(const char *key);
  bool isKey(const char *key);
  size_t putBool(const char *k, bool v) { return put(k, v ? 1 : 0); }
  size_t putUChar(const char *k, uint8_t v) { return put(k, v); }
  size_t putChar(const char *k, int8_t v) { return put(k, v); }
  size_t putUShort(const char *k, uint16_t v) { return put(k, v); }
  size_t putShort(const char *k, int16_t v) { return put(k, v); }
  size_t putUInt(const char *k, uint32_t v) { return put(k, v); }
  size_t putInt(const char *k, int32_t v) { return put(k, v); }
  size_t putULong(const char *k, uint32_t v) { return put(k, v); }
  bool getBool(const char *k, bool d = false) { return get(k, d) != 0; }
  uint8_t getUChar(const char *k, uint8_t d = 0) { return (uint8_t)get(k, d); }
  int8_t getChar(const char *k, int8_t d = 0) { return (int8_t)get(k, d); }
  uint16_t getUShort(const char *k, uint16_t d = 0) { return (uint16_t)get(k, d); }
  int16_t getShort(const char *k, int16_t d = 0) { return (int16_t)get(k, d); }
  uint32_t getUInt(const char *k, uint32_t d = 0) { return (uint32_t)get(k, d); }
  int32_t getInt(const char *k, int32_t d = 0) { return (int32_t)get(k, d); }
  uint32_t getULong(const char *k, uint32_t d = 0) { return (uint32_t)get(k, d); }
  size_t putBytes(const char *k, const void *v, size_t n);
  size_t getBytesLength(const char *k);
  size_t getBytes(const char *k, void *buf, size_t n);
  size_t putString(const char *k, const char *v);
  size_t putString(const char *k, const String &v) { return putString(k, v.c_str()); }
  String getString(const char *k, const String &d = String());
  size_t getString(const char *k, char *buf, size_t n);

 private:
  size_t put(const char *k, long long v);
  long long get(const char *k, long long d);
  std::string ns_;
};
