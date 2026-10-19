#include "services/portal_pages.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif

#include "config.h"
#include "services/adsb_client.h"
#include "services/device_health.h"
#include "services/geocode.h"
#include "services/ota.h"
#include "services/radar_location.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "util/coords.h"
#include "util/geocode.h"
#include "util/portal_input.h"
#include "util/setup_text.h"

namespace services::portal {

namespace {

constexpr char kSettingsPath[] = "/settings";
constexpr char kSavePath[] = "/settingssave";
constexpr char kLocationSavePath[] = "/locationsave";
constexpr char kResetPath[] = "/reset";
constexpr char kUpdateCheckPath[] = "/updatecheck";
constexpr char kScreenPath[] = "/screen.bmp";

/**
 * Navigation shown on every portal page. WiFiManager renders each page as a
 * dead end with at most a Back button, which is how the settings ended up
 * hidden; these four links are the whole map of the interface.
 */
void sendNav(WebServer& server, const char* active) {
  static const char* const kLinks[][2] = {
      {"/", "Home"}, {"/wifi", "Wi-Fi"}, {"/settings", "Settings"}, {"/info", "System"}};
  server.sendContent("<div id='nv'>");
  for (const auto& link : kLinks) {
    char buf[80];
    snprintf(buf, sizeof(buf), "<a%s href='%s'>%s</a>",
             strcmp(link[0], active) == 0 ? " class='on'" : "", link[0], link[1]);
    server.sendContent(buf);
  }
  server.sendContent("</div>");
}

/** Styles for our own pages: same palette as WiFiManager, nothing loaded. */
constexpr char kStyleHtml[] =
    "<style>"
    ":root{color-scheme:light}"
    "body{background:#fff;color:#222;font-family:system-ui,-apple-system,"
    "sans-serif;margin:0;padding:16px}"
    ".wrap{max-width:430px;margin:0 auto;text-align:left}"
    "h1{font-size:20px;margin:0 0 14px}"
    "p,li{font-size:15px;line-height:1.45}"
    "#nv{display:flex;gap:5px;margin:0 0 16px}"
    "#nv a{flex:1;text-align:center;padding:9px 4px;border-radius:8px;"
    "background:#f2f2f2;border:1px solid #dcdcdc;color:#444;"
    "text-decoration:none;font-size:14px}"
    "#nv a.on{background:#1fa3ec;border-color:#1fa3ec;color:#fff}"
    ".sc{margin:22px 0 8px;font-size:12px;font-weight:700;letter-spacing:.08em;"
    "text-transform:uppercase;color:#777;border-bottom:1px solid #e0e0e0;"
    "padding-bottom:5px}"
    ".hn{font-size:12px;line-height:1.45;color:#888;margin:2px 0 12px}"
    "label{display:block;font-size:14px;margin:10px 0 3px}"
    "label.cb{display:flex;align-items:center;gap:8px;margin:11px 0}"
    "input[type=number],input[type=text],select{width:100%;box-sizing:border-box;"
    "padding:8px;font-size:16px;border:1px solid #ccc;border-radius:6px;"
    "background:#fff;color:#222}"
    "input[type=checkbox]{width:18px;height:18px;margin:0;flex:none}"
    "summary{font-size:14px;margin:12px 0 4px;color:#1a73c8;cursor:pointer}"
    "button{width:100%;margin:24px 0 8px;padding:12px;font-size:16px;"
    "border:0;border-radius:6px;background:#1fa3ec;color:#fff;cursor:pointer}"
    "button.d{background:#dc3630}"
    "a.bt{display:block;text-align:center;padding:12px;border-radius:6px;"
    "border:1px solid #ccc;color:#444;text-decoration:none}"
    ".ok,.st,.er{margin:0 0 14px;padding:10px 12px;border-radius:6px;font-size:14px;"
    "line-height:1.45}"
    ".ok{background:#e6f6e6;border:1px solid #bfe3bf;color:#245c24}"
    ".st{background:#fff6e5;border:1px solid #f3d29a;color:#5a3d00}"
    ".er{background:#fdecea;border:1px solid #f5c2bd;color:#8a1f15}"
    "</style>";

/**
 * Cross-site request forgery token, one per boot.
 *
 * The portal has no login — anyone on the same network can open it, which is
 * the same trust model WiFiManager ships with. Without a token, though, any web
 * page the user happens to visit could silently POST to the device and change
 * its settings, because a browser attaches no origin restriction to a plain
 * form submit. The token is only readable by fetching the settings page itself,
 * which the browser's same-origin policy denies to a foreign page.
 */
char s_form_token[9] = {};

const char* formToken() {
  if (s_form_token[0] == '\0') {
    snprintf(s_form_token, sizeof(s_form_token), "%08lx",
             static_cast<unsigned long>(esp_random()));
  }
  return s_form_token;
}

/** A POST without this boot's token is refused (403) and returns false. */
bool tokenOk(WebServer& server) {
  if (server.arg("tok") == formToken()) {
    return true;
  }
  server.send(403, "text/plain", "Stale settings form. Reload /settings and try again.");
  return false;
}

void sendFormStart(WebServer& server, const char* action) {
  char buf[128];
  snprintf(buf, sizeof(buf),
           "<form method='POST' action='%s'><input type='hidden' name='tok' value='%s'>", action,
           formToken());
  server.sendContent(buf);
}

/**
 * Status line, chunked document start, styles and heading. `refresh_s` > 0
 * reloads the page by itself (a lookup under way); `nav` is the active link,
 * or nullptr on the hotspot, where there is nowhere else to go.
 */
void sendPageStart(WebServer& server, const char* title, const char* nav, int refresh_s = 0) {
  // Chunked output: the page never exists in RAM as a whole, so its length is
  // independent of how fragmented the heap is by the time it is requested.
  server.sendHeader("Cache-Control", "no-store");
  // No other page may frame these: "Reset for a new owner" is one click away.
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Content-Security-Policy", "frame-ancestors 'none'");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  char refresh[48] = "";
  if (refresh_s > 0) {
    snprintf(refresh, sizeof(refresh), "<meta http-equiv='refresh' content='%d'>", refresh_s);
  }
  char buf[240];
  snprintf(buf, sizeof(buf),
           "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "%s<title>%s - Plane Radar</title>",
           refresh, title);
  server.sendContent(buf);
  server.sendContent(kStyleHtml);
  server.sendContent("</head><body><div class='wrap'><h1>Plane Radar</h1>");
  if (nav != nullptr) {
    sendNav(server, nav);
  }
}

void sendPageEnd(WebServer& server) {
  server.sendContent("</div></body></html>");
  server.sendContent("");
}

void sendCheckbox(WebServer& server, const char* name, const char* label,
                  bool checked) {
  char buf[220];
  snprintf(buf, sizeof(buf),
           "<label class='cb'><input type='checkbox' name='%s' value='T'%s>"
           "<span>%s</span></label>",
           name, checked ? " checked" : "", label);
  server.sendContent(buf);
}

/**
 * A drop-down over a fixed set of numbers. The stored value is validated again
 * on save, so a hand-crafted POST gains nothing by picking another option.
 */
void sendChoiceSelect(WebServer& server, const char* name, const char* label,
                      const uint8_t* choices, size_t count, uint8_t current,
                      const char* const* option_labels) {
  char buf[200];
  snprintf(buf, sizeof(buf), "<label for='%s'>%s</label><select id='%s' name='%s'>",
           name, label, name, name);
  server.sendContent(buf);
  for (size_t i = 0; i < count; ++i) {
    snprintf(buf, sizeof(buf), "<option value='%u'%s>%s</option>",
             static_cast<unsigned>(choices[i]),
             choices[i] == current ? " selected" : "", option_labels[i]);
    server.sendContent(buf);
  }
  server.sendContent("</select>");
}

/** Whether `s` has anything but spaces in it. */
bool filled(const String& s) {
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] != ' ' && s[i] != '\t') {
      return true;
    }
  }
  return false;
}

