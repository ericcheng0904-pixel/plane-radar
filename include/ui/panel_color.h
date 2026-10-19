#pragma once

#include <LovyanGFX.hpp>

#include <cstdint>

#include "config.h"

namespace ui {

/**
 * RGB565 for a colour as it should appear on the glass.
 *
 * With config::kDisplayRgbOrder the GC9A01 shows the sprite's R and B swapped,
 * so the value written has them swapped back. Every colour that is meant to
 * look a particular way goes through here; see panelColor() in
 * radar_display.cpp for the history.
 */
inline uint16_t glassColor(uint8_t r, uint8_t g, uint8_t b) {
  return config::kDisplayRgbOrder ? lgfx::color565(b, g, r) : lgfx::color565(r, g, b);
}

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

inline uint16_t glassColor(const Rgb& c) { return glassColor(c.r, c.g, c.b); }

/** Linear blend a -> b, t in [0, 1]. */
inline Rgb mix(const Rgb& a, const Rgb& b, float t) {
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return Rgb{static_cast<uint8_t>(a.r + (b.r - a.r) * t),
             static_cast<uint8_t>(a.g + (b.g - a.g) * t),
             static_cast<uint8_t>(a.b + (b.b - a.b) * t)};
}

}  // namespace ui
