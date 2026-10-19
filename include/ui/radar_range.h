#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets (label on ring 3 = ¾ of outer radius).
 *
 * Recommended for ADS-B on a 1.28″ display:
 *   5 km  — pattern / very local (airfield vicinity)
 *  10 km  — default; neighborhood spotting
 *  15 km  — wider local area
 *  25 km  — metro / regional picture
 *
 * Outer radius (for aircraft math) is ring-3 distance ÷ 0.75.
 */
struct RangePreset {
  /** Distance shown on ring 3 (¾ of outer radius), always stored in km. */
  float ring3_km;
  float outer_km;
};

constexpr float kRing3ToOuterKm = 4.0f / 3.0f;

constexpr RangePreset kRangePresets[] = {
    {5.0f, 5.0f * kRing3ToOuterKm},
    {10.0f, 10.0f * kRing3ToOuterKm},
    {15.0f, 15.0f * kRing3ToOuterKm},
    {25.0f, 25.0f * kRing3ToOuterKm},
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);

/** Load saved range and distance units from flash. Call once after boot. */
void rangeInit();
/** Cycle preset and save to flash. */
void rangeNext();
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();

/**
 * UI text size steps, largest first. Each step maps to a set of embedded font
 * sizes in radar_theme.h; the portal shows them 1-based.
 */
constexpr uint8_t kFontStepCount = 3;

/** Display themes, as stored (see ui/theme.h). */
constexpr uint8_t kThemeCount = 4;

/**
 * Wi-Fi transmit power steps the portal accepts, in dBm.
 *
 * The firmware has always run at 8.5 dBm — a deliberate cap that keeps the
 * Super Mini's regulator out of trouble, at the cost of link margin. Raising it
 * costs current (and therefore heat) but is the first thing to try when the
 * connection drops in a weak spot.
 */
constexpr uint8_t kTxPowerChoices[] = {8, 13, 19};
constexpr size_t kTxPowerChoiceCount =
    sizeof(kTxPowerChoices) / sizeof(kTxPowerChoices[0]);

/** Flash durations the portal accepts, in seconds; 0 turns alerts off. */
constexpr uint8_t kAlertSecondsChoices[] = {0, 3, 5, 7};
constexpr size_t kAlertSecondsChoiceCount =
    sizeof(kAlertSecondsChoices) / sizeof(kAlertSecondsChoices[0]);

bool useMiles();
bool showRunways();
/** Track/speed vector lines drawn ahead of each aircraft symbol. */
bool showTrackVectors();

/** Dim the screen between dusk and dawn at the radar's location. */
bool dimAtNight();
void saveDimAtNightFromPortal(const char* checkbox_value);
/** Thin grey tail behind each aircraft. */
bool showTrails();
/** Per-class silhouettes (helicopter, heavy) instead of one triangle. */
bool showClassIcons();
/** Flash duration in seconds; 0 = alerts off. */
uint8_t alertSeconds();
uint8_t fontStep();
/** ui::theme::Id as stored; 0 (Scope) by default. */
uint8_t theme();
/** Wi-Fi transmit power in dBm, one of kTxPowerChoices. */
uint8_t txPowerDbm();
/** WiFi portal checkbox: "T" = miles, otherwise km. */
void saveMilesFromPortal(const char* checkbox_value);
void saveRunwaysFromPortal(const char* checkbox_value);
void saveTrackVectorsFromPortal(const char* checkbox_value);
void saveTrailsFromPortal(const char* checkbox_value);
void saveClassIconsFromPortal(const char* checkbox_value);
/** WiFi portal number field: one of kAlertSecondsChoices. */
void saveAlertSecondsFromPortal(const char* value);
/** WiFi portal number field: 1..kFontStepCount, stored 0-based. */
void saveFontStepFromPortal(const char* value);
/** WiFi portal select: 0..kThemeCount-1; anything else keeps the stored theme. */
void saveThemeFromPortal(const char* value);
/** WiFi portal number field: one of kTxPowerChoices. */
void saveTxPowerFromPortal(const char* value);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Reset the range and every display option to its default (reset for a new owner). */
void unitsReset();

}  // namespace ui::radar