/** The outcome of /locationsave, carried to /settings as a code (never the text). */
void sendLocationNotice(WebServer& server, const String& code) {
  // Room for the longest escaped text: every character can become "&quot;".
  char buf[640];
  char place[location::kMaxLabel * 6 + 1];
  char asked[util::geocode::kMaxQuery * 6 + 1];
  util::setup::htmlEscape(location::label(), place, sizeof(place));
  if (code == "saved") {
    snprintf(buf, sizeof(buf),
             "<div class='ok'>Location saved. The radar now shows planes near "
             "<b>%s</b>.</div>",
             place);
  } else if (code == "looking") {
    char raw[util::geocode::kMaxQuery + 1];
    geocode::asked(raw, sizeof(raw));
    util::setup::htmlEscape(raw, asked, sizeof(asked));
    switch (geocode::state()) {
      case geocode::State::kWaiting:
        snprintf(buf, sizeof(buf), "<div class='st'>Looking up &ldquo;%s&rdquo;&hellip;</div>",
                 asked);
        break;
      case geocode::State::kFound:
        snprintf(buf, sizeof(buf),
                 "<div class='ok'>Found <b>%s</b> (%.4f, %.4f). The radar shows planes "
                 "near it now. Not the right place? Add the state (Evanston IL) or "
                 "paste coordinates.</div>",
                 place, location::lat(), location::lon());
        break;
      case geocode::State::kNotFound:
        snprintf(buf, sizeof(buf),
                 "<div class='er'>Could not find &ldquo;%s&rdquo;. Add the state "
                 "(Evanston IL) or the country, or paste coordinates.</div>",
                 asked);
        break;
      case geocode::State::kFailed:
        snprintf(buf, sizeof(buf),
                 "<div class='er'>The town search did not answer. Check that the "
                 "radar's Wi-Fi reaches the internet, or paste coordinates "
                 "instead.</div>");
        break;
      case geocode::State::kIdle:
      default:
        return;
    }
  } else if (code == "badpair") {
    snprintf(buf, sizeof(buf),
             "<div class='er'>Could not read those coordinates. Paste two numbers, "
             "latitude first (41.88, -87.63), or a Google Maps link.</div>");
  } else if (code == "badll") {
    snprintf(buf, sizeof(buf),
             "<div class='er'>Fill in both latitude and longitude as decimal numbers, "
             "negative for south and west.</div>");
  } else if (code == "badtown") {
    snprintf(buf, sizeof(buf),
             "<div class='er'>Type a town name, or a five-digit ZIP code.</div>");
  } else if (code == "busy") {
    snprintf(buf, sizeof(buf),
             "<div class='er'>Still looking up the last one; try again in a few "
             "seconds.</div>");
  } else if (code == "empty") {
    snprintf(buf, sizeof(buf),
             "<div class='er'>Nothing entered: type a town or ZIP code, or paste "
             "coordinates.</div>");
  } else if (code == "same") {
    snprintf(buf, sizeof(buf), "<div class='ok'>Location unchanged.</div>");
  } else {
    return;
  }
  server.sendContent(buf);
}

