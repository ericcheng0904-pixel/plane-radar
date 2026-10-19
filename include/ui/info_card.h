#pragma once

#include <LovyanGFX.hpp>

#include <cstddef>

#include "services/adsb_client.h"
#include "services/routes.h"

/**
 * The info card: one aircraft at a time, spelled out — airline badge (its code
 * on its brand colour, or its logo in a private build) and name, flight number,
 * aircraft type, altitude, climb/descent and speed.
 *
 * It sits in the lower part of the disc and moves to the top while the aircraft
 * it describes is underneath it. With several aircraft in range it rotates
 * through the nearest few so each gets its turn.
 */
namespace ui::card {

/**
 * Pick the aircraft to feature this frame.
 *
 * @param planes  this frame's snapshot
 * @param nearest indices into `planes` of in-ring, airborne aircraft, nearest
 *                first
 * @return an index from `nearest`, or -1 for none (the card is hidden)
 */
int choose(const services::adsb::Aircraft* planes, const size_t* nearest,
           size_t count, unsigned long now_ms);

struct Rect {
  int x;
  int y;
  int w;
  int h;
  bool contains(int px, int py, int margin = 0) const {
    return px >= x - margin && px < x + w + margin && py >= y - margin &&
           py < y + h + margin;
  }
  bool overlaps(const Rect& o) const {
    return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h;
  }
};

/**
 * Where the card sits: in the lower part of the disc, or mirrored at the top.
 * `tall` is the size with a route line.
 */
Rect rect(bool at_top, bool tall);

/** Draw the card for `plane`; with `route` (a route, or one end of it) it adds a route line. */
void draw(LGFX_Sprite& frame, const services::adsb::Aircraft& plane, bool at_top,
          bool miles, const services::routes::Shown* route);

/** Forget the rotation, e.g. after the radar centre moved. */
void reset();

}  // namespace ui::card
