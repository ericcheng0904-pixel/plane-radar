#pragma once

#include <cstdint>

/**
 * The badge that stands in for an airline logo on the info card: the airline's
 * code on its brand colour. Logos are the airlines' trademarks and are not in
 * published builds, so this is what most cards show. Free of Arduino headers so
 * the host test suite can cover it.
 */
namespace util::airline_badge {

/** Text colours a badge chooses between, 0xRRGGBB as seen on the glass. */
constexpr uint32_t kDarkInk = 0x0E121E;
constexpr uint32_t kLightInk = 0xFFFFFF;

/**
 * The code on the badge: the IATA code people know from tickets and departure
 * boards ("UA"), else the ICAO code ("AMF") for airlines that have none.
 */
const char* code(const char* iata, const char* icao);

/**
 * WCAG 2 contrast ratio between two 0xRRGGBB colours: 1 (identical luminance)
 * to 21 (black on white).
 */
float contrastRatio(uint32_t a, uint32_t b);

/**
 * kDarkInk or kLightInk, whichever contrasts more with `fill`. Brand colours
 * run from navy to yellow, so no single text colour is legible on all of them;
 * the better of the two never drops below about 4.3:1.
 */
uint32_t inkOn(uint32_t fill);

}  // namespace util::airline_badge
