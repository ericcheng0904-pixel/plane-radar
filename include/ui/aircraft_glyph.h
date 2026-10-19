#pragma once

#include <LovyanGFX.hpp>

#include <cstdint>

#include "ui/panel_color.h"

/**
 * Top-down aircraft silhouettes, rotated to heading.
 *
 * Each class is a handful of triangles in a nose-up template, so any heading
 * rotates exactly and nothing is stored as a bitmap. A 1 px dark outline keeps
 * the shape readable where it crosses a ring or another aircraft.
 */
namespace ui::glyph {

/**
 * Flightradar24-style altitude ramp as seen on the glass: warm and low, cool
 * and high, so climbing and descending traffic separates at a glance.
 */
Rgb altitudeColor(int32_t alt_ft);

/**
 * Draw the silhouette for util::aircraft::Klass `klass` centred on (cx, cy).
 * `scale` 1.0 is map size; the info card draws the same glyph larger.
 * Pass outline == fill to skip the outline. `engines` (0: unknown, none
 * drawn) and `rear_engines` place the engine pods (util::aircraft::Shape).
 */
void draw(lgfx::LovyanGFX& gfx, int cx, int cy, float heading_deg,
          uint8_t klass, float scale, uint16_t fill, uint16_t outline,
          uint8_t engines = 0, bool rear_engines = false);

/** Half the glyph's longest extent at `scale`, for placing tags and brackets. */
int radiusPx(uint8_t klass, float scale);

}  // namespace ui::glyph
