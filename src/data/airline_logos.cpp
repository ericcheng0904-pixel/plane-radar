#include "data/airline_logos.h"

#include <Arduino.h>
#include <esp_partition.h>

#include <cstring>

#include "util/logo_pack.h"

namespace data::airline_logos {

namespace {

/** The "logos" partition (partitions/plane_radar.csv). */
constexpr uint8_t kPartitionSubtype = 0x40;
constexpr size_t kLogoBytes = static_cast<size_t>(kSize) * kSize * 2;

/**
 * The pack, mapped into the address space like the firmware's own constant
 * data: logos are read straight from flash, so they cost no RAM, which the
 * radar has little of while a TLS connection is open.
 */
const uint8_t* s_pack = nullptr;
size_t s_pack_bytes = 0;
util::logo_pack::Header s_header{};
bool s_opened = false;

/** Find, map and check the pack once. False when there is none or it is damaged. */
bool open() {
  if (s_opened) {
    return s_pack != nullptr;
  }
  s_opened = true;
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(kPartitionSubtype), "logos");
  uint8_t header[util::logo_pack::kHeaderBytes];
  if (part == nullptr || esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK ||
      !util::logo_pack::parseHeader(header, part->size, kSize, kTransparent, &s_header)) {
    Serial.println("logos: no logo pack - airline codes instead");
    return false;
  }
  const size_t bytes = sizeof(header) +
                       static_cast<size_t>(s_header.count) *
                           (util::logo_pack::kEntryBytes + kLogoBytes);
  const void* mapped = nullptr;
  spi_flash_mmap_handle_t handle = 0;
  if (esp_partition_mmap(part, 0, bytes, SPI_FLASH_MMAP_DATA, &mapped, &handle) != ESP_OK) {
    Serial.println("logos: could not map the logo pack");
    return false;
  }
  const auto* pack = static_cast<const uint8_t*>(mapped);
  // A pack cut short by an interrupted flash would show torn logos: check the
  // whole of it once, before the first card.
  if (util::logo_pack::crc32(0, pack + sizeof(header), bytes - sizeof(header)) != s_header.crc) {
    Serial.println("logos: logo pack damaged - not used");
    spi_flash_munmap(handle);
    return false;
  }
  s_pack = pack;
  s_pack_bytes = bytes;
  Serial.printf("logos: %u airline logos\n", static_cast<unsigned>(s_header.count));
  return true;
}

}  // namespace

const uint16_t* logo32(const airline_brands::AirlineBrand& brand) {
  if (!open()) {
    return nullptr;
  }
  uint32_t offset = 0;
  if (!util::logo_pack::find(s_pack + util::logo_pack::kHeaderBytes, s_header.count, brand.icao,
                             s_pack_bytes, kLogoBytes, &offset)) {
    return nullptr;
  }
  return reinterpret_cast<const uint16_t*>(s_pack + offset);  // offsets are even
}

}  // namespace data::airline_logos
