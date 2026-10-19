#include "util/geocode.h"

#include <cstdio>
#include <cstring>

namespace util::geocode {

namespace {

struct UsState {
  const char* code;
  const char* name;
};

/**
 * GeoNames' admin1 names for the US. The District appears under two names
 * depending on the record, so it has two rows.
 */
constexpr UsState kUsStates[] = {
    {"AL", "Alabama"},        {"AK", "Alaska"},         {"AZ", "Arizona"},
    {"AR", "Arkansas"},       {"CA", "California"},     {"CO", "Colorado"},
    {"CT", "Connecticut"},    {"DE", "Delaware"},       {"FL", "Florida"},
    {"GA", "Georgia"},        {"HI", "Hawaii"},         {"ID", "Idaho"},
    {"IL", "Illinois"},       {"IN", "Indiana"},        {"IA", "Iowa"},
    {"KS", "Kansas"},         {"KY", "Kentucky"},       {"LA", "Louisiana"},
    {"ME", "Maine"},          {"MD", "Maryland"},       {"MA", "Massachusetts"},
    {"MI", "Michigan"},       {"MN", "Minnesota"},      {"MS", "Mississippi"},
    {"MO", "Missouri"},       {"MT", "Montana"},        {"NE", "Nebraska"},
    {"NV", "Nevada"},         {"NH", "New Hampshire"},  {"NJ", "New Jersey"},
    {"NM", "New Mexico"},     {"NY", "New York"},       {"NC", "North Carolina"},
    {"ND", "North Dakota"},   {"OH", "Ohio"},           {"OK", "Oklahoma"},
    {"OR", "Oregon"},         {"PA", "Pennsylvania"},   {"RI", "Rhode Island"},
    {"SC", "South Carolina"}, {"SD", "South Dakota"},   {"TN", "Tennessee"},
    {"TX", "Texas"},          {"UT", "Utah"},           {"VT", "Vermont"},
    {"VA", "Virginia"},       {"WA", "Washington"},     {"WV", "West Virginia"},
    {"WI", "Wisconsin"},      {"WY", "Wyoming"},        {"DC", "Washington, D.C."},
    {"DC", "District of Columbia"},
};

/** U+00C0..U+00FF without accents; '_' marks the two that are not letters. */
constexpr char kLatin1Fold[] = "AAAAAAACEEEEIIIIDNOOOOO_OUUUUYTsaaaaaaaceeeeiiiidnooooo_ouuuuyty";

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool equalsIgnoreCase(const char* a, const char* b) {
  for (; *a != '\0' && *b != '\0'; ++a, ++b) {
    if (lower(*a) != lower(*b)) {
      return false;
    }
  }
  return *a == '\0' && *b == '\0';
}

/** The `n` bytes at `a` (not terminated) spell `b`, ignoring case. */
bool spellsIgnoreCase(const char* a, size_t n, const char* b) {
  for (size_t i = 0; i < n; ++i) {
    if (b[i] == '\0' || lower(a[i]) != lower(b[i])) {
      return false;
    }
  }
  return b[n] == '\0';
}

/** Copy [begin, end) without surrounding spaces; false if empty or too long. */
bool copyTrimmed(const char* begin, const char* end, char* out, size_t len) {
  while (begin < end && *begin == ' ') {
    ++begin;
  }
  while (end > begin && end[-1] == ' ') {
    --end;
  }
  const size_t n = static_cast<size_t>(end - begin);
  if (n == 0 || n >= len) {
    return false;
  }
  std::memcpy(out, begin, n);
  out[n] = '\0';
  return true;
}

/** The two bytes at `code` are a state's code. */
bool isStateCode(const char* code) {
  for (const UsState& s : kUsStates) {
    if (spellsIgnoreCase(code, 2, s.code)) {
      return true;
    }
  }
  return false;
}

/** "60201" or "60201-1234". */
bool isUsZip(const char* s, size_t n) {
  if (n != 5 && n != 10) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const bool want_dash = i == 5;
    if (want_dash ? s[i] != '-' : !isDigit(s[i])) {
      return false;
    }
  }
  return true;
}

/**
 * "Kansas City Missouri" -> where " Missouri" starts, or nullptr. The longest
 * match wins, so "Charleston West Virginia" is not read as "... Virginia".
 */
const char* trailingStateName(const char* s, size_t n) {
  const char* best = nullptr;
  size_t best_len = 0;
  for (const UsState& st : kUsStates) {
    const size_t k = std::strlen(st.name);
    if (k > best_len && n > k + 1 && s[n - k - 1] == ' ' &&
        spellsIgnoreCase(s + n - k, k, st.name)) {
      best = s + n - k - 1;
      best_len = k;
    }
  }
  return best;
}

bool regionMatches(const Candidate& c, const char* region) {
  if (equalsIgnoreCase(region, c.admin1) || equalsIgnoreCase(region, c.country) ||
      equalsIgnoreCase(region, c.country_code)) {
    return true;
  }
  if (std::strlen(region) == 2) {
    for (const UsState& st : kUsStates) {
      if (equalsIgnoreCase(region, st.code) && equalsIgnoreCase(c.admin1, st.name) &&
          equalsIgnoreCase(c.country_code, "US")) {
        return true;
      }
    }
  }
  static const char* const kUsAliases[] = {"USA", "U.S.", "U.S.A.", "America"};
  for (const char* alias : kUsAliases) {
    if (equalsIgnoreCase(region, alias)) {
      return equalsIgnoreCase(c.country_code, "US");
    }
  }
  return false;
}

}  // namespace

