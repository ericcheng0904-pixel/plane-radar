#include "util/airport_guess.h"

#include <cmath>

namespace util::airport_guess {

namespace {

constexpr float kKmPerDeg = 111.0f;
constexpr float kKmPerNm = 1.852f;
constexpr float kRadToDeg = 57.2957795f;
constexpr float kDegToRad = 0.01745329252f;
/** With two candidates, the nearer one wins only if it is under half as far. */
constexpr float kDominance = 0.5f;

/** Smallest angle between two bearings, 0..180. */
float angleOff(float a, float b) {
  float d = std::fmod(std::fabs(a - b), 360.0f);
  return d > 180.0f ? 360.0f - d : d;
}

/**
 * How high an aircraft this far out can sensibly be while landing (a 3 degree
 * glide is ~320 ft per nautical mile) or just after take-off (climbs are
 * steeper). Above it, it is passing over, not using the airport.
 */
float ceilingFt(Kind kind, float dist_nm) {
  return kind == Kind::kArriving ? 1500.0f + 450.0f * dist_nm
                                 : 2000.0f + 800.0f * dist_nm;
}

}  // namespace

Guess guess(const Plane& p, const Airport* airports, size_t count) {
  Guess none;
  if (p.alt_ft == INT32_MIN || p.alt_ft > kMaxAltFt || p.gs_knots < kMinGroundSpeedKt) {
    return none;
  }
  Kind kind = Kind::kNone;
  if (p.vrate_fpm <= -kMinVerticalFpm) {
    kind = Kind::kArriving;
  } else if (p.vrate_fpm >= kMinVerticalFpm) {
    kind = Kind::kDeparting;
  } else {
    return none;  // level: could be going anywhere
  }

  const float coslat = std::cos(p.lat * kDegToRad);
  Guess found;
  float best_km = 1e9f;
  float second_km = 1e9f;
  for (size_t i = 0; i < count; ++i) {
    const float east_km = (airports[i].lon - p.lon) * kKmPerDeg * coslat;
    const float north_km = (airports[i].lat - p.lat) * kKmPerDeg;
    const float dist_km = std::sqrt(east_km * east_km + north_km * north_km);
    if (dist_km > kMaxDistKm) {
      continue;
    }
    if (static_cast<float>(p.alt_ft) > ceilingFt(kind, dist_km / kKmPerNm)) {
      continue;
    }
    const float to_airport = std::atan2(east_km, north_km) * kRadToDeg;
    // Arriving: heading at it. Departing: heading straight away from it.
    const float wanted = kind == Kind::kArriving ? to_airport : to_airport + 180.0f;
    if (angleOff(p.track_deg, wanted) > kMaxTrackOffDeg) {
      continue;
    }
    if (dist_km < best_km) {
      second_km = best_km;
      best_km = dist_km;
      found.kind = kind;
      found.airport = i;
    } else if (dist_km < second_km) {
      second_km = dist_km;
    }
  }
  // Several airports fit: only trust the nearest if it is clearly nearer.
  return best_km < kMaxDistKm + 1.0f && best_km < kDominance * second_km ? found : none;
}

}  // namespace util::airport_guess
