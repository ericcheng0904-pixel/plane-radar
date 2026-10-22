#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  /** Age of the position fix at fetch time (ms), from the feed's seen_pos.
   *  Added to the elapsed time when dead-reckoning so the drawn position
   *  reflects the estimated current location, not the already-stale fix. */
  uint32_t pos_age_ms;
  char callsign[9];
  char type[5];
  char alt[12];
  /** ICAO 24-bit address; the only id stable across sweeps. */
  char hex[7];
  /** util::aircraft::Klass — which silhouette to draw. */
  uint8_t klass;
  /** util::aircraft::Shape: engines (0 = unknown) and where they sit. */
  uint8_t engines;
  bool rear_engines;
  /** util::aircraft::kFlag* bits — why this aircraft might be worth a flash. */
  uint8_t alert_flags;
  /** Barometric (else geometric) altitude in feet; kAltUnknown when absent. */
  int32_t alt_ft;
  /** Vertical rate in ft/min, + climbing; 0 when level or unknown. */
  int16_t vrate_fpm;
};

constexpr int32_t kAltUnknown = INT32_MIN;

constexpr size_t kMaxAircraft = 64;

/** Create the internal lock. Call once before any fetch/snapshot. */
void init();

/**
 * Copy the current aircraft into `out` (up to `max_out`) and report the fetch
 * timestamp, all under a lock — safe to call from a different thread than the
 * one running fetchUpdate(). Returns the number copied.
 */
size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms);

size_t aircraftCount();
const Aircraft* aircraftList();

/**
 * millis() timestamp of the last successful fetch (0 before the first). The
 * stored lat/lon are the values as of this time; callers can dead-reckon
 * newer positions from track_deg/gs_knots and the elapsed time.
 */
unsigned long lastUpdateMs();

/**
 * millis() of the last answer with aircraft from a feed (0 before the first),
 * whether they were shown or dropped as for an old centre. Unlike
 * lastUpdateMs(), discardSweepsBefore() leaves it alone: it is what the
 * no-data restart asks, so saving a setting does not look like a feed that
 * stopped answering.
 */
unsigned long lastAnswerMs();

/**
 * millis() when the feed in use, held back by its daily ration, may next be
 * asked; 0 when none is held back. For the screen, to say when the next
 * update is due rather than that it is retrying.
 */
unsigned long pacedUntilMs();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** How a fetchUpdate() pass went. */
enum class FetchResult : uint8_t {
  kAircraft,  // a feed answered: its aircraft are published
  kFailed,    // none this time: a feed failed, refused or rests, or no room to ask
  kPaced,     // nothing sent: the feed in use is rationed and its next request is not due
};

/**
 * Fetch aircraft within fetch_radius_km of center_lat/lon from the current
 * feed (HTTPS; see kFeeds in adsb_client.cpp): adsb.fi, else adsb.lol. A feed
 * that fails twice in a row hands over to the next, and one that rate-limits
 * (429) also rests for as long as it asks; the first is tried again every
 * five minutes. adsb.lol is rationed: past the first requests of a day, a
 * pass that comes before its pace allows sends nothing (kPaced), or tries
 * adsb.fi again once 30 s have passed (util::feed_turn).
 * `started_ms` is millis() taken before center_lat/lon were read, so a sweep
 * for a centre that has since changed can be recognised and dropped.
 */
FetchResult fetchUpdate(double center_lat, double center_lon, float fetch_radius_km,
                        unsigned long started_ms);

/**
 * Empty the aircraft list and ignore any sweep whose fetch started before
 * `ms` — call after the radar centre or range changes, so an in-flight fetch
 * for the old centre cannot land afterwards.
 */
void discardSweepsBefore(unsigned long ms);

/** HTTP status of the last fetchUpdate() (<= 0: HTTPClient transport error). */
int lastHttpStatus();

/**
 * millis() when the last fetch pass started, whatever its outcome (one held
 * back by the pace too); 0 = never.
 */
unsigned long lastAttemptMs();

/** Name of the feed in use, i.e. asked next ("adsb.fi"), for the settings page. */
const char* feedName();

/**
 * The time (Unix seconds, UTC) as of the last feed answer plus the time since;
 * false until a feed has answered. The radar has no clock of its own.
 */
bool utcNow(uint32_t* unix_s);

/** Name of the feed the aircraft on screen came from. */
const char* dataFeedName();

/** Whether that feed's certificate was checked (see services::tls). */
bool dataVerified();

/** True once every feed has failed in turn: time to slow down. */
bool allFeedsFailing();

/** Every feed is resting at its own request (refused, 429, allowance used). */
bool allFeedsResting();

/**
 * One line about feeds that are resting or rationed, for the settings page
 * ("adsb.fi refused, trying again in 5 h 40 min; adsb.lol: 118 of 400
 * requests today"); empty when there is nothing to say. Not thread-exact:
 * for display only.
 */
void feedStatusText(char* out, size_t n);

}  // namespace services::adsb