bool parseQuery(const char* text, Query* out) {
  if (text == nullptr || out == nullptr) {
    return false;
  }
  const size_t n = strnlen(text, kMaxQuery + 1);
  if (n == 0 || n > kMaxQuery) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x20 || c == 0x7F) {
      return false;
    }
  }

  Query q{};
  const char* begin = text;
  const char* end = text + n;
  while (begin < end && *begin == ' ') {
    ++begin;
  }
  while (end > begin && end[-1] == ' ') {
    --end;
  }
  const size_t len = static_cast<size_t>(end - begin);

  if (isUsZip(begin, len)) {
    std::memcpy(q.name, begin, 5);
    q.name[5] = '\0';
    std::memcpy(q.whole, begin, 5);
    q.whole[5] = '\0';
    q.us_zip = true;
    *out = q;
    return true;
  }

  const char* name_end = end;
  const char* comma = static_cast<const char*>(std::memchr(begin, ',', len));
  if (comma != nullptr) {
    // "Evanston, IL, USA": the part after the first comma chooses.
    name_end = comma;
    const char* region_end = static_cast<const char*>(
        std::memchr(comma + 1, ',', static_cast<size_t>(end - comma - 1)));
    if (!copyTrimmed(comma + 1, region_end != nullptr ? region_end : end, q.region,
                     sizeof(q.region))) {
      q.region[0] = '\0';
    }
  } else if (len > 3 && begin[len - 3] == ' ' && isStateCode(end - 2)) {
    // "Naperville IL"
    name_end = end - 3;
    std::memcpy(q.region, end - 2, 2);
    q.region[2] = '\0';
    q.region_guessed = true;
  } else if (const char* at = trailingStateName(begin, len)) {
    // "Springfield Illinois" — or "Port Washington", hence region_guessed.
    name_end = at;
    copyTrimmed(at, end, q.region, sizeof(q.region));
    q.region_guessed = true;
  }

  if (!copyTrimmed(begin, name_end, q.name, sizeof(q.name)) || std::strlen(q.name) < 2 ||
      !copyTrimmed(begin, end, q.whole, sizeof(q.whole))) {
    return false;
  }
  *out = q;
  return true;
}

