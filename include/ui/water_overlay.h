#pragma once

#include <LovyanGFX.hpp>

/**
 * Lakes, rivers and sea under the radar grid, from data/water.h.
 *
 * The polygons are rasterised once into a 1-bit mask whenever the range or the
 * radar centre changes; every frame then only tints the mask's runs and draws
 * the shore as anti-aliased lines along the polygon. When the configured centre is outside the area the
 * data was generated for, nothing is drawn.
 */
namespace ui::water {

/**
 * Build the mask for the current range and centre if they changed. Call once
 * per frame, before any of it is drawn: the frame is drawn in halves, and a
 * build that failed (out of memory) part way through a frame would leave water
 * in one half only. draw() and drawLandDots() use whatever was built last.
 */
void prepare();

/**
 * Tint the water and draw its shoreline onto `gfx`. With the frame sprite as
 * `frame` the water is blended into the backdrop; without (panel fallback) it
 * is filled flat.
 */
void draw(lgfx::LovyanGFX& gfx, LGFX_Sprite* frame);

/** Land as a halftone dot lattice (the Map theme). Nothing without water data. */
void drawLandDots(lgfx::LovyanGFX& gfx, uint16_t color);

}  // namespace ui::water
