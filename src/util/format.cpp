#include "util/format.h"

#include <cstdio>
#include <cstring>

namespace util::format {

void thousands(int32_t value, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  // Magnitude as int64 so INT32_MIN negates safely.
  const long long magnitude = value < 0 ? -static_cast<long long>(value) : value;
  char digits[24];
  snprintf(digits, sizeof(digits), "%lld", magnitude);
  const size_t n = strlen(digits);

  size_t o = 0;
  if (value < 0 && o + 1 < out_len) {
    out[o++] = '-';
  }
  for (size_t i = 0; i < n && o + 1 < out_len; ++i) {
    out[o++] = digits[i];
    const size_t left = n - i - 1;
    if (left > 0 && left % 3 == 0 && o + 1 < out_len) {
      out[o++] = ',';
    }
  }
  out[o] = '\0';
}

void flightNumber(const char* callsign, const char* iata, char* out,
                  size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  if (callsign == nullptr) {
    out[0] = '\0';
    return;
  }
  if (iata != nullptr && iata[0] != '\0' && strlen(callsign) > 3) {
    snprintf(out, out_len, "%s%s", iata, callsign + 3);
  } else {
    snprintf(out, out_len, "%s", callsign);
  }
}

bool routeLeg(const char* codes, const char* airport, bool arriving, char* out,
              size_t out_len) {
  if (codes == nullptr || airport == nullptr || out == nullptr || out_len == 0) {
    return false;
  }
  // Split "SNA-ORD-IAD" into stops.
  constexpr size_t kMaxStops = 6;
  char stops[kMaxStops][5] = {};
  size_t n = 0;
  for (const char* p = codes; *p != '\0' && n < kMaxStops;) {
    const size_t len = strcspn(p, "-");
    if (len == 0 || len >= sizeof(stops[0])) {
      return false;
    }
    memcpy(stops[n], p, len);
    stops[n][len] = '\0';
    ++n;
    p += len;
    if (*p == '-') {
      ++p;
    }
  }
  if (n < 2) {
    return false;
  }
  if (arriving) {
    for (size_t i = n; i-- > 1;) {
      if (strcmp(stops[i], airport) == 0) {
        snprintf(out, out_len, "%s-%s", stops[i - 1], stops[i]);
        return true;
      }
    }
  } else {
    for (size_t i = 0; i + 1 < n; ++i) {
      if (strcmp(stops[i], airport) == 0) {
        snprintf(out, out_len, "%s-%s", stops[i], stops[i + 1]);
        return true;
      }
    }
  }
  return false;
}

}  // namespace util::format
