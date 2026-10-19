#pragma once

#include <LovyanGFX.hpp>

#include <cstdint>

namespace ui {

/**
 * An anti-aliased line `r` pixels either side of A-B with round ends, drawn
 * only inside `gfx`'s clip rectangle. Use it instead of LovyanGFX's
 * drawWideLine, which sets its own clip rectangle to the line's bounding box
 * and resets it to the whole sprite afterwards. The radar frame is a
 * full-screen view onto a half-height buffer (radar_display.cpp), so that
 * wrote over the heap past the buffer, during the line and every draw after it.
 * `color` is RGB565, as for the LovyanGFX call it replaces.
 */
void wideLine(lgfx::LGFXBase& gfx, float ax, float ay, float bx, float by, float r,
              uint16_t color);

}  // namespace ui
