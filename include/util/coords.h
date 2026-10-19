#pragma once

/**
 * Latitude/longitude parsing for portal input and stored preferences.
 *
 * Free of Arduino headers so the host test suite can cover it
 * (test/test_util). Out-of-range or malformed coordinates must be rejected:
 * they feed the URL of the ADS-B request and the radar's own projection.
 */
namespace util::coords {

constexpr double kLatMin = -90.0;
constexpr double kLatMax = 90.0;
constexpr double kLonMin = -180.0;
constexpr double kLonMax = 180.0;

inline bool validLatLon(double lat, double lon) {
  // NaN fails every comparison, so this rejects it too.
  return lat >= kLatMin && lat <= kLatMax && lon >= kLonMin && lon <= kLonMax;
}

/**
 * Strict decimal parse: the whole string must be one number. Returns false —
 * leaving *out untouched — on empty input, trailing characters, or a value
 * outside the coordinate range.
 */
bool parseCoordinate(const char* text, double* out);

/**
 * One pasted latitude/longitude pair, in the forms a phone hands out:
 *
 *   "41.9786, -87.9048"  "41.9786 -87.9048"  "(41.9786,-87.9048)"
 *   "41.9786° N, 87.9048° W"                       (Apple Maps)
 *   ".../maps/place/.../@41.9786,-87.9048,15z/..."  (Google Maps, "@lat,lon,")
 *   "...?q=41.9786,-87.9048"  "...&query=..."  "...?ll=..."  ("%2C" as comma)
 *
 * The person setting the radar up copies these from a map app; asking them
 * to split the pair into two fields was where setup went wrong. Numbers are
 * plain decimals only — no exponent, hex, "inf" or "nan", at most 3 integer
 * and 15 fractional digits — and a hemisphere letter replaces the sign rather
 * than adding to it. Returns false, leaving the outputs untouched, for
 * anything else, including a third number in plain text, a pair out of range
 * and input longer than kMaxPairText.
 */
bool parseLatLonPair(const char* text, double* lat, double* lon);

/** Longest text parseLatLonPair() looks at; a Google Maps place link fits. */
constexpr unsigned kMaxPairText = 1024;

}  // namespace util::coords
