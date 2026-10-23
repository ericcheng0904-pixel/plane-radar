#include "services/welcome.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cstring>

#include "util/portal_input.h"

namespace services::welcome {

namespace {

/** Separate from the location's and the display settings' namespaces. */
constexpr char kPrefsNamespace[] = "welcome";
constexpr char kKeyName[] = "name";
constexpr char kKeyShown[] = "shown";
constexpr char kKeyGuide[] = "guide";

char s_name[util::portal::kMaxRadarName + 1] = "";
bool s_shown = false;
bool s_guide_seen = false;
bool s_guide_opened = false;

/** Store `key` as true. Written once: the callers skip it when already set. */
void putFlag(const char* key) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(key, true);
  prefs.end();
}

}  // namespace

void init() {
  Preferences prefs;
  // Fails while the namespace does not exist yet: a new radar, or one just
  // reset for a new owner. No name then, and the welcome is due.
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  s_shown = prefs.getBool(kKeyShown, false);
  s_guide_seen = prefs.getBool(kKeyGuide, false);
  char stored[sizeof(s_name) + 1] = "";
  if (prefs.isKey(kKeyName)) {
    prefs.getString(kKeyName, stored, sizeof(stored));
  }
  prefs.end();
  // Checked again on the way in: the screen and the pages rely on it.
  if (!util::portal::radarName(stored, s_name, sizeof(s_name))) {
    s_name[0] = '\0';
  }
}

const char* name() { return s_name; }

bool saveName(const char* value) {
  char clean[sizeof(s_name)];
  if (!util::portal::radarName(value, clean, sizeof(clean))) {
    return false;
  }
  if (std::strcmp(clean, s_name) == 0) {
    return true;  // every settings save sends it: no flash write for nothing
  }
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return false;
  }
  if (clean[0] == '\0') {
    prefs.remove(kKeyName);
  } else {
    prefs.putString(kKeyName, clean);
  }
  prefs.end();
  std::memcpy(s_name, clean, sizeof(s_name));
  Serial.printf("Radar name saved: '%s'\n", s_name);
  return true;
}

bool due() { return !s_shown; }

void markShown() {
  if (s_shown) {
    return;
  }
  s_shown = true;
  putFlag(kKeyShown);
}

void noteGuideOpened() {
  s_guide_opened = true;
  if (!s_guide_seen) {
    s_guide_seen = true;
    putFlag(kKeyGuide);
  }
}

bool consumeGuideOpened() {
  const bool opened = s_guide_opened;
  s_guide_opened = false;
  return opened;
}

bool guideSeen() { return s_guide_seen; }

void clear() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  s_name[0] = '\0';
  s_shown = false;
  s_guide_seen = false;
  s_guide_opened = false;
}

}  // namespace services::welcome