/**
 * The location form. Its own form and its own endpoint: when the coordinates
 * sat in the one big form, saving any display option saved them too — and
 * with nothing stored they were pre-filled with the default, so the radar
 * quietly moved to Amsterdam.
 */
void sendLocationSection(WebServer& server) {
  char buf[480];
  const bool set = location::isSet();
  server.sendContent("<div class='sc'>Location</div>");
  if (set) {
    char place[location::kMaxLabel * 6 + 1];
    util::setup::htmlEscape(location::label(), place, sizeof(place));
    snprintf(buf, sizeof(buf),
             "<div class='hn'>Showing planes near <b>%s</b>. To move the radar, "
             "use any one of these.</div>",
             place);
    server.sendContent(buf);
  }
  sendFormStart(server, kLocationSavePath);
  server.sendContent(
      "<label for='town'>Town or ZIP code</label>"
      "<input id='town' name='town' type='text' maxlength='64' "
      "placeholder='Evanston IL or 60201' autocomplete='off'>"
      "<div class='hn'>The radar looks it up itself, with the free geocoding "
      "service of <a href='https://open-meteo.com/'>Open-Meteo.com</a> (location "
      "data based on GeoNames, CC BY 4.0). A ZIP code finds the middle of its "
      "town; paste coordinates to be exact.</div>"
      "<label for='pair'>Or paste coordinates</label>"
      "<input id='pair' name='pair' type='text' maxlength='1024' "
      "placeholder='41.88, -87.63' autocapitalize='none' autocorrect='off' "
      "spellcheck='false'>"
      "<div class='hn'>From a map app. Google Maps: touch and hold the spot, then "
      "tap the numbers at the top to copy them (or copy the link). Apple Maps: "
      "touch and hold to drop a pin, swipe up, and copy the coordinates.</div>"
      "<details><summary>Exact latitude and longitude</summary>"
      "<div class='hn'>Decimal degrees, negative for south and west.</div>");
  // Empty while nothing is stored: a pre-filled default would be saved back
  // by anyone who pressed Save without reading it.
  char value[24] = "";
  if (set) {
    snprintf(value, sizeof(value), "%.6f", location::lat());
  }
  snprintf(buf, sizeof(buf),
           "<label for='lat'>Latitude</label>"
           "<input id='lat' name='lat' type='number' step='0.000001' "
           "min='-90' max='90' value='%s'>",
           value);
  server.sendContent(buf);
  value[0] = '\0';
  if (set) {
    snprintf(value, sizeof(value), "%.6f", location::lon());
  }
  snprintf(buf, sizeof(buf),
           "<label for='lon'>Longitude</label>"
           "<input id='lon' name='lon' type='number' step='0.000001' "
           "min='-180' max='180' value='%s'>",
           value);
  server.sendContent(buf);
  server.sendContent(
      "</details><button type='submit'>Save location</button></form>"
      "<div class='hn'>The lake and shoreline layer covers the Chicago area only; "
      "elsewhere the radar simply shows no water.</div>");
}

void sendDisplaySections(WebServer& server) {
  sendFormStart(server, kSavePath);
  server.sendContent("<div class='sc'>Display</div>");
  {
    static const uint8_t kFontChoices[] = {1, 2, 3};
    static const char* const kFontLabels[] = {"1 - normal", "2 - small",
                                              "3 - smallest"};
    sendChoiceSelect(server, "font", "Text size", kFontChoices,
                     sizeof(kFontChoices) / sizeof(kFontChoices[0]),
                     static_cast<uint8_t>(ui::radar::fontStep() + 1),
                     kFontLabels);
  }
  {
    static const uint8_t kThemeChoices[] = {0, 1, 2, 3};
    static const char* const kThemeLabels[] = {
        "Scope - navy, altitude colours", "Map - blue, dotted land",
        "Classic - green on black", "Sweep - rotating beam"};
    sendChoiceSelect(server, "theme", "Theme", kThemeChoices,
                     sizeof(kThemeChoices) / sizeof(kThemeChoices[0]),
                     ui::radar::theme(), kThemeLabels);
  }
  sendCheckbox(server, "miles", "Distances in miles instead of kilometres",
               ui::radar::useMiles());
  sendCheckbox(server, "runways", "Show airport runways",
               ui::radar::showRunways());
  sendCheckbox(server, "icons",
               "Aircraft shapes by kind (regional, narrow-body, wide-body, engines)",
               ui::radar::showClassIcons());
  sendCheckbox(server, "track", "Show direction lines",
               ui::radar::showTrackVectors());
  sendCheckbox(server, "trails", "Show flight trails",
               ui::radar::showTrails());
  sendCheckbox(server, "night", "Dim the screen at night (dusk to dawn where the radar is)",
               ui::radar::dimAtNight());

  server.sendContent(
      "<div class='sc'>Alerts</div>"
      "<div class='hn'>The radar pulses a coloured ring when an emergency, a "
      "military aircraft or a rare type first appears.</div>");
  {
    static const char* const kAlertLabels[] = {"Off", "3 seconds", "5 seconds",
                                              "7 seconds"};
    sendChoiceSelect(server, "alert", "Flash duration",
                     ui::radar::kAlertSecondsChoices,
                     ui::radar::kAlertSecondsChoiceCount,
                     ui::radar::alertSeconds(), kAlertLabels);
  }

  server.sendContent(
      "<div class='sc'>Network</div>"
      "<div class='hn'>Raise the transmit power if the connection drops in a "
      "weak spot; it costs current, and therefore heat.</div>");
  {
    static const char* const kTxLabels[] = {"8 dBm - low", "13 dBm - medium",
                                           "19 dBm - high"};
    sendChoiceSelect(server, "tx", "Wi-Fi transmit power",
                     ui::radar::kTxPowerChoices, ui::radar::kTxPowerChoiceCount,
                     ui::radar::txPowerDbm(), kTxLabels);
  }

  server.sendContent(
      "<button type='submit'>Save</button></form>"
      "<div class='hn'>Saved settings survive a restart. Hold the button on the "
      "radar for ten seconds to clear the saved Wi-Fi; location and these "
      "settings are kept.</div>");
}

