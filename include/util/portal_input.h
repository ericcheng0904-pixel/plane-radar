#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

/**
 * Parsers for values arriving from the Wi-Fi setup portal.
 *
 * Everything here is reachable by anyone who can open the portal, so each
 * parser rejects rather than guesses: a malformed field must leave the stored
 * setting untouched. Free of Arduino headers so the host test suite can cover
 * it (test/test_util).
 */
namespace util::portal {

/**
 * A browser submits a checkbox's value= attribute only when it is ticked, and
 * the portal prefills every checkbox with "T"; an unticked box arrives empty.
 * Only affirmative spellings count — treating any non-empty string as "on"
 * would turn a malformed or hostile POST into a silent settings change.
 */
inline bool checkboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  if (value[1] == '\0') {
    return value[0] == 'T' || value[0] == 't' || value[0] == '1';
  }
  return std::strcmp(value, "on") == 0 || std::strcmp(value, "true") == 0;
}

/**
 * Parse a 1-based step number from a portal number field into a 0-based index.
 * Returns false — leaving *out untouched — for anything that is not a plain
 * integer in 1..count, including trailing characters and overlong input.
 */
inline bool stepIndex(const char* value, uint8_t count, uint8_t* out) {
  if (value == nullptr || out == nullptr || count == 0) {
    return false;
  }

  // Hand-rolled rather than strtol: no locale, no overflow, no sign, and a
  // hard length bound regardless of what the field contained.
  uint32_t parsed = 0;
  size_t digits = 0;
  for (const char* p = value; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    if (++digits > 3) {
      return false;
    }
    parsed = parsed * 10 + static_cast<uint32_t>(*p - '0');
  }

  if (digits == 0 || parsed < 1 || parsed > count) {
    return false;
  }
  *out = static_cast<uint8_t>(parsed - 1);
  return true;
}

/**
 * Parse a portal number field that only accepts a fixed set of values, such as
 * a flash duration of 0, 3, 5 or 7 seconds. Returns false — leaving *out
 * untouched — for anything else.
 */
inline bool oneOf(const char* value, const uint8_t* allowed, size_t count,
                  uint8_t* out) {
  if (value == nullptr || allowed == nullptr || out == nullptr || count == 0) {
    return false;
  }

  uint32_t parsed = 0;
  size_t digits = 0;
  for (const char* p = value; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    if (++digits > 3) {
      return false;
    }
    parsed = parsed * 10 + static_cast<uint32_t>(*p - '0');
  }
  if (digits == 0) {
    return false;
  }

  for (size_t i = 0; i < count; ++i) {
    if (parsed == allowed[i]) {
      *out = allowed[i];
      return true;
    }
  }
  return false;
}

/** Longest radar name kept, in characters (bytes too: it is plain ASCII). */
constexpr size_t kMaxRadarName = 32;

/**
 * The plain stand-in for a phone keyboard's "smart" punctuation, given the
 * third byte of its UTF-8 sequence E2 80 xx; nullptr for anything else.
 * iPhones type ’ for ' by default, which must not make a name unsavable.
 */
inline const char* plainPunctuation(uint8_t third) {
  switch (third) {
    case 0x98:  // ‘
    case 0x99:  // ’
      return "'";
    case 0x9C:  // “
    case 0x9D:  // ”
      return "\"";
    case 0x93:  // – en dash
    case 0x94:  // — em dash ("--" on an iPhone)
      return "-";
    case 0xA6:  // …
      return "...";
    default:
      return nullptr;
  }
}

/**
 * The radar's own name from the settings form ("Ada's radar"), with the spaces
 * at either end trimmed and curly quotes, dashes and the ellipsis made plain
 * (plainPunctuation), into `out` (room for kMaxRadarName + 1). What is kept
 * is only space to '~': that is every character the round screen's font has,
 * so an accent or an emoji would be drawn as a gap. Anything else, or more
 * than kMaxRadarName characters once made plain, returns false and leaves
 * `out` untouched. Empty is valid: the radar has no name.
 */
inline bool radarName(const char* value, char* out, size_t len) {
  if (value == nullptr || out == nullptr || len <= kMaxRadarName) {
    return false;
  }
  // A hard bound on what is even looked at: the form's 32 characters are at
  // most 96 bytes, every one of them curly.
  constexpr size_t kMaxField = 128;
  const size_t n = strnlen(value, kMaxField + 1);
  if (n > kMaxField) {
    return false;
  }
  size_t first = 0;
  while (first < n && value[first] == ' ') {
    ++first;
  }
  size_t end = n;
  while (end > first && value[end - 1] == ' ') {
    --end;
  }
  char clean[kMaxRadarName + 1];
  size_t o = 0;
  for (size_t i = first; i < end;) {
    const auto c = static_cast<uint8_t>(value[i]);
    char single[2] = {static_cast<char>(c), '\0'};
    const char* plain = nullptr;
    size_t used = 1;
    if (c >= ' ' && c <= '~') {
      plain = single;
    } else if (c == 0xE2 && end - i >= 3 && static_cast<uint8_t>(value[i + 1]) == 0x80) {
      plain = plainPunctuation(static_cast<uint8_t>(value[i + 2]));
      used = 3;
    }
    const size_t plain_len = plain != nullptr ? std::strlen(plain) : 0;
    if (plain == nullptr || o + plain_len > kMaxRadarName) {
      return false;
    }
    std::memcpy(clean + o, plain, plain_len);
    o += plain_len;
    i += used;
  }
  clean[o] = '\0';
  std::memcpy(out, clean, o + 1);
  return true;
}

}  // namespace util::portal
