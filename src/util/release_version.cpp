#include "util/release_version.h"

namespace util::release_version {

namespace {

constexpr uint32_t kMaxPart = 65535;

bool isDigit(char c) { return c >= '0' && c <= '9'; }

/**
 * Read one decimal part at `p` and advance past it. Rejects an empty part, a
 * leading zero and anything above kMaxPart; the digit count is capped before
 * the value can overflow.
 */
bool readPart(const char*& p, uint16_t& out) {
  if (!isDigit(*p)) {
    return false;
  }
  if (*p == '0' && isDigit(p[1])) {
    return false;
  }
  uint32_t value = 0;
  int digits = 0;
  while (isDigit(*p)) {
    if (++digits > 5) {
      return false;
    }
    value = value * 10 + static_cast<uint32_t>(*p - '0');
    ++p;
  }
  if (value > kMaxPart) {
    return false;
  }
  out = static_cast<uint16_t>(value);
  return true;
}

}  // namespace

bool parse(const char* text, Version& out) {
  if (text == nullptr || *text != 'v') {
    return false;
  }
  const char* p = text + 1;
  Version v{};
  if (!readPart(p, v.major) || *p++ != '.') {
    return false;
  }
  if (!readPart(p, v.minor) || *p++ != '.') {
    return false;
  }
  if (!readPart(p, v.patch) || *p != '\0') {
    return false;
  }
  out = v;
  return true;
}

int compare(const Version& a, const Version& b) {
  if (a.major != b.major) {
    return a.major < b.major ? -1 : 1;
  }
  if (a.minor != b.minor) {
    return a.minor < b.minor ? -1 : 1;
  }
  if (a.patch != b.patch) {
    return a.patch < b.patch ? -1 : 1;
  }
  return 0;
}

bool shouldInstall(const char* running, const char* offered) {
  Version have{};
  Version want{};
  if (!parse(running, have) || !parse(offered, want)) {
    return false;
  }
  return compare(want, have) > 0;
}

}  // namespace util::release_version