void sendStatusLine(WebServer& server) {
  // Status in words, for whoever is looking after the radar.
  const unsigned long last = services::adsb::lastUpdateMs();
  const unsigned long up_min = millis() / 60000;
  char data[128];
  if (last == 0) {
    snprintf(data, sizeof(data), "no flight data yet (trying %s)",
             services::adsb::feedName());
  } else {
    // The feed being asked can differ from the one the data came from
    // around a failover; name both then.
    const char* from = services::adsb::dataFeedName();
    const char* next = services::adsb::feedName();
    char trying[32] = "";
    if (strcmp(from, next) != 0) {
      snprintf(trying, sizeof(trying), ", now trying %s", next);
    }
    snprintf(data, sizeof(data), "flight data from %s %lu s ago%s%s", from,
             (millis() - last) / 1000,
             services::adsb::dataVerified() ? "" : " (certificate not checked)", trying);
  }
  char buf[256];
  snprintf(buf, sizeof(buf),
           "<div class='hn'>Status: %s &middot; running %lu h %lu min &middot; "
           "last restart: %s</div>",
           data, up_min / 60, up_min % 60, services::health::lastRestartText());
  server.sendContent(buf);
  char feeds[160];
  services::adsb::feedStatusText(feeds, sizeof(feeds));
  if (feeds[0] != '\0') {
    // Feed names and numbers only: nothing in it needs escaping.
    snprintf(buf, sizeof(buf), "<div class='hn'>Feeds: %s</div>", feeds);
    server.sendContent(buf);
  }
}

void sendAboutSection(WebServer& server);

void handleSettings(WebServer& server) {
  // A lookup that finished since the last loop pass shows as applied.
  geocode::applyResult();
  const String loc = server.arg("loc");
  const bool looking = loc == "looking" && geocode::state() == geocode::State::kWaiting;
  sendPageStart(server, "Settings", kSettingsPath, looking ? 2 : 0);

  if (server.hasArg("saved")) {
    server.sendContent("<div class='ok'>Settings saved.</div>");
  }
  sendLocationNotice(server, loc);

  if (!location::isSet()) {
    // Setup step 2: the location is the only thing asked for, so it is the
    // only thing on the page.
    server.sendContent(
        "<div class='st'><b>Step 2 of 2:</b> tell the radar where it is. Type "
        "your town or ZIP code, or paste coordinates from a map app, then tap "
        "<b>Save location</b>.</div>");
    sendLocationSection(server);
    server.sendContent(
        "<div class='hn'>The other settings appear here once the radar knows "
        "where it is.</div>");
    sendPageEnd(server);
    return;
  }

  sendStatusLine(server);
  sendLocationSection(server);
  sendDisplaySections(server);

  sendAboutSection(server);

  server.sendContent(
      "<div class='sc'>Giving the radar away</div>"
      "<div class='hn'>Erases the location, the display settings and the Wi-Fi, "
      "and restarts at setup step 1 for the next person.</div>");
  sendFormStart(server, kResetPath);
  server.sendContent(
      "<button class='d' type='submit'>Reset for a new owner&hellip;</button></form>");
  sendPageEnd(server);
}

/**
 * Version, updates, where the code lives, and whose data the radar shows
 * (adsb.fi's terms ask for a link). URLs are spelled out as well as linked:
 * a phone on the radar's hotspot has no internet to follow them.
 */
