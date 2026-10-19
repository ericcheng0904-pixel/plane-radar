#include "util/logo_pack.h"

#include <cstring>

namespace util::logo_pack {

namespace {

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint16_t kVersion = 1;

}  // namespace

bool parseHeader(const uint8_t* bytes, size_t partition_bytes, uint16_t want_size,
                 uint16_t want_key, Header* out) {
  if (bytes == nullptr || std::memcmp(bytes, "PRLG", 4) != 0 || u16(bytes + 4) != kVersion) {
    return false;
  }
  Header h{u16(bytes + 6), u16(bytes + 8), u16(bytes + 10), u32(bytes + 12)};
  if (h.count == 0 || h.size != want_size || h.key != want_key) {
    return false;
  }
  const size_t logo_bytes = static_cast<size_t>(h.size) * h.size * 2;
  const size_t total = kHeaderBytes + h.count * (kEntryBytes + logo_bytes);
  if (total > partition_bytes) {
    return false;
  }
  *out = h;
  return true;
}

bool find(const uint8_t* index, uint16_t count, const char* icao, size_t partition_bytes,
          size_t logo_bytes, uint32_t* offset) {
  if (index == nullptr || icao == nullptr) {
    return false;
  }
  char key[4] = {};
  std::strncpy(key, icao, sizeof(key));
  int lo = 0;
  int hi = static_cast<int>(count) - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const uint8_t* e = index + static_cast<size_t>(mid) * kEntryBytes;
    const int cmp = std::memcmp(key, e, sizeof(key));
    if (cmp == 0) {
      const uint32_t off = u32(e + 4);
      if (off < kHeaderBytes || off > partition_bytes || partition_bytes - off < logo_bytes) {
        return false;
      }
      *offset = off;
      return true;
    }
    if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

uint32_t crc32(uint32_t crc, const uint8_t* data, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

}  // namespace util::logo_pack
