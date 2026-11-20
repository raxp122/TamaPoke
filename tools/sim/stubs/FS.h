#pragma once
// microSD stand-in backed by a folder on the PC (see SD_MMC.h).
#include <Arduino.h>
#include <memory>
#include <string>
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace fs {
class File {
 public:
  File() {}
  File(const std::string &path, const char *mode);
  explicit operator bool() const { return (bool)h_; }
  size_t size() const;
  size_t read(uint8_t *buf, size_t n);
  int read();
  size_t write(const uint8_t *buf, size_t n);
  size_t write(uint8_t c) { return write(&c, 1); }
  bool seek(uint32_t pos);
  size_t position() const;
  int available() const;
  void close() { h_.reset(); }
  bool isDirectory() const;
  const char *name() const { return name_.c_str(); }
  const char *path() const { return path_.c_str(); }
  File openNextFile(const char *mode = FILE_READ);
  void rewindDirectory() { dirPos_ = 0; }

 private:
  struct Handle;
  std::shared_ptr<Handle> h_;
  std::string path_, name_;
  size_t dirPos_ = 0;
};
class FS {};
}  // namespace fs
using fs::File;
using fs::FS;
