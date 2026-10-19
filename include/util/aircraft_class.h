#pragma once

#include <cstdint>

/**
 * Classify an aircraft for drawing and for alerts.
 *
 * The ADS-B feed is inconsistent: `category`, `dbFlags`, `emergency` and
 * `squawk` are all optional and many aircraft send none of them. Every rule
 * here therefore degrades to a safe default rather than guessing, and each
 * input is treated as untrusted text. Free of Arduino headers so the host test
 * suite can cover it.
 */
namespace util::aircraft {

/**
 * What kind of aircraft, for its symbol: a regional jet, an A320 and a 787
 * should not look alike on the glass. The first four values are the radar's
 * original classes and keep their numbers.
 */
enum class Klass : uint8_t {
  kLight = 0,      // piston and light singles: small straight-wing symbol
  kJet = 1,        // a jet of unknown kind: the symbol the radar has always drawn
  kHeavy = 2,      // big, kind unknown (emitter category A5)
  kRotor = 3,      // rotor disc, heading-independent
  kRegional = 4,   // CRJ, ERJ, E-Jets, ARJ21, Superjet
  kNarrow = 5,     // single aisle: A220/A320 family, 737, 757, MD-80
  kWide = 6,       // twin aisle: 767, 777, 787, A330, A350, 747, A380, MD-11
  kTurboprop = 7,  // Dash 8, ATR, King Air, C-130, PC-12
  kBizJet = 8,     // Citation, Learjet, Challenger, Gulfstream, Falcon
};

/** Everything the symbol shows about an aircraft's type. */
struct Shape {
  Klass klass;
  /** Number of engines; 0 when not known. */
  uint8_t engines;
  /** Engines on the rear fuselage (CRJ, ERJ, 717, MD-80, business jets). */
  bool rear_engines;
};

/** Reasons an aircraft is worth flashing the display for. */
constexpr uint8_t kFlagMilitary = 1u << 0;
constexpr uint8_t kFlagEmergency = 1u << 1;
constexpr uint8_t kFlagNotable = 1u << 2;

/** dbFlags bit 0 marks military traffic in the readsb-derived feeds. */
constexpr uint32_t kDbFlagMilitary = 1u << 0;

/**
 * @param type_code ICAO type designator ("A388", "EC35"), may be empty.
 * @param category  ADS-B emitter category ("A1".."A7"), may be empty.
 */
Shape shapeOf(const char* type_code, const char* category);

/** shapeOf(...).klass. */
Klass classify(const char* type_code, const char* category);

/**
 * Short tag for the info card: "RJ", "NB", "WB", "TP", "BJ"; nullptr for the
 * classes that need none (light, rotor, unknown).
 */
const char* shortLabel(Klass k);

/**
 * @param emergency readsb `emergency` string ("none", "general", ...), optional.
 * @param squawk    transponder code as text ("7700"), optional.
 * @param db_flags  readsb `dbFlags` bitfield; 0 when the feed omits it.
 */
uint8_t alertFlags(const char* type_code, const char* emergency,
                   const char* squawk, uint32_t db_flags);

/** True for 7500 (hijack), 7600 (radio failure) and 7700 (general emergency). */
bool isEmergencySquawk(const char* squawk);

}  // namespace util::aircraft
