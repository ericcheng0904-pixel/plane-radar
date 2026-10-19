#include "util/coords.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace util::coords {

bool parseCoordinate(const char* text, double* out) {
  if (text == nullptr || out == nullptr || text[0] == '\0') {
    return false;
  }

  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || end == nullptr || *end != '\0') {
    return false;
  }
  if (!std::isfinite(value)) {
    return false;
  }

  *out = value;
  return true;
}

namespace {

constexpr int kMaxIntDigits = 3;
constexpr int kMaxFracDigits = 15;

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool isAlnum(char c) {
  return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

const char* skipSpaces(const char* p) {
  while (isSpace(*p)) {
    ++p;
  }
  return p;
}

/** One number of a pair, with how it was written. */
struct Part {
  double value = 0.0;
  bool has_sign = false;
  char hemisphere = 0;  // 'N', 'S', 'E', 'W' or 0
};

/**
 * Strict decimal at *p — [+-]digits[.digits] — and on success *p moves past
 * it. Only text of that shape ever reaches strtod, so "inf", hex floats and
 * exponents cannot slip through, and the digit limits bound the work.
 */
bool scanDecimal(const char** p, Part* out) {
  const char* s = *p;
  char buf[1 + kMaxIntDigits + 1 + kMaxFracDigits + 1];
  size_t n = 0;
  out->has_sign = false;
  if (*s == '+' || *s == '-') {
    out->has_sign = true;
    buf[n++] = *s++;
  }
  int int_digits = 0;
  while (isDigit(*s)) {
    if (++int_digits > kMaxIntDigits) {
      return false;
    }
    buf[n++] = *s++;
  }
  if (int_digits == 0) {
    return false;
  }
  if (*s == '.') {
    buf[n++] = *s++;
    int frac_digits = 0;
    while (isDigit(*s)) {
      if (++frac_digits > kMaxFracDigits) {
        return false;
      }
      buf[n++] = *s++;
    }
    if (frac_digits == 0) {
      return false;
    }
  }
  buf[n] = '\0';
  char* end = nullptr;
  const double value = std::strtod(buf, &end);
  if (end != buf + n || !std::isfinite(value)) {
    return false;
  }
  out->value = value;
  *p = s;
  return true;
}

/**
 * After a number: an optional degree sign, then an optional hemisphere letter
 * that stands alone ("41.88° N", "87.63W") — the "N" of "North" is not one.
 */
void scanHemisphere(const char** p, Part* out) {
  const char* s = *p;
  if (static_cast<unsigned char>(s[0]) == 0xC2 && static_cast<unsigned char>(s[1]) == 0xB0) {
    s += 2;  // UTF-8 "°"
  }
  const char* t = skipSpaces(s);
  char c = *t;
  if (c >= 'a' && c <= 'z') {
    c = static_cast<char>(c - 'a' + 'A');
  }
  if ((c == 'N' || c == 'S' || c == 'E' || c == 'W') && !isAlnum(t[1])) {
    out->hemisphere = c;
    *p = t + 1;
    return;
  }
  out->hemisphere = 0;
  *p = s;
}

/** Between the two numbers: a comma ("," or "%2C"), spaces, or a "+". */
bool scanSeparator(const char** p) {
  const char* s = skipSpaces(*p);
  bool comma = false;
  if (*s == ',') {
    comma = true;
    ++s;
  } else if (s[0] == '%' && s[1] == '2' && (s[2] == 'C' || s[2] == 'c')) {
    comma = true;
    s += 3;
  } else if (*s == '+') {
    comma = true;  // a space, as a query string writes it
    ++s;
  }
  s = skipSpaces(s);
  if (!comma && s == *p) {
    return false;  // the numbers touch: "41.88-87.63" is not a pair
  }
  *p = s;
  return true;
}

bool scanPair(const char** p, Part* a, Part* b) {
  const char* s = *p;
  if (!scanDecimal(&s, a)) {
    return false;
  }
  scanHemisphere(&s, a);
  if (!scanSeparator(&s) || !scanDecimal(&s, b)) {
    return false;
  }
  scanHemisphere(&s, b);
  *p = s;
  return true;
}

/** Signs from hemisphere letters, latitude first, range check. */
bool resolve(Part a, Part b, double* lat, double* lon) {
  if ((a.hemisphere == 0) != (b.hemisphere == 0)) {
    return false;  // "41.88 N, -87.63": half lettered, half signed
  }
  if (a.hemisphere != 0) {
    if (a.has_sign || b.has_sign) {
      return false;  // "-41.88 S" says south twice, or contradicts itself
    }
    const bool a_is_lat = a.hemisphere == 'N' || a.hemisphere == 'S';
    const bool b_is_lat = b.hemisphere == 'N' || b.hemisphere == 'S';
    if (a_is_lat == b_is_lat) {
      return false;  // two latitudes or two longitudes
    }
    if (a.hemisphere == 'S' || a.hemisphere == 'W') {
      a.value = -a.value;
    }
    if (b.hemisphere == 'S' || b.hemisphere == 'W') {
      b.value = -b.value;
    }
    if (!a_is_lat) {
      const Part t = a;
      a = b;
      b = t;
    }
  }
  if (!validLatLon(a.value, b.value)) {
    return false;
  }
  *lat = a.value;
  *lon = b.value;
  return true;
}

/** The whole text is the pair, perhaps in brackets and with spaces around. */
bool parsePlain(const char* text, double* lat, double* lon) {
  const char* s = skipSpaces(text);
  const bool bracket = *s == '(';
  if (bracket) {
    s = skipSpaces(s + 1);
  }
  Part a;
  Part b;
  if (!scanPair(&s, &a, &b)) {
    return false;
  }
  s = skipSpaces(s);
  if (bracket) {
    if (*s != ')') {
      return false;
    }
    s = skipSpaces(s + 1);
  }
  return *s == '\0' && resolve(a, b, lat, lon);
}

/** A link's "@lat,lon," map centre (Google Maps), first one that reads. */
bool parseAtCentre(const char* text, double* lat, double* lon) {
  for (const char* at = std::strchr(text, '@'); at != nullptr; at = std::strchr(at + 1, '@')) {
    const char* s = at + 1;
    Part a;
    Part b;
    if (!scanDecimal(&s, &a) || *s != ',') {
      continue;
    }
    ++s;
    if (!scanDecimal(&s, &b)) {
      continue;
    }
    if ((*s == ',' || *s == '/' || *s == '?' || *s == '\0') && resolve(a, b, lat, lon)) {
      return true;
    }
  }
  return false;
}

/** A link's "?q=lat,lon" (or query=, ll=, center=), up to the next '&' or '#'. */
bool parseQueryParam(const char* text, double* lat, double* lon) {
  static const char* const kKeys[] = {"q=", "query=", "ll=", "center="};
  for (const char* key : kKeys) {
    const size_t key_len = std::strlen(key);
    for (const char* hit = std::strstr(text, key); hit != nullptr;
         hit = std::strstr(hit + 1, key)) {
      if (hit == text || (hit[-1] != '?' && hit[-1] != '&')) {
        continue;  // "faq=" or the middle of a word
      }
      const char* s = hit + key_len;
      Part a;
      Part b;
      if (!scanPair(&s, &a, &b)) {
        continue;
      }
      if ((*s == '\0' || *s == '&' || *s == '#') && resolve(a, b, lat, lon)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

bool parseLatLonPair(const char* text, double* lat, double* lon) {
  if (text == nullptr || lat == nullptr || lon == nullptr) {
    return false;
  }
  const size_t len = strnlen(text, kMaxPairText + 1);
  if (len == 0 || len > kMaxPairText) {
    return false;
  }
  return parsePlain(text, lat, lon) || parseAtCentre(text, lat, lon) ||
         parseQueryParam(text, lat, lon);
}

}  // namespace util::coords
