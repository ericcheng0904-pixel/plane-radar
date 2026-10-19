#include "data/airline_logos.h"

#include <Arduino.h>
#include <esp_partition.h>

#include <cstdlib>
#include <cstring>

#include "util/logo_pack.h"

namespace data::airline_logos {

namespace {

/** The "logos" partition (partitions/plane_radar.csv). */
constexpr uint8_t kPartitionSubtype = 0x40;
constexpr size_t kLogoBytes = static_cast<size_t>(kSize) * kSize * 2;

const esp_partition_t* s_part = nullptr;
util::logo_pack::Header s_header{};
/** The pack's index, read once (8 bytes an airline). */
uint8_t* s_index = nullptr;
bool s_opened = false;
/** One logo at a time: the card shows one aircraft. */
uint16_t s_pixels[kSize * kSize];
char s_held[4] = {};

/** Find and check the pack once. False when there is none or it is damaged. */
bool open() {
  if (s_opened) {
    return s_index != nullptr;
  }
  s_opened = true;
  s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    static_cast<esp_partition_subtype_t>(kPartitionSubtype),
                                    "logos");
  uint8_t header[util::logo_pack::kHeaderBytes];
  if (s_part == nullptr || esp_partition_read(s_part, 0, header, sizeof(header)) != ESP_OK ||
      !util::logo_pack::parseHeader(header, s_part->size, kSize, kTransparent, &s_header)) {
    Serial.println("logos: no logo pack - airline codes instead");
    return false;
  }
  const size_t index_bytes = static_cast<size_t>(s_header.count) * util::logo_pack::kEntryBytes;
  auto* index = static_cast<uint8_t*>(malloc(index_bytes));
  if (index == nullptr || esp_partition_read(s_part, sizeof(header), index, index_bytes) != ESP_OK) {
    free(index);
    return false;
  }
  // A pack cut short by an interrupted flash would show torn logos: check the
  // whole of it once (~0.3 MB, well under 0.1 s, before the first card).
  uint32_t crc = util::logo_pack::crc32(0, index, index_bytes);
  const size_t end = sizeof(header) + index_bytes + s_header.count * kLogoBytes;
  uint8_t chunk[512];
  for (size_t off = sizeof(header) + index_bytes; off < end; off += sizeof(chunk)) {
    const size_t n = end - off < sizeof(chunk) ? end - off : sizeof(chunk);
    if (esp_partition_read(s_part, off, chunk, n) != ESP_OK) {
      free(index);
      return false;
    }
    crc = util::logo_pack::crc32(crc, chunk, n);
  }
  if (crc != s_header.crc) {
    Serial.println("logos: logo pack damaged - not used");
    free(index);
    return false;
  }
  s_index = index;
  Serial.printf("logos: %u airline logos\n", static_cast<unsigned>(s_header.count));
  return true;
}

}  // namespace

const uint16_t* logo32(const airline_brands::AirlineBrand& brand) {
  if (!open()) {
    return nullptr;
  }
  if (std::strncmp(s_held, brand.icao, sizeof(s_held)) == 0) {
    return s_pixels;
  }
  uint32_t offset = 0;
  if (!util::logo_pack::find(s_index, s_header.count, brand.icao, s_part->size, kLogoBytes,
                             &offset)) {
    return nullptr;
  }
  if (esp_partition_read(s_part, offset, s_pixels, kLogoBytes) != ESP_OK) {
    s_held[0] = '\0';
    return nullptr;
  }
  std::strncpy(s_held, brand.icao, sizeof(s_held));
  return s_pixels;
}

}  // namespace data::airline_logos
