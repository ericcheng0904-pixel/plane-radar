#include "ui/radar_range.h"

#include "ui/radar_theme.h"
#include "util/portal_input.h"

#include <Preferences.h>
#include <cmath>
#include <cstdio>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsTrackKey[] = "showTrack";
constexpr char kPrefsNightKey[] = "nightDim";
constexpr char kPrefsFontStepKey[] = "fontStep";
constexpr char kPrefsThemeKey[] = "theme";
constexpr char kPrefsTrailsKey[] = "showTrails";
constexpr char kPrefsIconsKey[] = "showIcons";
constexpr char kPrefsAlertSecKey[] = "alertSec";
constexpr char kPrefsTxPowerKey[] = "txPower";
/** Obsolete: the CPU throttle was removed after it measured no cooler. */
constexpr char kPrefsObsoletePowerSaveKey[] = "powerSave";
constexpr uint8_t kDefaultAlertSeconds = 5;
/** The value the firmware has always used. */
constexpr uint8_t kDefaultTxPowerDbm = 8;
constexpr uint8_t kDefaultRangeIndex = 1;  // 10 km ring
constexpr float kKmPerMile = 1.609344f;

Preferences s_prefs;
uint8_t s_range_index = kDefaultRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;
bool s_show_track_vectors = true;
bool s_dim_at_night = true;
bool s_show_trails = true;
bool s_show_class_icons = true;
uint8_t s_alert_seconds = kDefaultAlertSeconds;
uint8_t s_font_step = 0;
uint8_t s_theme = 0;
uint8_t s_tx_power_dbm = kDefaultTxPowerDbm;

void saveRangeIndex() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

void saveShowTrackVectors() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsTrackKey, s_show_track_vectors);
  s_prefs.end();
}

void saveTheme() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsThemeKey, s_theme);
  s_prefs.end();
}

void saveFontStep() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsFontStepKey, s_font_step);
  s_prefs.end();
}

void saveShowTrails() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsTrailsKey, s_show_trails);
  s_prefs.end();
}

void saveShowClassIcons() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsIconsKey, s_show_class_icons);
  s_prefs.end();
}

void saveAlertSeconds() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsAlertSecKey, s_alert_seconds);
  s_prefs.end();
}

void saveTxPower() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsTxPowerKey, s_tx_power_dbm);
  s_prefs.end();
}

/** A stored value from an older or corrupted record must not reach the UI. */
uint8_t sanitizedChoice(uint8_t stored, const uint8_t* choices, size_t count,
                        uint8_t fallback) {
  for (size_t i = 0; i < count; ++i) {
    if (stored == choices[i]) {
      return stored;
    }
  }
  return fallback;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_show_track_vectors = s_prefs.getBool(kPrefsTrackKey, true);
  s_dim_at_night = s_prefs.getBool(kPrefsNightKey, true);
  s_show_trails = s_prefs.getBool(kPrefsTrailsKey, true);
  s_show_class_icons = s_prefs.getBool(kPrefsIconsKey, true);
  s_alert_seconds = sanitizedChoice(
      s_prefs.getUChar(kPrefsAlertSecKey, kDefaultAlertSeconds),
      kAlertSecondsChoices, kAlertSecondsChoiceCount, kDefaultAlertSeconds);
  s_tx_power_dbm = sanitizedChoice(
      s_prefs.getUChar(kPrefsTxPowerKey, kDefaultTxPowerDbm), kTxPowerChoices,
      kTxPowerChoiceCount, kDefaultTxPowerDbm);
  const uint8_t font_step = s_prefs.getUChar(kPrefsFontStepKey, 0);
  s_font_step = (font_step < kFontStepCount) ? font_step : 0;
  const uint8_t theme = s_prefs.getUChar(kPrefsThemeKey, 0);
  s_theme = (theme < kThemeCount) ? theme : 0;
  s_prefs.end();
}

void rangeNext() {
  s_range_index = static_cast<uint8_t>((s_range_index + 1) % kRangePresetCount);
  saveRangeIndex();
}

const RangePreset& rangeCurrent() { return kRangePresets[s_range_index]; }

uint8_t rangeIndex() { return s_range_index; }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

bool showTrackVectors() { return s_show_track_vectors; }

