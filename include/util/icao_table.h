#pragma once

#include <cstddef>
#include <cstring>

/**
 * Lookup in tables keyed by a three-letter ICAO airline code. Free of Arduino
 * headers so the host test suite can cover it.
 */
namespace util::icao {

/**
 * The entry of `table` whose `icao` member (char[4], NUL-terminated) equals
 * `code`, or nullptr. `table` must be sorted by `icao`; the generators write
 * it that way. Binary search, because the brand table is consulted for every
 * tag on every frame.
 */
template <typename Entry>
const Entry* find(const Entry* table, size_t count, const char* code) {
  if (table == nullptr || code == nullptr) {
    return nullptr;
  }
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int cmp = strncmp(code, table[mid].icao, sizeof(table[mid].icao));
    if (cmp == 0) {
      return &table[mid];
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return nullptr;
}

}  // namespace util::icao
