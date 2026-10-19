/**
 * Host check for the portal settings page.
 *
 * The bug this guards against was silent: WiFiManager built the settings page
 * as one Arduino String, String::concat gives no error when the heap cannot
 * grow, and the page simply stopped after six controls — no Save button, no
 * warning anywhere. Rendering the page off the board and asserting that every
 * control and the closing tag are present turns that failure into a red build.
 *
 * It also holds the setup page to what setup step 2 promised: with no
 * location stored the coordinate fields are empty (they used to carry the
 * default, which any Save then stored), and everything typed back is escaped.
 *
 * Build and run (also run by .github/workflows/quality.yml):
 *   g++ -std=gnu++17 -Wall -Wextra -Iinclude -Itools/host_checks/stubs \
 *       tools/host_checks/portal_page_test.cpp src/services/portal_pages.cpp \
 *       src/util/coords.cpp src/util/setup_text.cpp -o /tmp/portal_page_test
 *   /tmp/portal_page_test
 */

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "services/geocode.h"
#include "services/portal_pages.h"
#include "services/radar_location.h"
#include "ui/radar_display.h"

HostSerial Serial;
HostWiFi WiFi;

namespace {
bool g_location_set = true;
}

// Stand-ins for the firmware's stored settings, with values chosen so a
// mixed-up field shows up: not every checkbox is on, and no two drop-downs
// select the same position.
namespace services::location {
bool isSet() { return g_location_set; }
double lat() { return 52.520000; }
double lon() { return 13.405000; }
const char* label() { return g_location_set ? "Berlin, DE" : ""; }
bool save(double, double, const char*) { return true; }
bool saveFromStrings(const char*, const char*) { return true; }
}  // namespace services::location

namespace services::geocode {
void applyResult() {}
State state() { return State::kIdle; }
void asked(char* out, size_t len) { snprintf(out, len, "%s", ""); }
Request request(const char*) { return Request::kQueued; }
}  // namespace services::geocode

namespace services::adsb {
unsigned long lastUpdateMs() { return 3700000UL; }
const char* feedName() { return "adsb.fi"; }
const char* dataFeedName() { return "adsb.fi"; }
bool dataVerified() { return true; }
void feedStatusText(char* out, size_t n) {
  snprintf(out, n, "adsb.lol: 12 of 400 requests today");
}
}  // namespace services::adsb

namespace services::ota {
void requestCheck() {}
void statusText(char* out, size_t n) { snprintf(out, n, "up to date (checked 2 h ago)"); }
}  // namespace services::ota

namespace services::health {
const char* lastRestartText() { return "power on"; }
}  // namespace services::health

namespace ui {
bool radarDisplayCapture(FrameRowsFn, void*) { return false; }
}  // namespace ui

namespace ui::radar {
bool useMiles() { return false; }
bool showRunways() { return true; }
bool showTrackVectors() { return false; }
bool showTrails() { return true; }
bool dimAtNight() { return true; }
void saveDimAtNightFromPortal(const char*) {}
bool showClassIcons() { return true; }
uint8_t alertSeconds() { return 5; }
uint8_t fontStep() { return 1; }
uint8_t theme() { return 2; }
uint8_t txPowerDbm() { return 13; }
void saveMilesFromPortal(const char*) {}
void saveRunwaysFromPortal(const char*) {}
void saveTrackVectorsFromPortal(const char*) {}
void saveTrailsFromPortal(const char*) {}
void saveClassIconsFromPortal(const char*) {}
void saveAlertSecondsFromPortal(const char*) {}
void saveFontStepFromPortal(const char*) {}
void saveThemeFromPortal(const char*) {}
void saveTxPowerFromPortal(const char*) {}
}  // namespace ui::radar

