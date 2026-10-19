#pragma once

#include <cstddef>
#include <cstdint>

/**
 * The airline logo pack: written by scripts/build_airline_brands.py --logos,
 * flashed once into the radar's own "logos" partition, read by
 * data::airline_logos. Logos are trademarks, so they live there and never in a
 * firmware image; this is only the format, free of Arduino headers so the host
 * test suite can feed it broken packs.
 *
 * Little-endian: "PRLG", u16 version, u16 count, u16 size, u16 transparent key,
 * u32 CRC-32 of everything after the header; count x (char icao[4], u32
 * offset) sorted by ICAO code; then size*size u16 pixels per logo.
 */
namespace util::logo_pack {

constexpr size_t kHeaderBytes = 16;
constexpr size_t kEntryBytes = 8;

struct Header {
  uint16_t count;
  uint16_t size;
  uint16_t key;
  uint32_t crc;
};

/**
 * Check a pack's header against what this firmware draws and the space the
 * partition has. False for anything else: no pack, an older format, a pack
 * for another logo size, or one that claims more than fits.
 */
bool parseHeader(const uint8_t* bytes, size_t partition_bytes, uint16_t want_size,
                 uint16_t want_key, Header* out);

/**
 * Where `icao`'s logo starts in the partition, from the index that follows the
 * header (count entries). False when it has none, or when the index points
 * outside the partition.
 */
bool find(const uint8_t* index, uint16_t count, const char* icao, size_t partition_bytes,
          size_t logo_bytes, uint32_t* offset);

/** CRC-32 (IEEE, as zlib's crc32), continued from `crc` (0 to start). */
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t n);

}  // namespace util::logo_pack
