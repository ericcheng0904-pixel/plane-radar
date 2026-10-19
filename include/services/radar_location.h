#pragma once

#include <cstddef>

namespace services::location {

/** Load saved lat/lon from NVS, or use config defaults. Call once before WiFi setup. */
void init();

/**
 * Where the radar is centred. Until a location is saved this is the config
 * default, which nothing should show: the radar waits on setup step 2 instead.
 */
double lat();
double lon();

/** False until a location has been saved (setup step 2 is still to do). */
bool isSet();

/**
 * What the radar is centred on, in words, for the screen and the settings
 * page: the town the lookup found ("Evanston, IL") or the coordinates as
 * entered ("41.9786, -87.9048"). "" while unset.
 */
const char* label();

/** Longest label kept, without the terminator. */
constexpr size_t kMaxLabel = 47;

/**
 * Validate, persist and apply. `place` names it ("Evanston, IL"); nullptr
 * labels it with the coordinates. False, changing nothing, when out of range.
 * Main loop only.
 */
bool save(double lat, double lon, const char* place);

/** Parse portal strings, validate, persist to NVS, update runtime values. */
bool saveFromStrings(const char* lat_str, const char* lon_str);

/**
 * Latched: a save moved the radar since the last call, so the main loop can
 * say where it now looks and drop what it drew for the old centre.
 */
bool consumeChanged();

/** Forget the stored location (reset for a new owner): back to setup step 2. */
void clear();

}  // namespace services::location