Query asWhole(const Query& q) {
  Query whole = q;
  snprintf(whole.name, sizeof(whole.name), "%s", q.whole);
  whole.region[0] = '\0';
  whole.region_guessed = false;
  return whole;
}

bool urlEncode(const char* in, char* out, size_t len) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  if (in == nullptr || out == nullptr || len == 0) {
    return false;
  }
  size_t o = 0;
  for (const char* p = in; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || isDigit(*p) ||
                       c == '-' || c == '.' || c == '_' || c == '~';
    const size_t need = plain ? 1 : 3;
    if (o + need >= len) {
      out[0] = '\0';
      return false;
    }
    if (plain) {
      out[o++] = *p;
    } else {
      out[o++] = '%';
      out[o++] = kHex[c >> 4];
      out[o++] = kHex[c & 0x0F];
    }
  }
  out[o] = '\0';
  return true;
}

bool searchPath(const Query& q, unsigned count, char* out, size_t len) {
  char name[kMaxName * 3 + 1];
  if (!urlEncode(q.name, name, sizeof(name))) {
    return false;
  }
  const int written = snprintf(out, len,
                               "/v1/search?name=%s&count=%u&language=en&format=json%s", name,
                               count, q.us_zip ? "&countryCode=US" : "");
  return written > 0 && static_cast<size_t>(written) < len;
}

void asciiFold(const char* in, char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  size_t o = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(in != nullptr ? in : "");
  while (*p != '\0' && o + 1 < len) {
    const unsigned char c = *p;
    if (c < 0x80) {
      if (c >= 0x20 && c != 0x7F) {
        out[o++] = static_cast<char>(c);
      }
      ++p;
      continue;
    }
    // A Latin-1 letter is 0xC3 0x80..0xBF (U+00C0..U+00FF).
    if (c == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) {
      const char folded = kLatin1Fold[p[1] - 0x80];
      if (folded != '_') {
        out[o++] = folded;
      }
      p += 2;
      continue;
    }
    // Anything else: skip the lead byte and its continuation bytes.
    ++p;
    while (*p >= 0x80 && *p <= 0xBF) {
      ++p;
    }
  }
  out[o] = '\0';
}

const char* usStateCode(const char* state_name) {
  if (state_name == nullptr) {
    return nullptr;
  }
  for (const UsState& st : kUsStates) {
    if (equalsIgnoreCase(state_name, st.name)) {
      return st.code;
    }
  }
  return nullptr;
}

int pickBest(const Candidate* c, size_t n, const Query& q) {
  char want[kMaxName + 1];
  asciiFold(q.name, want, sizeof(want));
  int best = -1;
  int best_rank = -1;
  long best_population = -1;
  for (size_t i = 0; i < n; ++i) {
    if (q.region[0] != '\0' && !regionMatches(c[i], q.region)) {
      continue;
    }
    char name[kMaxName + 1];
    asciiFold(c[i].name, name, sizeof(name));
    const bool exact = !q.us_zip && equalsIgnoreCase(want, name);
    const bool town = std::strncmp(c[i].feature_code, "PPL", 3) == 0;
    const int rank = (exact ? 2 : 0) + (town ? 1 : 0);
    const long population = c[i].population > 0 ? c[i].population : 0;
    if (rank > best_rank || (rank == best_rank && population > best_population)) {
      best = static_cast<int>(i);
      best_rank = rank;
      best_population = population;
    }
  }
  return best;
}

void label(const Candidate& c, char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  char name[kMaxName + 1];
  asciiFold(c.name, name, sizeof(name));
  const char* state =
      equalsIgnoreCase(c.country_code, "US") ? usStateCode(c.admin1) : nullptr;
  if (state != nullptr) {
    snprintf(out, len, "%s, %s", name, state);
  } else if (c.country_code[0] != '\0') {
    snprintf(out, len, "%s, %s", name, c.country_code);
  } else {
    snprintf(out, len, "%s", name);
  }
}

}  // namespace util::geocode
