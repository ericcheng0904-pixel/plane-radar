#pragma once

#include <cstddef>

/**
 * "Town or ZIP code" for the settings page: from what somebody types to one
 * place, through Open-Meteo's geocoding API (services::geocode makes the
 * request on the fetch task).
 *
 * The API matches place names only — "Naperville IL" finds nothing, and
 * "Evanston" alone answers Evanston, Wyoming first — so the text is split into
 * a name to search and a region to choose by, and several answers are ranked
 * here rather than trusting the first. A five-digit number is a US ZIP code:
 * the API finds the town that lists it, which is as close as the radar's
 * kilometre-scale rings need. Free of Arduino headers so the host test suite
 * can cover it (test/test_setup).
 */
namespace util::geocode {

/** Longest input accepted, in bytes; a town name and a state fit easily. */
constexpr size_t kMaxQuery = 64;
/** A name can be the whole query (asWhole()). */
constexpr size_t kMaxName = kMaxQuery;
constexpr size_t kMaxRegion = 32;

/** What was typed, split the way the API needs it. */
struct Query {
  char name[kMaxName + 1];      // searched for: "Evanston", "60201"
  char region[kMaxRegion + 1];  // chosen by: "IL", "Illinois", "France"; may be ""
  char whole[kMaxQuery + 1];    // the text as typed, trimmed
  bool us_zip;                  // five digits (or ZIP+4): US places only
  /**
   * The region was split off at a space, not a comma, so it may be part of
   * the name after all ("Port Washington"): worth one more search for
   * `whole` when nothing in the region turns up (asWhole()).
   */
  bool region_guessed;
};

/**
 * Split `text`: "Evanston, IL", "Naperville IL", "Kansas City Missouri",
 * "60201", "60614-1234". False — `out` untouched — for empty text, more than
 * kMaxQuery bytes, control characters, or a name that is too short to search.
 */
bool parseQuery(const char* text, Query* out);

/** The same text searched as one name, with no region: the second try. */
Query asWhole(const Query& q);

/**
 * Path and query string for the API ("/v1/search?name=...&count=..."), with
 * the name percent-encoded. False if it does not fit `len`.
 */
bool searchPath(const Query& q, unsigned count, char* out, size_t len);

/** One answer as the API gives it. Strings may be empty, never null. */
struct Candidate {
  const char* name;
  const char* admin1;        // state or province: "Illinois"
  const char* country_code;  // "US"
  const char* country;       // "United States"
  const char* feature_code;  // GeoNames class: "PPL" towns, "AIRH" heliports...
  double lat;
  double lon;
  long population;           // 0 when the API does not know
};

/**
 * Index of the place meant, or -1. Answers outside the region (when one was
 * given) never count; among the rest an exact name beats a near one, a town
 * beats a heliport, and the larger town wins — "Evanston" is the one in
 * Illinois, not Wyoming.
 */
int pickBest(const Candidate* c, size_t n, const Query& q);

/**
 * Short ASCII name for the round screen and the settings page: "Evanston, IL"
 * in the US, "Zurich, CH" elsewhere. The panel's fonts are ASCII only.
 */
void label(const Candidate& c, char* out, size_t len);

/** "IL" for "Illinois" (any case), or nullptr when it is not a US state. */
const char* usStateCode(const char* state_name);

/** Percent-encode everything but A-Z a-z 0-9 - . _ ~. False if `len` is too small. */
bool urlEncode(const char* in, char* out, size_t len);

/**
 * UTF-8 to ASCII: Latin-1 letters lose their accents ("Zürich" -> "Zurich"),
 * anything else outside ASCII is dropped. Always terminates.
 */
void asciiFold(const char* in, char* out, size_t len);

}  // namespace util::geocode
