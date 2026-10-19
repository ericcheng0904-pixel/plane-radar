#pragma once

#include <cstdint>

#include "data/airline_brands.h"

/**
 * Airline logos for the info card: the one place the card asks for a logo.
 *
 * Logos are the airlines' trademarks, so none are in the repository or in any
 * firmware image. They live in the radar's own "logos" flash partition, as a
 * pack written by scripts/build_airline_brands.py --logos and flashed once
 * over USB. Firmware updates rewrite an app slot only, so they never remove
 * the logos. Without a pack every lookup answers nullptr and the card draws
 * the airline's code on its brand colour instead.
 *
 * Images are kSize x kSize panel-native RGB565 (R/B pre-swapped) in LovyanGFX's
 * default big-endian layout, so pushImage(x, y, kSize, kSize, logo,
 * kTransparent) draws them as is. They are read straight from flash (mapped),
 * so the pointer stays valid. Main loop only.
 */
namespace data::airline_logos {

constexpr int kSize = 32;
/** Pixels of this value are not drawn: the card shows through. */
constexpr uint16_t kTransparent = 0x1FF8;

/** Logo for `brand`, or nullptr when this build has none for it. */
const uint16_t* logo32(const airline_brands::AirlineBrand& brand);

}  // namespace data::airline_logos
