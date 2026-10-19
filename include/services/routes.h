#pragma once

#include <cstdint>

#include "services/adsb_client.h"
#include "util/route_cache.h"

/**
 * Origin and destination of the flights on screen, from adsb.im's route API
 * (http://adsb.im/api/0/routeset — plain HTTP, several flights per request;
 * it checks each route against the aircraft's position and says whether it
 * is plausible).
 *
 * The fetch task calls refresh() after each sweep; it asks about at most a
 * small batch of new airline flights, nearest first, no more often than every
 * kMinIntervalMs, and remembers every answer (util::RouteCache).
 */
namespace services::routes {

/** Create the lock. Call once before the fetch task starts. */
void init();

/** Fetch task only: look up routes for new flights in the current sweep. */
void refresh();

/** A known, plausible route for `callsign`; safe from the drawing loop. */
bool lookup(const char* callsign, util::Route* out);

/** What the radar shows as an aircraft's route. */
struct Shown {
  enum class Kind : uint8_t { kNone, kRoute, kArriving, kDeparting };
  Kind kind = Kind::kNone;
  /** kRoute: the codes as given ("MCO-ORD"); otherwise the one airport ("ORD"). */
  char codes[16] = "";
};

/**
 * The route from the database when it is known and plausible; failing that,
 * for airline flights (not helicopters or light aircraft), one end inferred
 * from a nearby major airport the aircraft is clearly landing at or leaving
 * (util::airport_guess); else nothing. `lat`/`lon` are where the
 * aircraft is drawn. Drawing loop only.
 */
Shown describe(const services::adsb::Aircraft& plane, float lat, float lon);

}  // namespace services::routes