namespace {

int g_failures = 0;

void expectContains(const std::string& page, const char* needle,
                    const char* what) {
  if (page.find(needle) == std::string::npos) {
    std::printf("FAIL  %s\n      missing: %s\n", what, needle);
    ++g_failures;
  }
}

void expectAbsent(const std::string& page, const char* needle, const char* what) {
  if (page.find(needle) != std::string::npos) {
    std::printf("FAIL  %s\n      present: %s\n", what, needle);
    ++g_failures;
  }
}

std::string render(WebServer& server, const char* route) {
  server.body.clear();
  server.routes[route]();
  return server.body;
}

}  // namespace

int main() {
  WebServer server;
  services::portal::registerLanRoutes(server, services::portal::Hooks{});

  for (const char* route : {"/settings", "/settingssave", "/locationsave", "/reset", "/param",
                            "/wifi", "/wifisave"}) {
    if (server.routes.count(route) == 0) {
      std::printf("FAIL  portal route %s was not registered\n", route);
      return 1;
    }
  }

  const std::string page = render(server, "/settings");

  // Every control the settings page owes the user, in the order they appear.
  const std::vector<std::pair<const char*, const char*>> controls = {
      {"name='town'", "town or ZIP code field"},
      {"name='pair'", "pasted coordinates field"},
      {"name='lat'", "latitude field"},
      {"name='lon'", "longitude field"},
      {"name='font'", "text size"},
      {"name='theme'", "theme"},
      {"name='miles'", "miles checkbox"},
      {"name='runways'", "runways checkbox"},
      {"name='icons'", "class icons checkbox"},
      {"name='track'", "direction lines checkbox"},
      {"name='trails'", "trails checkbox"},
      {"name='alert'", "alert duration"},
      {"name='tx'", "transmit power"},
      {"name='tok'", "CSRF token"},
  };
  for (const auto& control : controls) {
    expectContains(page, control.first, control.second);
  }

  // The page must reach its end: this is the truncation the check exists for.
  expectContains(page, "<button type='submit'>Save location</button>", "save location button");
  expectContains(page, "<button type='submit'>Save</button>", "save button");
  expectContains(page, "Reset for a new owner", "reset for a new owner");
  expectContains(page, "</form>", "closed form");
  expectContains(page, "</html>", "closed document");

  // Stored values must reach the form, or the page would quietly reset
  // settings the moment somebody pressed Save.
  expectContains(page, "value='52.520000'", "latitude prefilled");
  expectContains(page, "value='13.405000'", "longitude prefilled");
  expectContains(page, "value='2' selected", "text size step 2 selected");
  expectContains(page, "value='5' selected", "5 second alert selected");
  expectContains(page, "value='13' selected", "13 dBm selected");
  expectContains(page, "name='runways' value='T' checked", "runways ticked");
  expectContains(page, "name='track' value='T'>", "direction lines unticked");
  expectContains(page, "Open-Meteo", "geocoding attribution");

  // Navigation on every page was the other half of the complaint.
  expectContains(page, "href='/wifi'", "Wi-Fi link");
  expectContains(page, "href='/info'", "System link");

  // Setup step 2: nothing stored, so nothing pre-filled for Save to store.
  g_location_set = false;
  const std::string step2 = render(server, "/settings");
  expectContains(step2, "Step 2 of 2", "step 2 banner");
  expectContains(step2, "value=''>", "empty coordinate fields");
  expectAbsent(step2, "value='52.520000'", "no default latitude");
  expectContains(step2, "</html>", "step 2 page closed");
  g_location_set = true;

  // The home-network Wi-Fi page explains instead of taking credentials.
  const std::string wifi = render(server, "/wifi");
  expectContains(wifi, "10 seconds", "how to change Wi-Fi");
  expectAbsent(wifi, "name='p'", "no password field");

  if (g_failures != 0) {
    std::printf("\n%d check(s) failed; page was %zu bytes\n", g_failures,
                page.size());
    return 1;
  }
  std::printf("portal settings page: %zu bytes, %zu controls, all present\n",
              page.size(), controls.size());
  return 0;
}
