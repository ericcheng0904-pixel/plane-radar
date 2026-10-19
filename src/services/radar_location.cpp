#include "services/radar_location.h"

#include <Preferences.h>

#include <cstdio>

#include "config.h"
#include "util/coords.h"

namespace services::location {

namespace {

constexpr char kPrefsNamespace[] = "radar";
constexpr char kKeyLat[] = "lat";
constexpr char kKeyLon[] = "lon";
constexpr char kKeyLabel[] = "name";

double s_lat = config::kDefaultRadarLat;
double s_lon = config::kDefaultRadarLon;
bool s_is_set = false;
char s_label[kMaxLabel + 1] = "";
bool s_changed = false;

void labelFromCoordinates(double lat, double lon) {
  snprintf(s_label, sizeof(s_label), "%.4f, %.4f", lat, lon);
}

void persist(double lat, double lon) {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.putDouble(kKeyLat, lat);
  prefs.putDouble(kKeyLon, lon);
  prefs.putString(kKeyLabel, s_label);
  prefs.end();
  s_lat = lat;
  s_lon = lon;
  s_is_set = true;
  s_changed = true;
}

}  // namespace

void init() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, true);
  if (prefs.isKey(kKeyLat) && prefs.isKey(kKeyLon)) {
    const double lat = prefs.getDouble(kKeyLat, config::kDefaultRadarLat);
    const double lon = prefs.getDouble(kKeyLon, config::kDefaultRadarLon);
    if (util::coords::validLatLon(lat, lon)) {
      s_lat = lat;
      s_lon = lon;
      s_is_set = true;
      // Saved before places had names: the coordinates will do.
      if (!prefs.isKey(kKeyLabel) || prefs.getString(kKeyLabel, s_label, sizeof(s_label)) == 0) {
        labelFromCoordinates(lat, lon);
      }
    }
  }
  prefs.end();
}

double lat() { return s_lat; }

double lon() { return s_lon; }

bool isSet() { return s_is_set; }

const char* label() { return s_is_set ? s_label : ""; }

bool save(double lat, double lon, const char* place) {
  if (!util::coords::validLatLon(lat, lon)) {
    return false;
  }
  if (place != nullptr && place[0] != '\0') {
    snprintf(s_label, sizeof(s_label), "%s", place);
  } else {
    labelFromCoordinates(lat, lon);
  }
  persist(lat, lon);
  Serial.printf("Radar location saved: %s\n", s_label);
  return true;
}

bool saveFromStrings(const char* lat_str, const char* lon_str) {
  double lat = 0.0;
  double lon = 0.0;
  if (!util::coords::parseCoordinate(lat_str, &lat) ||
      !util::coords::parseCoordinate(lon_str, &lon)) {
    return false;
  }
  return save(lat, lon, nullptr);
}

bool consumeChanged() {
  const bool changed = s_changed;
  s_changed = false;
  return changed;
}

void clear() {
  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);
  prefs.remove(kKeyLat);
  prefs.remove(kKeyLon);
  prefs.remove(kKeyLabel);
  prefs.end();
  s_lat = config::kDefaultRadarLat;
  s_lon = config::kDefaultRadarLon;
  s_is_set = false;
  s_label[0] = '\0';
  s_changed = false;
}

}  // namespace services::location
