#pragma once

#include <cstddef>
#include <cstdint>

/**
 * "Town or ZIP code" on the settings page, looked up by the radar itself
 * through Open-Meteo's free geocoding API (location data from GeoNames,
 * CC BY 4.0; attribution is on the settings page).
 *
 * The radar does the lookup itself, so the settings page stays a plain form
 * that works in any phone browser, scripts or not. The request runs on the
 * fetch task, so it is the only connection open while it runs — never
 * alongside an aircraft fetch — and the main loop applies the answer, so
 * services::location is only ever written from one thread.
 */
namespace services::geocode {

enum class State : uint8_t {
  kIdle,      // nothing asked since boot
  kWaiting,   // asked, not answered yet
  kFound,     // found(): applied to services::location by applyResult()
  kNotFound,  // the service knows no such place
  kFailed,    // no answer (offline, timeout, server error)
};

/** Why request() refused. */
enum class Request : uint8_t { kQueued, kBadText, kBusy };

/** Call once in setup(), before the fetch task starts. */
void init();

/**
 * Fetch task, between passes: sleep up to `ms`, less if request() or poke()
 * has something for it. A semaphore of its own rather than the task's
 * notification slot, which library code running on the task may use.
 */
void idle(unsigned long ms);

/** Wake the fetch task now (a newly saved location wants its first sweep). */
void poke();

/** Main loop: ask for `text` to be looked up. */
Request request(const char* text);

/**
 * Fetch task: run the lookup that is waiting, if any. True when it made a
 * request, so the caller leaves the aircraft fetch for its next pass.
 */
bool runPending();

/** Main loop: move a found place into services::location, once. */
void applyResult();

/** For the settings page. kWaiting turns into kFailed if it stalls. */
State state();

/** The text as asked, trimmed; escape it before putting it in a page. */
void asked(char* out, size_t len);

/** The place found ("Evanston, IL"); meaningful in kFound. */
void found(char* out, size_t len);

}  // namespace services::geocode