bool dimAtNight() { return s_dim_at_night; }

bool showTrails() { return s_show_trails; }

bool showClassIcons() { return s_show_class_icons; }

uint8_t alertSeconds() { return s_alert_seconds; }

uint8_t txPowerDbm() { return s_tx_power_dbm; }

uint8_t fontStep() { return s_font_step; }

uint8_t theme() { return s_theme; }

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = util::portal::checkboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = util::portal::checkboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void saveTrackVectorsFromPortal(const char* checkbox_value) {
  s_show_track_vectors = util::portal::checkboxChecked(checkbox_value);
  saveShowTrackVectors();
  Serial.printf("Track vectors: %s\n", s_show_track_vectors ? "on" : "off");
}

void saveDimAtNightFromPortal(const char* checkbox_value) {
  s_dim_at_night = util::portal::checkboxChecked(checkbox_value);
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.putBool(kPrefsNightKey, s_dim_at_night);
    s_prefs.end();
  }
  Serial.printf("Dim at night: %s\n", s_dim_at_night ? "on" : "off");
}

void saveTrailsFromPortal(const char* checkbox_value) {
  s_show_trails = util::portal::checkboxChecked(checkbox_value);
  saveShowTrails();
  Serial.printf("Trails: %s\n", s_show_trails ? "on" : "off");
}

void saveClassIconsFromPortal(const char* checkbox_value) {
  s_show_class_icons = util::portal::checkboxChecked(checkbox_value);
  saveShowClassIcons();
  Serial.printf("Class icons: %s\n", s_show_class_icons ? "on" : "off");
}

void saveAlertSecondsFromPortal(const char* value) {
  uint8_t seconds = 0;
  if (!util::portal::oneOf(value, kAlertSecondsChoices,
                           kAlertSecondsChoiceCount, &seconds)) {
    return;
  }
  s_alert_seconds = seconds;
  saveAlertSeconds();
  Serial.printf("Alert flash: %u s\n", static_cast<unsigned>(s_alert_seconds));
}

void saveTxPowerFromPortal(const char* value) {
  uint8_t dbm = 0;
  if (!util::portal::oneOf(value, kTxPowerChoices, kTxPowerChoiceCount, &dbm)) {
    return;
  }
  s_tx_power_dbm = dbm;
  saveTxPower();
  Serial.printf("WiFi TX power: %u dBm\n", static_cast<unsigned>(s_tx_power_dbm));
}

void saveFontStepFromPortal(const char* value) {
  // Portal shows 1..kFontStepCount; anything else keeps the stored step.
  uint8_t step = 0;
  if (!util::portal::stepIndex(value, kFontStepCount, &step)) {
    return;
  }
  s_font_step = step;
  saveFontStep();
  Serial.printf("Text size step: %u\n", static_cast<unsigned>(s_font_step) + 1);
}

void saveThemeFromPortal(const char* value) {
  static const uint8_t kThemes[] = {0, 1, 2, 3};
  static_assert(sizeof(kThemes) == kThemeCount, "theme choices");
  uint8_t theme = 0;
  if (!util::portal::oneOf(value, kThemes, kThemeCount, &theme)) {
    return;
  }
  s_theme = theme;
  saveTheme();
  Serial.printf("Theme: %u\n", static_cast<unsigned>(s_theme));
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_range_index = kDefaultRangeIndex;
  s_use_miles = false;
  s_show_runways = true;
  s_show_track_vectors = true;
  s_dim_at_night = true;
  s_show_trails = true;
  s_show_class_icons = true;
  s_alert_seconds = kDefaultAlertSeconds;
  s_font_step = 0;
  s_theme = 0;
  s_tx_power_dbm = kDefaultTxPowerDbm;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsRangeKey);
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.remove(kPrefsTrackKey);
    s_prefs.remove(kPrefsFontStepKey);
    s_prefs.remove(kPrefsThemeKey);
    s_prefs.remove(kPrefsTrailsKey);
    s_prefs.remove(kPrefsIconsKey);
    s_prefs.remove(kPrefsAlertSecKey);
    s_prefs.remove(kPrefsObsoletePowerSaveKey);
    s_prefs.remove(kPrefsTxPowerKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
