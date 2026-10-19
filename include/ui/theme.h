#pragma once

#include <cstdint>

#include "ui/panel_color.h"

/**
 * Display themes, chosen on the settings page. Every colour is as seen on the
 * glass (it goes through ui::glassColor).
 *
 *   Scope   navy vignette, altitude-coloured aircraft (the default)
 *   Map     deep blue with the land as a halftone dot pattern, 12 spokes, a
 *           double bezel and orange aircraft, after the Kongji S1 "blue" skin
 *   Classic green rings on black, cyan/yellow tags: the original look
 *   Sweep   a rotating beam; aircraft light up as it passes and fade after
 */
namespace ui::theme {

enum class Id : uint8_t { kScope = 0, kMap = 1, kClassic = 2, kSweep = 3 };

struct Style {
  Rgb backdrop_edge;
  Rgb backdrop_centre;
  Rgb ring_inner;
  Rgb ring_outer;
  /** Radial lines: 4 = plain crosshair, 12 = one every 30 degrees. */
  uint8_t spokes;
  Rgb spoke;
  Rgb tick_minor;
  Rgb tick_major;
  /** Second ring just outside the outer ring, with accent marks every 30 deg. */
  bool double_bezel;
  Rgb accent;
  Rgb scale_label;
  Rgb water;
  /** How far water is blended toward `water` (0..256); the backdrop shows through. */
  uint16_t water_alpha;
  Rgb shore;
  /** Land drawn as a dot lattice (needs water data to know where land is). */
  bool land_dots;
  Rgb land_dot;
  /** Aircraft coloured by altitude, or all in `aircraft`. */
  bool altitude_ramp;
  Rgb aircraft;
  Rgb glyph_outline;
  Rgb tag_title;
  Rgb tag_type;
  /** Origin-destination line on tags and the card. */
  Rgb tag_route;
  bool tag_alt_ramp;
  Rgb tag_alt;
  Rgb trail;
  /** Bright dot at the oldest trail point, as on the Kongji map skin. */
  bool trail_end_dot;
  Rgb lock;
  /** Double-ring reticle around the featured aircraft instead of corners. */
  bool lock_rings;
  Rgb card_border;
  /** Rotating beam; aircraft brightness follows the time since it passed. */
  bool sweep;
  Rgb beam;
};

Id currentId();
const Style& current();

}  // namespace ui::theme