void sendAboutSection(WebServer& server) {
  char status[160];
  services::ota::statusText(status, sizeof(status));
  char escaped[sizeof(status) * 6];
  util::setup::htmlEscape(status, escaped, sizeof(escaped));
  char buf[sizeof(escaped) + 320];
  snprintf(buf, sizeof(buf),
           "<div class='sc'>About</div>"
           "<div class='hn'>Plane Radar %s &middot; <a href='%s'>%s</a><br>"
           "Updates: %s. Signed updates install by themselves, about once a day; "
           "Wi-Fi, location and settings are kept.</div>",
           config::kFirmwareVersion, config::kRepoUrl, config::kRepoUrl + 8, escaped);
  server.sendContent(buf);
  sendFormStart(server, kUpdateCheckPath);
  server.sendContent(
      "<button type='submit'>Check for an update now</button></form>"
      "<div class='hn'>Flight data: <a href='https://adsb.fi'>adsb.fi</a> and "
      "<a href='https://adsb.lol'>adsb.lol</a> (ODbL) &middot; routes: "
      "<a href='https://adsb.im'>adsb.im</a> &middot; town search: Open-Meteo "
      "(GeoNames, CC BY 4.0) &middot; lake outline &copy; OpenStreetMap "
      "contributors (ODbL) &middot; airports: OurAirports &middot; airlines: "
      "OpenFlights (ODbL) &middot; font: Noto Sans (OFL). Airline logos, where "
      "shown, are their owners' trademarks.</div>");
}

void handleUpdateCheck(WebServer& server) {
  if (!tokenOk(server)) {
    return;
  }
  services::ota::requestCheck();
  sendPageStart(server, "Updates", kSettingsPath, 20);
  server.sendContent(
      "<div class='ok'>Looking for an update now. This page goes back to the "
      "settings in 20 seconds; the result is under About.</div>");
  sendPageEnd(server);
}

/** True when the named checkbox arrived ticked. Absent means unticked. */
bool checkboxArg(WebServer& server, const char* name) {
  if (!server.hasArg(name)) {
    return false;
  }
  const String value = server.arg(name);
  return util::portal::checkboxChecked(value.c_str());
}

void redirect(WebServer& server, const char* to) {
  server.sendHeader("Location", to);
  server.send(302, "text/plain", "");
}

void handleSave(WebServer& server, void (*on_saved)()) {
  if (!tokenOk(server)) {
    return;
  }

  // Every value goes through the same validating parsers the host test suite
  // covers; anything malformed leaves the stored setting untouched.
  ui::radar::saveFontStepFromPortal(server.arg("font").c_str());
  ui::radar::saveThemeFromPortal(server.arg("theme").c_str());
  ui::radar::saveMilesFromPortal(checkboxArg(server, "miles") ? "T" : "");
  ui::radar::saveRunwaysFromPortal(checkboxArg(server, "runways") ? "T" : "");
  ui::radar::saveClassIconsFromPortal(checkboxArg(server, "icons") ? "T" : "");
  ui::radar::saveTrackVectorsFromPortal(checkboxArg(server, "track") ? "T" : "");
  ui::radar::saveDimAtNightFromPortal(checkboxArg(server, "night") ? "T" : "");
  ui::radar::saveTrailsFromPortal(checkboxArg(server, "trails") ? "T" : "");
  ui::radar::saveAlertSecondsFromPortal(server.arg("alert").c_str());
  ui::radar::saveTxPowerFromPortal(server.arg("tx").c_str());

  if (on_saved != nullptr) {
    on_saved();
  }

  char location[64];
  snprintf(location, sizeof(location), "%s?saved=1", kSettingsPath);
  redirect(server, location);
}

/**
 * Whichever one field was filled in: pasted coordinates, then a town, then
 * the exact pair. Empty fields are never saved — leaving them blank keeps
 * the stored location.
 */
void handleLocationSave(WebServer& server) {
  if (!tokenOk(server)) {
    return;
  }
  const String pair = server.arg("pair");
  const String town = server.arg("town");
  String lat = server.arg("lat");
  String lon = server.arg("lon");
  lat.trim();
  lon.trim();

  const char* outcome = "empty";
  if (filled(pair)) {
    double la = 0.0;
    double lo = 0.0;
    outcome = util::coords::parseLatLonPair(pair.c_str(), &la, &lo) &&
                      location::save(la, lo, nullptr)
                  ? "saved"
                  : "badpair";
  } else if (filled(town)) {
    switch (geocode::request(town.c_str())) {
      case geocode::Request::kQueued:
        outcome = "looking";
        break;
      case geocode::Request::kBusy:
        outcome = "busy";
        break;
      case geocode::Request::kBadText:
      default:
        outcome = "badtown";
        break;
    }
  } else if (filled(lat) || filled(lon)) {
    // The exact fields come back pre-filled; saved unchanged they would only
    // swap a found town's name for numbers.
    double la = 0.0;
    double lo = 0.0;
    const bool parsed = util::coords::parseCoordinate(lat.c_str(), &la) &&
                        util::coords::parseCoordinate(lon.c_str(), &lo);
    const bool same = parsed && location::isSet() && std::fabs(la - location::lat()) < 5e-7 &&
                      std::fabs(lo - location::lon()) < 5e-7;
    if (same) {
      outcome = "same";
    } else {
      outcome = parsed && location::save(la, lo, nullptr) ? "saved" : "badll";
    }
  }
  char to[48];
  snprintf(to, sizeof(to), "%s?loc=%s", kSettingsPath, outcome);
  redirect(server, to);
}

