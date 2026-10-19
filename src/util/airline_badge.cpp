#include "util/airline_badge.h"

#include <cmath>

namespace util::airline_badge {

namespace {

/** sRGB channel (0..255) to linear light, as WCAG 2 defines it. */
float linear(uint32_t channel) {
  const float c = static_cast<float>(channel & 0xFF) / 255.0f;
  return c <= 0.03928f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float luminance(uint32_t rgb) {
  return 0.2126f * linear(rgb >> 16) + 0.7152f * linear(rgb >> 8) +
         0.0722f * linear(rgb);
}

}  // namespace

const char* code(const char* iata, const char* icao) {
  if (iata != nullptr && iata[0] != '\0') {
    return iata;
  }
  return icao != nullptr ? icao : "";
}

float contrastRatio(uint32_t a, uint32_t b) {
  const float la = luminance(a);
  const float lb = luminance(b);
  return la > lb ? (la + 0.05f) / (lb + 0.05f) : (lb + 0.05f) / (la + 0.05f);
}

uint32_t inkOn(uint32_t fill) {
  return contrastRatio(fill, kDarkInk) > contrastRatio(fill, kLightInk) ? kDarkInk
                                                                        : kLightInk;
}

}  // namespace util::airline_badge
