#pragma once

#include <cstdint>

/**
 * Where the sun is, roughly (about 0.5 degrees), for dimming the screen at
 * night: the radar knows its own location, and the flight feed's answers
 * carry the time, so no clock or extra service is needed. Free of Arduino
 * headers so the host test suite can cover it.
 */
namespace util::solar {

/** Sun's elevation above the horizon in degrees at `unix_s` (UTC) seen from lat/lon. */
double elevationDeg(double lat_deg, double lon_deg, uint32_t unix_s);

/** Dark enough to dim: the sun below civil twilight (-6 degrees). */
bool isNight(double lat_deg, double lon_deg, uint32_t unix_s);

}  // namespace util::solar
