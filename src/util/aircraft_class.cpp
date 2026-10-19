#include "util/aircraft_class.h"

#include <cstddef>
#include <cstring>

namespace util::aircraft {

namespace {

/**
 * Rotorcraft type designators. ADS-B category A7 is the authoritative signal,
 * but most helicopters transmit no category at all, and a helicopter drawn as
 * an airliner is the one mistake that looks obviously wrong on screen.
 *
 * Matched exactly, never as a prefix: "B29" would otherwise claim the B-29 for
 * Bell, and "B47" the B-47 Stratojet.
 */
const char* const kRotorTypes[] = {
    "A109", "A119", "A139", "A149", "A169", "A189", "AS32", "AS3B", "AS350",
    "AS35", "AS50", "AS55", "AS65", "AW09", "AW89", "B06", "B06T", "B105",
    "B212", "B222", "B230", "B407", "B412", "B427", "B429", "B430", "B47G",
    "B47J", "B505", "BK17", "EC20", "EC25", "EC30", "EC35", "EC45", "EC55",
    "EC75", "EH10", "EN28", "EXPL", "G2CA", "H47", "H500", "H60", "H64",
    "HUCO", "KA26", "KA32", "LYNX", "MD50", "MD52", "MD60", "MI2", "MI8",
    "MI17", "MI24", "MI26", "NH90", "PUMA", "R22", "R44", "R66", "S330",
    "S61", "S64", "S70", "S76", "S92", "TIGR", "UH1", "UH60",
};

/**
 * Type designator -> what it is. Covers the types data/aircraft_types names
 * (scripts/build_aircraft_types.py) and a few common military and outsize
 * ones; anything else falls back to the emitter category in shapeOf().
 */
struct TypeShape {
  const char* code;
  Klass klass;
  uint8_t engines;
  bool rear;
};
constexpr Klass W = Klass::kWide;
constexpr Klass N = Klass::kNarrow;
constexpr Klass R = Klass::kRegional;
constexpr Klass T = Klass::kTurboprop;
constexpr Klass B = Klass::kBizJet;
const TypeShape kTypeShapes[] = {
    // Twin aisle.
    {"A306", W, 2, false}, {"A30B", W, 2, false}, {"A310", W, 2, false},
    {"A332", W, 2, false}, {"A333", W, 2, false}, {"A337", W, 2, false},
    {"A338", W, 2, false}, {"A339", W, 2, false}, {"A359", W, 2, false},
    {"A35K", W, 2, false}, {"A3ST", W, 2, false}, {"B762", W, 2, false},
    {"B763", W, 2, false}, {"B764", W, 2, false}, {"KC46", W, 2, false},
    {"B772", W, 2, false}, {"B77L", W, 2, false}, {"B773", W, 2, false},
    {"B77W", W, 2, false}, {"B778", W, 2, false}, {"B779", W, 2, false},
    {"B788", W, 2, false}, {"B789", W, 2, false}, {"B78X", W, 2, false},
    {"DC10", W, 3, false}, {"MD11", W, 3, false}, {"L101", W, 3, false},
    {"A342", W, 4, false}, {"A343", W, 4, false}, {"A345", W, 4, false},
    {"A346", W, 4, false}, {"A388", W, 4, false}, {"B741", W, 4, false},
    {"B742", W, 4, false}, {"B743", W, 4, false}, {"B744", W, 4, false},
    {"B748", W, 4, false}, {"B74D", W, 4, false}, {"B74R", W, 4, false},
    {"B74S", W, 4, false}, {"A124", W, 4, false}, {"A225", W, 6, false},
    {"IL76", W, 4, false}, {"IL86", W, 4, false}, {"IL96", W, 4, false},
    {"C5M", W, 4, false}, {"C17", W, 4, false},
    // Single aisle.
    {"A318", N, 2, false}, {"A319", N, 2, false}, {"A320", N, 2, false},
    {"A321", N, 2, false}, {"A19N", N, 2, false}, {"A20N", N, 2, false},
    {"A21N", N, 2, false}, {"BCS1", N, 2, false}, {"BCS3", N, 2, false},
    {"B732", N, 2, false}, {"B733", N, 2, false}, {"B734", N, 2, false},
    {"B735", N, 2, false}, {"B736", N, 2, false}, {"B737", N, 2, false},
    {"B738", N, 2, false}, {"B739", N, 2, false}, {"B37M", N, 2, false},
    {"B38M", N, 2, false}, {"B39M", N, 2, false}, {"B3XM", N, 2, false},
    {"P8", N, 2, false}, {"B752", N, 2, false}, {"B753", N, 2, false},
    {"C919", N, 2, false}, {"B712", N, 2, true}, {"MD82", N, 2, true},
    {"MD83", N, 2, true}, {"MD87", N, 2, true}, {"MD88", N, 2, true},
    {"MD90", N, 2, true}, {"B722", N, 3, true}, {"B703", N, 4, false},
    {"K35R", N, 4, false}, {"E3TF", N, 4, false}, {"B52", N, 8, false},
    // Regional jets.
    {"CRJ1", R, 2, true}, {"CRJ2", R, 2, true}, {"CRJ7", R, 2, true},
    {"CRJ9", R, 2, true}, {"CRJX", R, 2, true}, {"E135", R, 2, true},
    {"E145", R, 2, true}, {"E35L", R, 2, true}, {"AJ27", R, 2, true},
    {"E170", R, 2, false}, {"E75L", R, 2, false}, {"E75S", R, 2, false},
    {"E190", R, 2, false}, {"E195", R, 2, false}, {"E290", R, 2, false},
    {"E295", R, 2, false}, {"SU95", R, 2, false}, {"B461", R, 4, false},
    {"B462", R, 4, false}, {"B463", R, 4, false}, {"RJ85", R, 4, false},
    {"RJ1H", R, 4, false},
    // Turboprops.
    {"DH8A", T, 2, false}, {"DH8B", T, 2, false}, {"DH8C", T, 2, false},
    {"DH8D", T, 2, false}, {"DHC6", T, 2, false}, {"AT43", T, 2, false},
    {"AT45", T, 2, false}, {"AT46", T, 2, false}, {"AT72", T, 2, false},
    {"AT75", T, 2, false}, {"AT76", T, 2, false}, {"SF34", T, 2, false},
    {"SB20", T, 2, false}, {"AN26", T, 2, false}, {"BE20", T, 2, false},
    {"BE9L", T, 2, false}, {"B350", T, 2, false}, {"C130", T, 4, false},
    {"C30J", T, 4, false}, {"AN12", T, 4, false}, {"AN22", T, 4, false},
    {"A400", T, 4, false}, {"PC12", T, 1, false}, {"C208", T, 1, false},
    {"TBM7", T, 1, false}, {"TBM8", T, 1, false}, {"TBM9", T, 1, false},
    {"P46T", T, 1, false},
    // Business jets.
    {"C510", B, 2, true}, {"C525", B, 2, true}, {"C25A", B, 2, true},
    {"C25B", B, 2, true}, {"C25C", B, 2, true}, {"C550", B, 2, true},
    {"C560", B, 2, true}, {"C56X", B, 2, true}, {"C680", B, 2, true},
    {"C68A", B, 2, true}, {"C700", B, 2, true}, {"C750", B, 2, true},
    {"CL30", B, 2, true}, {"CL35", B, 2, true}, {"CL60", B, 2, true},
    {"GL5T", B, 2, true}, {"GLEX", B, 2, true}, {"GL7T", B, 2, true},
    {"LJ35", B, 2, true}, {"LJ45", B, 2, true}, {"LJ60", B, 2, true},
    {"LJ75", B, 2, true}, {"E50P", B, 2, true}, {"E55P", B, 2, true},
    {"E545", B, 2, true}, {"E550", B, 2, true}, {"PC24", B, 2, true},
    {"HDJT", B, 2, true}, {"GLF4", B, 2, true}, {"GLF5", B, 2, true},
    {"GLF6", B, 2, true}, {"G280", B, 2, true}, {"GA5C", B, 2, true},
    {"GA6C", B, 2, true}, {"G150", B, 2, true}, {"GALX", B, 2, true},
    {"F2TH", B, 2, true}, {"FA6X", B, 2, true}, {"FA50", B, 3, true},
    {"F900", B, 3, true}, {"FA7X", B, 3, true}, {"FA8X", B, 3, true},
    {"H25B", B, 2, true}, {"HA4T", B, 2, true}, {"BE40", B, 2, true},
    {"SF50", B, 1, true},
};

const TypeShape* findType(const char* type_code) {
  if (type_code == nullptr || type_code[0] == '\0') {
    return nullptr;
  }
  for (const TypeShape& t : kTypeShapes) {
    if (std::strcmp(type_code, t.code) == 0) {
      return &t;
    }
  }
  return nullptr;
}

/**
 * The ones worth interrupting the display for: the double decker, outsize
 * freighters, and the oddities you would walk to the window for.
 */
const char* const kNotableTypes[] = {
    "A124", "A225", "A337", "A388", "A3ST", "A400", "AN22", "B52", "B703",
    "B741", "B742", "B743", "B744", "B748", "B74D", "B74R", "B74S", "C5M",
    "CONC", "IL76", "IL86", "IL96",
};

template <size_t N>
bool matchesExactly(const char* type_code, const char* const (&table)[N]) {
  if (type_code == nullptr || type_code[0] == '\0') {
    return false;
  }
  for (size_t i = 0; i < N; ++i) {
    if (std::strcmp(type_code, table[i]) == 0) {
      return true;
    }
  }
  return false;
}

/** Emitter category, e.g. "A7" for rotorcraft. Returns 0 when absent. */
char categoryDigit(const char* category) {
  if (category == nullptr || category[0] != 'A' || category[1] == '\0' ||
      category[2] != '\0') {
    return 0;
  }
  return category[1];
}

}  // namespace

Shape shapeOf(const char* type_code, const char* category) {
  const char digit = categoryDigit(category);
  // The transmitter's own "I am a rotorcraft" wins over any type table.
  if (digit == '7' || matchesExactly(type_code, kRotorTypes)) {
    return Shape{Klass::kRotor, 0, false};
  }
  if (const TypeShape* t = findType(type_code)) {
    return Shape{t->klass, t->engines, t->rear};
  }
  // Unknown type: the emitter category gives the size at best. Without one
  // there is no dependable split, so default to the jet symbol the radar has
  // always drawn.
  switch (digit) {
    case '1':
      return Shape{Klass::kLight, 0, false};
    case '5':
      return Shape{Klass::kHeavy, 0, false};
    default:
      return Shape{Klass::kJet, 0, false};
  }
}

Klass classify(const char* type_code, const char* category) {
  return shapeOf(type_code, category).klass;
}

const char* shortLabel(Klass k) {
  switch (k) {
    case Klass::kRegional:
      return "RJ";
    case Klass::kNarrow:
      return "NB";
    case Klass::kWide:
      return "WB";
    case Klass::kTurboprop:
      return "TP";
    case Klass::kBizJet:
      return "BJ";
    default:
      return nullptr;
  }
}

bool isEmergencySquawk(const char* squawk) {
  if (squawk == nullptr) {
    return false;
  }
  return std::strcmp(squawk, "7500") == 0 || std::strcmp(squawk, "7600") == 0 ||
         std::strcmp(squawk, "7700") == 0;
}

uint8_t alertFlags(const char* type_code, const char* emergency,
                   const char* squawk, uint32_t db_flags) {
  uint8_t flags = 0;

  if ((db_flags & kDbFlagMilitary) != 0) {
    flags |= kFlagMilitary;
  }

  // readsb sends "none" for the overwhelming majority; anything else is a
  // declared emergency. The squawk is the fallback for feeds that omit it.
  if (emergency != nullptr && emergency[0] != '\0' &&
      std::strcmp(emergency, "none") != 0) {
    flags |= kFlagEmergency;
  }
  if (isEmergencySquawk(squawk)) {
    flags |= kFlagEmergency;
  }

  if (matchesExactly(type_code, kNotableTypes)) {
    flags |= kFlagNotable;
  }

  return flags;
}

}  // namespace util::aircraft