/** Two steps, both POST with the token: a page that says what goes, then the reset. */
void handleReset(WebServer& server, void (*on_reset)(bool)) {
  if (!tokenOk(server)) {
    return;
  }
  if (server.arg("confirm") != "yes") {
    sendPageStart(server, "Reset", kSettingsPath);
    server.sendContent(
        "<div class='sc'>Reset for a new owner</div>"
        "<p>This erases where the radar is, its display settings and its Wi-Fi. "
        "It restarts and shows <b>Step 1 of 2</b>, ready for the next person to "
        "set it up with their phone.</p>");
    sendFormStart(server, kResetPath);
    server.sendContent(
        "<input type='hidden' name='confirm' value='yes'>"
        "<label class='cb'><input type='checkbox' name='keep' value='1'>"
        "Keep the location (the radar is staying in this area)</label>"
        "<button class='d' type='submit'>Yes, erase everything</button></form>"
        "<a class='bt' href='/settings'>Cancel</a>");
    sendPageEnd(server);
    return;
  }
  sendPageStart(server, "Reset", nullptr);
  server.sendContent(
      "<div class='ok'>Erasing. The radar restarts in a few seconds and shows "
      "<b>Step 1 of 2</b>. You can close this page.</div>");
  sendPageEnd(server);
  if (on_reset != nullptr) {
    on_reset(server.arg("keep") == "1");
  }
}

/**
 * /wifi on the home network. WiFiManager's form there saved new credentials
 * before trying them, so one typo cut the radar off from the network the page
 * was reached through, with nothing on the phone to say so. The hotspot path
 * checks a network before keeping it; this page sends people there.
 */
void handleLanWifi(WebServer& server) {
  sendPageStart(server, "Wi-Fi", "/wifi");
  char ssid[33 * 6 + 1];
  util::setup::htmlEscape(WiFi.SSID().c_str(), ssid, sizeof(ssid));
  const int rssi = WiFi.RSSI();
  const char* strength = rssi >= -60 ? "strong" : rssi >= -72 ? "fair" : "weak";
  char buf[400];
  snprintf(buf, sizeof(buf),
           "<div class='sc'>Wi-Fi</div>"
           "<p>Connected to <b>%s</b>; the signal is %s (%d dBm).</p>",
           ssid, strength, rssi);
  server.sendContent(buf);
  server.sendContent(
      "<p>To move the radar to another network &mdash; a new router, a new "
      "Wi-Fi password, a new home:</p><ol>"
      "<li>Hold the button on the radar for 10 seconds, until it says "
      "<b>Wi-Fi reset</b>. A countdown shows after two seconds.</li>"
      "<li>It restarts with its own setup Wi-Fi. Scan the code on its screen "
      "with your phone and pick the new network.</li></ol>"
      "<div class='hn'>Its location and settings are kept. Changing networks "
      "from this page is switched off: a mistyped password would cut the radar "
      "off from this network before it could tell you.</div>");
  sendPageEnd(server);
}

/**
 * The hotspot's answer to the Wi-Fi form. Nothing is stored here: the radar
 * tries the network first (wifi_setup.cpp) and keeps it only if it works,
 * showing the outcome on its own screen — the phone loses the hotspot while
 * the radar tries, so it could not show it anyway.
 */
void handleHotspotSave(WebServer& server, void (*on_join)(const char*, const char*)) {
  const String ssid = server.arg("s");
  const String pass = server.arg("p");
  char name[33 * 6 + 1];
  util::setup::htmlEscape(ssid.c_str(), name, sizeof(name));
  if (!util::setup::plausibleSsid(ssid.c_str()) ||
      !util::setup::plausiblePassword(pass.c_str())) {
    sendPageStart(server, "Wi-Fi setup", nullptr);
    server.sendContent(
        ssid.length() == 0
            ? "<div class='er'>Pick your home network from the list first.</div>"
            : "<div class='er'>That password cannot be right: Wi-Fi passwords have "
              "8 to 63 characters.</div>");
    server.sendContent("<a class='bt' href='/wifi'>Back</a>");
    sendPageEnd(server);
    return;
  }
  if (on_join != nullptr) {
    on_join(ssid.c_str(), pass.c_str());
  }
  const bool two_steps = !location::isSet();
  sendPageStart(server, "Wi-Fi setup", nullptr);
  char buf[480];
  snprintf(buf, sizeof(buf),
           "<div class='st'><b>%s</b> the radar is joining <b>%s</b>.</div>"
           "<p>Watch the radar's screen.</p>",
           two_steps ? "Step 1 of 2:" : "Wi-Fi setup:", name);
  server.sendContent(buf);
  server.sendContent(
      two_steps ? "<p>When it says <b>Step 2 of 2</b>, put your phone back on your "
                  "home Wi-Fi and scan the new code.</p>"
                : "<p>When it shows planes again, you are done: put your phone back "
                  "on your home Wi-Fi.</p>");
  snprintf(buf, sizeof(buf),
           "<div class='hn'>If it says <b>Could not join</b> or <b>Not found</b>, "
           "join <b>%s</b> again and pick the network once more.</div>",
           config::kPortalApName);
  server.sendContent(buf);
  sendPageEnd(server);
}

/** Little-endian field writers for the BMP header. */
void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void putLe32(uint8_t* p, uint32_t v) {
  putLe16(p, static_cast<uint16_t>(v));
  putLe16(p + 2, static_cast<uint16_t>(v >> 16));
}

/** Where /screen.bmp is in its stream, between bands of the frame. */
struct ScreenStream {
  WebServer* server;
  unsigned long started_ms;
  bool aborted;
};

/**
 * Converts one band of the frame to BMP rows and sends it. Everything else on
 * the main loop waits while this runs, so a slow or stalled client gets a
 * fixed budget, not the whole display. Rows go out four at a time: one
 * 720-byte write per row ran at ~45 KB/s.
 */
