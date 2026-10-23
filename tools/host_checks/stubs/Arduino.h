#pragma once
// Host stub. Just enough Arduino for the portal page code to compile and run
// off the board; the firmware build never sees this file.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : value_(s != nullptr ? s : "") {}
  const char* c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  char operator[](size_t i) const { return value_[i]; }
  bool operator==(const char* other) const { return value_ == other; }
  bool operator!=(const char* other) const { return !(*this == other); }
  void trim() {
    const auto first = value_.find_first_not_of(" \t\r\n");
    const auto last = value_.find_last_not_of(" \t\r\n");
    value_ = first == std::string::npos ? "" : value_.substr(first, last - first + 1);
  }

 private:
  std::string value_;
};

struct HostSerial {
  void println(const char* text) { (void)text; }
  void printf(const char* format, ...) { (void)format; }
};
extern HostSerial Serial;

/** An hour and a bit after boot; a test may move the clock on. */
inline unsigned long g_host_millis = 3723000UL;
inline unsigned long millis() { return g_host_millis; }
