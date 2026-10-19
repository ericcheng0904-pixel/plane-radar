#include "util/solar.h"

#include <cmath>

namespace util::solar {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kRad = kPi / 180.0;
/** 2000-01-01 12:00 UTC (J2000.0) as a Unix time. */
constexpr double kJ2000 = 946728000.0;
}  // namespace

// The US Naval Observatory's low-precision solar position: good to about a
// degree for decades either side of 2000, plenty for "is it night".
double elevationDeg(double lat_deg, double lon_deg, uint32_t unix_s) {
  const double d = (static_cast<double>(unix_s) - kJ2000) / 86400.0;
  const double g = (357.529 + 0.98560028 * d) * kRad;   // mean anomaly
  const double q = 280.459 + 0.98564736 * d;            // mean longitude, degrees
  const double l = (q + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * kRad;
  const double e = (23.439 - 0.00000036 * d) * kRad;    // obliquity of the ecliptic
  const double ra = std::atan2(std::cos(e) * std::sin(l), std::cos(l));
  const double dec = std::asin(std::sin(e) * std::sin(l));
  const double gmst_h = std::fmod(18.697374558 + 24.06570982441908 * d, 24.0);
  const double hour_angle = (gmst_h * 15.0 + lon_deg) * kRad - ra;
  const double lat = lat_deg * kRad;
  return std::asin(std::sin(lat) * std::sin(dec) +
                   std::cos(lat) * std::cos(dec) * std::cos(hour_angle)) / kRad;
}

bool isNight(double lat_deg, double lon_deg, uint32_t unix_s) {
  return elevationDeg(lat_deg, lon_deg, unix_s) < -6.0;
}

}  // namespace util::solar