void sendScreenRows(const uint8_t* px, int /*y0*/, int rows, int w, void* ctx) {
  auto* st = static_cast<ScreenStream*>(ctx);
  constexpr unsigned long kBudgetMs = 8000;
  constexpr int kRowsPerWrite = 4;
  static uint8_t out[kRowsPerWrite * 240 * 3];
  if (w > 240) {
    st->aborted = true;
  }
  const size_t row_bytes = static_cast<size_t>(w) * 3u;
  size_t filled = 0;
  for (int y = 0; y < rows && !st->aborted; ++y) {
    if (!st->server->client().connected() || millis() - st->started_ms > kBudgetMs) {
      st->aborted = true;
      break;
    }
    uint8_t* row = out + filled;
    const uint8_t* src = px + static_cast<size_t>(y) * static_cast<size_t>(w) * 2u;
    for (int x = 0; x < w; ++x) {
      const uint16_t c = static_cast<uint16_t>((src[2 * x] << 8) | src[2 * x + 1]);
      // The panel shows the sprite's R and B swapped (see panelColor() in
      // radar_display), so swap here too: the BMP should match the glass.
      const uint8_t hi5 = (c >> 11) & 0x1F;
      const uint8_t g6 = (c >> 5) & 0x3F;
      const uint8_t lo5 = c & 0x1F;
      const uint8_t r5 = config::kDisplayRgbOrder ? lo5 : hi5;
      const uint8_t b5 = config::kDisplayRgbOrder ? hi5 : lo5;
      row[3 * x + 0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
      row[3 * x + 1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
      row[3 * x + 2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    }
    filled += row_bytes;
    if (filled + row_bytes > sizeof(out) || y == rows - 1) {
      st->server->sendContent(reinterpret_cast<const char*>(out), filled);
      filled = 0;
    }
  }
}

/**
 * The radar as the panel shows it, as a 24-bit top-down BMP. The frame only
 * ever exists half at a time, so a fresh one is drawn for the picture and
 * streamed band by band as each half is finished. Read-only; the same picture
 * anyone in the room can see on the device.
 */
void handleScreen(WebServer& server) {
  if (!location::isSet()) {
    // The panel shows setup step 2, which is not drawn through the frame.
    server.send(503, "text/plain", "setup not finished: no location yet");
    return;
  }
  constexpr int kW = 240;
  constexpr int kH = 240;
  const uint32_t row_bytes = static_cast<uint32_t>(kW) * 3u;  // 240*3: 4-aligned
  const uint32_t image_bytes = row_bytes * static_cast<uint32_t>(kH);
  uint8_t header[54] = {'B', 'M'};
  putLe32(header + 2, 54u + image_bytes);
  putLe32(header + 10, 54u);  // pixel data offset
  putLe32(header + 14, 40u);  // BITMAPINFOHEADER
  putLe32(header + 18, static_cast<uint32_t>(kW));
  putLe32(header + 22, static_cast<uint32_t>(-kH));  // negative: top-down rows
  putLe16(header + 26, 1u);
  putLe16(header + 28, 24u);
  putLe32(header + 34, image_bytes);

  ScreenStream st{&server, millis(), false};
  // Headers go out only once a frame is sure to follow.
  struct Gate {
    ScreenStream* st;
    const uint8_t* header;
    bool sent;
  } gate{&st, header, false};
  auto sink = [](const uint8_t* px, int y0, int rows, int w, void* ctx) {
    auto* g = static_cast<Gate*>(ctx);
    if (!g->sent) {
      g->sent = true;
      g->st->server->setContentLength(54u + static_cast<uint32_t>(240 * 3 * 240));
      g->st->server->send(200, "image/bmp", "");
      g->st->server->sendContent(reinterpret_cast<const char*>(g->header), 54);
    }
    sendScreenRows(px, y0, rows, w, g->st);
  };
  if (!ui::radarDisplayCapture(sink, &gate)) {
    server.send(503, "text/plain", "no frame yet");
    return;
  }
  if (st.aborted) {
    server.client().stop();
  }
}

}  // namespace

const char* headHtml() {
  /**
   * WiFiManager's own pages get the navigation from this script, because the
   * library offers no hook to add markup inside the page body.
   */
  static constexpr char kHead[] =
      "<style>"
      // WiFiManager's pages set no colours of their own, so a browser in dark
      // mode drew them black-on-black. Pin them to the same light look as the
      // settings page.
      ":root{color-scheme:light}"
      "body{background:#fff;color:#222;font-family:system-ui,-apple-system,"
      "sans-serif;padding:16px;margin:0}"
      "h1{font-size:20px;margin:0 0 6px;color:#222}"
      "h3{font-size:13px;font-weight:600;color:#777;margin:0 0 14px}"
      ".msg{border-radius:6px;background:#f7f9fb;color:#222}"
      "button,input[type='submit']{border-radius:6px}"
      "input:not([type]),input[type=text],input[type=password]{box-sizing:border-box;"
      "padding:8px;font-size:15px;border:1px solid #ccc;border-radius:6px;"
      "background:#fff;color:#222}"
      "a{color:#1a73c8}"
      ".wrap{display:block;max-width:430px;margin:0 auto;text-align:left}"
      "#nv{display:flex;gap:5px;margin:0 0 16px}"
      "#nv a{flex:1;text-align:center;padding:9px 4px;border-radius:8px;"
      "background:#f2f2f2;border:1px solid #dcdcdc;color:#444;"
      "text-decoration:none;font-size:14px;font-weight:400}"
      "#nv a.on{background:#1fa3ec;border-color:#1fa3ec;color:#fff}"
      "body.invert #nv a{background:#2b2b2b;border-color:#3a3a3a;color:#ddd}"
      "</style>"
      "<script>"
      "addEventListener('DOMContentLoaded',function(){"
      "var m=[['/','Home'],['/wifi','Wi-Fi'],['/settings','Settings'],"
      "['/info','System']],w=document.querySelector('.wrap');if(!w)return;"
      "var n=document.createElement('div');n.id='nv';"
      "m.forEach(function(e){var a=document.createElement('a');"
      "a.href=e[0];a.textContent=e[1];"
      "if(location.pathname==e[0]){a.className='on';"
      "document.title=e[1]+' - Plane Radar'}n.appendChild(a)});"
      "w.insertBefore(n,w.firstChild)});"
      "</script>";
  return kHead;
}

// The hotspot's <head>: the light look of the home-network pages without the
// navigation (there is only the Wi-Fi form to see), WiFiManager's status box
// hidden (it reports the library's own attempts, which this firmware does not
// make), and a script that adds the step header and says "Network" for SSID.
#define PLANE_RADAR_HOTSPOT_HEAD(header)                                              \
  "<style>"                                                                           \
  ":root{color-scheme:light}"                                                         \
  "body{background:#fff;color:#222;font-family:system-ui,-apple-system,sans-serif;"   \
  "padding:16px;margin:0}"                                                            \
  ".wrap{display:block;max-width:430px;margin:0 auto;text-align:left}"                \
  "button,input[type='submit']{border-radius:6px}"                                    \
  "input:not([type]),input[type=text],input[type=password]{box-sizing:border-box;"    \
  "padding:8px;font-size:16px;border:1px solid #ccc;border-radius:6px;"               \
  "background:#fff;color:#222}"                                                       \
  "a{color:#1a73c8}.msg{display:none}"                                                \
  ".st{margin:0 0 14px;padding:10px 12px;border-radius:6px;font-size:15px;"           \
  "line-height:1.4;background:#fff6e5;border:1px solid #f3d29a;color:#5a3d00}"        \
  "</style><script>"                                                                  \
  "addEventListener('DOMContentLoaded',function(){"                                   \
  "var w=document.querySelector('.wrap');if(!w)return;"                               \
  "var d=document.createElement('div');d.className='st';d.innerHTML='" header "';"    \
  "w.insertBefore(d,w.firstChild);"                                                   \
  "var l=document.querySelector(\"label[for='s']\");if(l)l.textContent='Network';"    \
  "document.title='Wi-Fi setup - Plane Radar'});"                                     \
  "</script>"

const char* hotspotHeadHtml(bool location_set) {
  static constexpr char kStepOne[] = PLANE_RADAR_HOTSPOT_HEAD(
      "<b>Step 1 of 2:</b> connect the radar to your home Wi-Fi. "
      "Only 2.4 GHz networks work.");
  static constexpr char kWifiOnly[] = PLANE_RADAR_HOTSPOT_HEAD(
      "<b>Wi-Fi setup:</b> connect the radar to your Wi-Fi. "
      "Only 2.4 GHz networks work.");
  return location_set ? kWifiOnly : kStepOne;
}

#undef PLANE_RADAR_HOTSPOT_HEAD

void registerLanRoutes(WebServer& server, const Hooks& hooks) {
  server.on(kSettingsPath, HTTP_GET, [&server]() { handleSettings(server); });
  server.on(kScreenPath, HTTP_GET, [&server]() { handleScreen(server); });
  const auto on_saved = hooks.on_settings_saved;
  server.on(kSavePath, HTTP_POST,
            [&server, on_saved]() { handleSave(server, on_saved); });
  server.on(kLocationSavePath, HTTP_POST, [&server]() { handleLocationSave(server); });
  const auto on_reset = hooks.on_factory_reset;
  server.on(kResetPath, HTTP_POST, [&server, on_reset]() { handleReset(server, on_reset); });
  server.on(kUpdateCheckPath, HTTP_POST, [&server]() { handleUpdateCheck(server); });
  for (const char* route : {"/wifi", "/0wifi", "/wifisave"}) {
    server.on(route, HTTP_ANY, [&server]() { handleLanWifi(server); });
  }
  // WiFiManager still registers its parameter page. It carries no parameters
  // any more, so send anyone who lands there to the real one.
  server.on("/param", HTTP_ANY, [&server]() { redirect(server, kSettingsPath); });
}

void registerHotspotRoutes(WebServer& server, const Hooks& hooks) {
  // Phones open "/" when they join (the captive-portal check lands there
  // too); the menu behind it would only offer pages that do not matter yet.
  server.on("/", HTTP_ANY, [&server]() {
    char to[48];
    snprintf(to, sizeof(to), "http://%s/wifi", config::kPortalIp);
    redirect(server, to);
  });
  const auto on_join = hooks.on_join;
  server.on("/wifisave", HTTP_ANY,
            [&server, on_join]() { handleHotspotSave(server, on_join); });
}

}  // namespace services::portal
