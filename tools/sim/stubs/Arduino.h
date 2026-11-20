#pragma once
// Host stand-in for the Arduino core: just enough for the TamaPoke sketch to
// compile and run on a PC inside the screen simulator (tools/sim).
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define IRAM_ATTR
#define PROGMEM
#define F(x) x
#define INPUT 0x01
#define INPUT_PULLUP 0x05
#define OUTPUT 0x03
#define LOW 0
#define HIGH 1
#define FALLING 0x02
#define RISING 0x01
#define digitalPinToInterrupt(p) (p)
#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define TWO_PI 6.283185307179586476925286766559
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))

typedef bool boolean;
typedef uint8_t byte;

// ---- time (driven by the simulator) ----
extern uint32_t gSimMillis;
inline uint32_t millis() { return gSimMillis; }
inline uint64_t micros() { return (uint64_t)gSimMillis * 1000ULL; }
inline void delay(uint32_t ms) { gSimMillis += ms; }
inline void yield() {}

// ---- misc ----
inline void pinMode(int, int) {}
inline int digitalRead(int) { return HIGH; }
inline void digitalWrite(int, int) {}
inline void attachInterrupt(int, void (*)(), int) {}
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);
inline void *ps_malloc(size_t n) { return malloc(n); }
inline void *ps_calloc(size_t n, size_t s) { return calloc(n, s); }

template <typename T, typename U>
inline auto min(T a, U b) -> decltype(a < b ? a : b) { return a < b ? a : b; }
template <typename T, typename U>
inline auto max(T a, U b) -> decltype(a > b ? a : b) { return a > b ? a : b; }
template <typename T, typename L, typename H>
inline T constrain(T v, L lo, H hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ---- String (subset of the Arduino API used by the sketch) ----
class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  String(char c) : s_(1, c) {}
  String(int v) : s_(std::to_string(v)) {}
  String(unsigned v) : s_(std::to_string(v)) {}
  String(long v) : s_(std::to_string(v)) {}
  String(unsigned long v) : s_(std::to_string(v)) {}
  const char *c_str() const { return s_.c_str(); }
  unsigned length() const { return (unsigned)s_.size(); }
  bool startsWith(const String &p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
  bool endsWith(const String &p) const {
    return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0;
  }
  int indexOf(char c, unsigned from = 0) const { auto i = s_.find(c, from); return i == std::string::npos ? -1 : (int)i; }
  int indexOf(const String &t, unsigned from = 0) const { auto i = s_.find(t.s_, from); return i == std::string::npos ? -1 : (int)i; }
  int lastIndexOf(char c) const { auto i = s_.rfind(c); return i == std::string::npos ? -1 : (int)i; }
  String substring(unsigned from) const { return from >= s_.size() ? String() : String(s_.substr(from)); }
  String substring(unsigned from, unsigned to) const {
    if (from >= s_.size() || to <= from) return String();
    return String(s_.substr(from, to - from));
  }
  long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
  float toFloat() const { return strtof(s_.c_str(), nullptr); }
  void trim() {
    size_t a = s_.find_first_not_of(" \t\r\n");
    size_t b = s_.find_last_not_of(" \t\r\n");
    s_ = a == std::string::npos ? "" : s_.substr(a, b - a + 1);
  }
  void toUpperCase() { for (auto &c : s_) c = (char)toupper((unsigned char)c); }
  char operator[](unsigned i) const { return i < s_.size() ? s_[i] : 0; }
  char charAt(unsigned i) const { return (*this)[i]; }
  bool operator==(const String &o) const { return s_ == o.s_; }
  bool operator==(const char *o) const { return s_ == (o ? o : ""); }
  bool operator!=(const String &o) const { return s_ != o.s_; }
  String &operator+=(const String &o) { s_ += o.s_; return *this; }
  String &operator+=(const char *o) { s_ += o; return *this; }
  String &operator+=(char c) { s_ += c; return *this; }
  friend String operator+(const String &a, const String &b) { return String(a.s_ + b.s_); }
  friend String operator+(const String &a, const char *b) { return String(a.s_ + b); }
  friend String operator+(const char *a, const String &b) { return String(std::string(a) + b.s_); }
  bool equals(const String &o) const { return s_ == o.s_; }
  bool isEmpty() const { return s_.empty(); }

 private:
  std::string s_;
};

// ---- Print / Serial ----
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t *b, size_t n) {
    size_t w = 0;
    while (n--) w += write(*b++);
    return w;
  }
  size_t print(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
  size_t print(const String &s) { return print(s.c_str()); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(int v) { return printNum((long)v); }
  size_t print(unsigned v) { return printNum((unsigned long)v); }
  size_t print(long v) { return printNum(v); }
  size_t print(unsigned long v) { return printNum(v); }
  size_t print(double v, int digits = 2) {
    char b[32];
    snprintf(b, sizeof(b), "%.*f", digits, v);
    return print(b);
  }
  size_t println() { return print("\n"); }
  template <typename T>
  size_t println(T v) { size_t n = print(v); return n + print("\n"); }
  size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return print(b);
  }

 private:
  size_t printNum(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return print(b); }
  size_t printNum(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); return print(b); }
};

class SimSerial : public Print {
 public:
  void begin(unsigned long) {}
  void setRxBufferSize(size_t) {}
  void setTxTimeoutMs(uint32_t) {}
  void setTimeout(uint32_t) {}
  int available() { return 0; }
  int read() { return -1; }
  size_t readBytes(uint8_t *, size_t) { return 0; }
  size_t readBytes(char *, size_t) { return 0; }
  String readStringUntil(char) { return String(); }
  void flush() {}
  size_t write(uint8_t c) override;
  using Print::write;
  explicit operator bool() const { return true; }
};
extern SimSerial Serial;

class SimEsp {
 public:
  uint32_t getFreeHeap() { return 200000; }
  uint32_t getMinFreeHeap() { return 180000; }
  uint32_t getFreePsram() { return 6000000; }
  void restart();
};
extern SimEsp ESP;
