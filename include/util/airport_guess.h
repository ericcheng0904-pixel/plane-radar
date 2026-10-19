#pragma once

#include <cstddef>
#include <cstdint>

/**
 * One end of a route, inferred when the route database has nothing it trusts:
 * an aircraft low, close to a major airport, descending toward it is arriving
 * there; one climbing away from it has just left it.
 *
 * Every condition must hold. When several airports fit, the nearest is taken
 * only if it is under half as far as the next; otherwise no guess — a blank
 * is better than a wrong airport. Free of Arduino
 * headers so the host test suite can cover it.
 */
namespace util::airport_guess {

struct Airport {
  float lat;
  float lon;
};

struct Plane {
  float lat;
  float lon;
  int32_t alt_ft;      // INT32_MIN when unknown
  int16_t vrate_fpm;   // + climbing
  float track_deg;     // over the ground, 0 = north
  float gs_knots;
};

enum class Kind : uint8_t { kNone = 0, kArriving, kDeparting };

struct Guess {
  Kind kind = Kind::kNone;
  size_t airport = 0;  // index into the list passed in
};

constexpr int32_t kMaxAltFt = 10000;
constexpr float kMaxDistKm = 35.0f;
constexpr int16_t kMinVerticalFpm = 300;
constexpr float kMaxTrackOffDeg = 30.0f;
constexpr float kMinGroundSpeedKt = 60.0f;

Guess guess(const Plane& plane, const Airport* airports, size_t count);

}  // namespace util::airport_guess
