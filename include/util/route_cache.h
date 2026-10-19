#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Remembered flight routes, keyed by callsign.
 *
 * Routes come from a web service that is asked in small batches; each answer
 * — a route, or "unknown" — is kept so the same flight is not asked about on
 * every sweep. Known routes are kept for hours (a callsign flies the same
 * route all day); "unknown" is retried sooner, in case the service learns it.
 * Fixed size, oldest entry recycled. Free of Arduino headers so the host test
 * suite can cover it.
 */
namespace util {

struct Route {
  char callsign[9];
  /** IATA codes as the service gives them: "MCO-ORD", "LAX-ORD-LGA". */
  char codes[16];
  bool known;
};

class RouteCache {
 public:
  static constexpr size_t kCapacity = 64;
  static constexpr unsigned long kKnownTtlMs = 6UL * 3600UL * 1000UL;
  static constexpr unsigned long kUnknownTtlMs = 30UL * 60UL * 1000UL;

  /** Store an answer for `route.callsign` at `now_ms` (replaces any older one). */
  void put(const Route& route, unsigned long now_ms);

  /** Copy a fresh answer for `callsign` into `out`. False if none or expired. */
  bool find(const char* callsign, unsigned long now_ms, Route* out) const;

  /** True when `callsign` has no fresh answer and should be asked about. */
  bool needsLookup(const char* callsign, unsigned long now_ms) const;

 private:
  struct Slot {
    Route route;
    unsigned long stored_ms;
    bool used;
  };
  const Slot* slotFor(const char* callsign) const;
  bool fresh(const Slot& slot, unsigned long now_ms) const;

  Slot slots_[kCapacity] = {};
};

}  // namespace util
