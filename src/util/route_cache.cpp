#include "util/route_cache.h"

#include <cstring>

namespace util {

const RouteCache::Slot* RouteCache::slotFor(const char* callsign) const {
  if (callsign == nullptr || callsign[0] == '\0') {
    return nullptr;
  }
  for (const Slot& slot : slots_) {
    if (slot.used && strncmp(slot.route.callsign, callsign, sizeof(slot.route.callsign)) == 0) {
      return &slot;
    }
  }
  return nullptr;
}

bool RouteCache::fresh(const Slot& slot, unsigned long now_ms) const {
  const unsigned long ttl = slot.route.known ? kKnownTtlMs : kUnknownTtlMs;
  return now_ms - slot.stored_ms <= ttl;
}

void RouteCache::put(const Route& route, unsigned long now_ms) {
  Slot* target = const_cast<Slot*>(slotFor(route.callsign));
  if (target == nullptr) {
    // A free slot, else the one stored longest ago.
    for (Slot& slot : slots_) {
      if (!slot.used) {
        target = &slot;
        break;
      }
      if (target == nullptr || now_ms - slot.stored_ms > now_ms - target->stored_ms) {
        target = &slot;
      }
    }
  }
  target->route = route;
  target->route.callsign[sizeof(target->route.callsign) - 1] = '\0';
  target->stored_ms = now_ms;
  target->used = true;
}

bool RouteCache::find(const char* callsign, unsigned long now_ms, Route* out) const {
  const Slot* slot = slotFor(callsign);
  if (slot == nullptr || !fresh(*slot, now_ms)) {
    return false;
  }
  *out = slot->route;
  return true;
}

bool RouteCache::needsLookup(const char* callsign, unsigned long now_ms) const {
  const Slot* slot = slotFor(callsign);
  return slot == nullptr || !fresh(*slot, now_ms);
}

}  // namespace util
