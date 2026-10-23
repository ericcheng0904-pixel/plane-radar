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
 * The new owner's pages are checked too: the guide renders whole and short,
 * the radar's name is escaped wherever it appears (in the hotspot's head it
 * sits inside a script's string), and a refused name is said on the page.
 *
 * And the pages go out in a few large chunks: each sendContent is three TCP
 * segments on the radar, and the settings page in sixty pieces took 5 s. A
 * client that stops reading gets nothing more after 10 s, and its connection
 * is shut down, so the main loop is not held for a minute.
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

#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include "services/geocode.h"
#include "services/portal_pages.h"
#include "services/radar_location.h"
#include "services/welcome.h"
#include "ui/radar_display.h"
#include "util/portal_input.h"

HostSerial Serial;
HostWiFi WiFi;

namespace {
bool g_location_set = true;
std::string g_name;
bool g_welcome_due = true;
bool g_guide_seen = false;
bool g_guide_opened = false;
WebServer* g_server = nullptr;
/** The response status when the guide was noted as opened (200: its page). */
int g_status_at_guide_note = -1;
}  // namespace

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

namespace services::welcome {
const char* name() { return g_name.c_str(); }
bool saveName(const char* value) {
  char clean[util::portal::kMaxRadarName + 1];
  if (!util::portal::radarName(value, clean, sizeof(clean))) {
    return false;
  }
  g_name = clean;
  return true;
}
bool due() { return g_welcome_due; }
void noteGuideOpened() {
  g_guide_opened = true;
  g_guide_seen = true;
  g_status_at_guide_note = g_server != nullptr ? g_server->status : -1;
}
bool guideSeen() { return g_guide_seen; }
}  // namespace services::welcome

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
  server.chunks = 0;
  server.routes[route]();
  return server.body;
}

/**
 * A page goes out in chunks of about one TCP segment (1436 bytes), not piece
 * by piece: each sendContent is three segments on the radar, and lwIP stops at
 * 8 waiting for an ACK. Sixty-odd pieces made the settings page take 5 s.
 */
void expectFewChunks(const WebServer& server, const std::string& page, const char* what) {
  const size_t most = page.size() / 1024 + 2;  // full chunks, a short one, the end
  if (server.chunks > most) {
    std::printf("FAIL  %s went out in %zu chunks (at most %zu for %zu bytes): "
                "write it with put/putf, not server.sendContent\n",
                what, server.chunks, most, page.size());
    ++g_failures;
  }
}

/** The page's text without its markup, for counting the words a reader gets. */
size_t wordCount(const std::string& html) {
  size_t words = 0;
  bool in_tag = false;
  bool in_word = false;
  for (const char c : html) {
    if (c == '<') {
      in_tag = true;
      in_word = false;
    } else if (c == '>') {
      in_tag = false;
    } else if (!in_tag) {
      const bool letter = !std::isspace(static_cast<unsigned char>(c));
      if (letter && !in_word) {
        ++words;
      }
      in_word = letter;
    }
  }
  return words;
}

/** What the hotspot's script puts on the page: the single-quoted string. */
std::string scriptMarkup(const std::string& head) {
  const std::string open = "d.innerHTML='";
  const size_t from = head.find(open);
  const size_t to = head.find("';w.insertBefore", from);
  if (from == std::string::npos || to == std::string::npos) {
    return "";
  }
  return head.substr(from + open.size(), to - from - open.size());
}

}  // namespace

int main() {
  WebServer server;
  g_server = &server;
  services::portal::registerLanRoutes(server, services::portal::Hooks{});

  for (const char* route : {"/settings", "/settingssave", "/locationsave", "/reset", "/guide",
                            "/param", "/wifi", "/wifisave"}) {
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
  // ... and in that order: a piece sent around the page buffer would come
  // out of place.
  size_t from = 0;
  for (const auto& control : controls) {
    const size_t at = page.find(control.first, from);
    if (at == std::string::npos) {
      std::printf("FAIL  %s out of order\n", control.second);
      ++g_failures;
      break;
    }
    from = at;
  }
  expectFewChunks(server, page, "settings page");

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

  // The radar's name: in the form, escaped, and refused when the screen's
  // font could not draw it. A new owner gets the guide link until it is read.
  expectContains(page, "name='rname'", "radar name field");
  expectContains(page, "href='/guide'", "Guide link");
  expectContains(page, "New here?", "guide link for a new owner");
  g_name = "Ada's <b>radar</b>";
  g_guide_seen = true;
  const std::string named = render(server, "/settings");
  expectContains(named, "value='Ada&#39;s &lt;b&gt;radar&lt;/b&gt;'", "name escaped in the form");
  expectAbsent(named, "New here?", "no guide link once it was read");
  g_guide_seen = false;
  const size_t tok_at = page.find("name='tok' value='");
  std::string token = tok_at == std::string::npos ? "" : page.substr(tok_at + 18, 8);
  server.args = {{"tok", token}, {"rname", "Zo\xc3\xab's radar"}};
  render(server, "/settingssave");
  expectContains(server.headers["Location"], "name=bad", "accented name refused");
  expectContains(g_name, "Ada's", "refused name leaves the old one");
  // No pattern= on the field: a browser would hold back the whole form for an
  // iPhone's curly apostrophe, which the save makes plain instead.
  expectAbsent(page, "pattern=", "no client pattern holding back the form");
  server.args = {{"tok", token}, {"rname", "Ada\xe2\x80\x99s radar"}};
  render(server, "/settingssave");
  expectAbsent(server.headers["Location"], "name=bad", "curly apostrophe accepted");
  expectContains(g_name + "|", "Ada's radar|", "curly apostrophe made plain");
  server.args = {{"tok", token}, {"rname", "  Kitchen radar  "}};
  render(server, "/settingssave");
  expectContains(g_name + "|", "Kitchen radar|", "name trimmed and saved");
  server.args = {{"name", "bad"}};
  expectContains(render(server, "/settings"), "name was not changed", "refusal said");
  server.args.clear();

  // The guide: whole, greeting by name, and short enough to be read.
  g_guide_opened = false;
  server.status = 0;
  const std::string guide = render(server, "/guide");
  // Only a page that started counts: a "busy" 503 must not end the welcome.
  if (g_status_at_guide_note != 200) {
    std::printf("FAIL  the guide was noted as opened before its page started\n");
    ++g_failures;
  }
  expectFewChunks(server, guide, "guide");
  expectContains(guide, "<h2>Welcome to Kitchen radar</h2>", "guide greets by name");
  expectContains(guide, "5, 10, 15 or 25 km (3, 6, 9 or 16 in miles)", "guide range steps");
  expectContains(guide, "violet", "guide explains the height colours");
  expectContains(guide, "B738", "guide names the type shown instead of a route");
  expectContains(guide, "brackets", "guide says how the card's plane is marked");
  expectContains(guide, "your internet", "guide names the internet as a cause");
  expectContains(guide, "Some skies are quiet", "guide covers an empty sky");
  expectContains(guide, "192.168.1.23/settings", "guide gives the address");
  expectContains(guide, "</html>", "guide page closed");
  if (!g_guide_opened) {
    std::printf("FAIL  opening the guide was not noted\n");
    ++g_failures;
  }
  const size_t start = guide.find("<h2>");
  const size_t words = wordCount(guide.substr(start == std::string::npos ? 0 : start));
  if (words < 250 || words > 400) {
    std::printf("FAIL  guide has %zu words; aim for 250-400\n", words);
    ++g_failures;
  }
  g_name = "";
  expectContains(render(server, "/guide"), "Welcome to your Plane Radar", "guide unnamed");

  // Setup step 2 greets a new owner first, and nobody else.
  g_location_set = false;
  expectContains(render(server, "/settings"), "Welcome to <b>your Plane Radar</b>!",
                 "step 2 intro");
  g_welcome_due = false;
  expectAbsent(render(server, "/settings"), "Welcome to", "no intro once welcomed");
  g_welcome_due = true;
  g_location_set = true;

  // After step 2 the phone says what the round screen does next: the welcome
  // while it is due, the planes otherwise.
  server.args = {{"loc", "saved"}};
  const std::string saved = render(server, "/settings");
  expectContains(saved, "says welcome first", "saved notice names the welcome");
  expectAbsent(saved, "shows planes near it now", "saved notice does not promise planes");
  g_welcome_due = false;
  expectContains(render(server, "/settings"), "shows planes near it now", "saved notice later");
  g_welcome_due = true;
  server.args.clear();

  // A name that ends in its own punctuation is not followed by another "!".
  g_name = "Happy birthday!";
  g_location_set = false;
  const std::string bang = render(server, "/settings");
  expectContains(bang, "<b>Happy birthday!</b> It shows", "no doubled exclamation mark");
  g_location_set = true;
  g_name = "";

  // The reset keeps the name with the location.
  server.args = {{"tok", token}};
  const std::string reset = render(server, "/reset");
  expectContains(reset, "Keep the location and the radar&#39;s name", "reset keeps the name");
  expectContains(reset, "Do this last", "reset says the welcome shows once");
  server.args.clear();

  // The hotspot's head: the name sits in a script's single-quoted string, so
  // nothing in it may end the string, the line or the script.
  const std::string hostile = services::portal::hotspotHeadHtml(false, true, "A'\\x\"</script>&");
  const std::string markup = scriptMarkup(hostile);
  expectContains(markup, "Welcome to <b>A&#39;&#92;x&quot;&lt;/script&gt;&amp;</b>!",
                 "hotspot name escaped");
  expectContains(markup, "Step 1 of 2", "hotspot step 1");
  expectContains(markup, "two minutes", "hotspot two steps");
  if (markup.empty() || markup.find_first_of("'\\\n") != std::string::npos) {
    std::printf("FAIL  hotspot script string broken: %s\n", markup.c_str());
    ++g_failures;
  }
  const std::string worst = services::portal::hotspotHeadHtml(
      true, true, std::string(util::portal::kMaxRadarName, '"').c_str());
  expectContains(worst, "Wi-Fi setup:", "hotspot Wi-Fi only");
  expectContains(worst, "a minute", "hotspot one step");
  expectContains(worst, "</script>", "hotspot head whole");
  if (worst.size() > 1536) {
    std::printf("FAIL  hotspot head is %zu bytes; WiFiManager copies it into every page\n",
                worst.size());
    ++g_failures;
  }
  const std::string quiet = services::portal::hotspotHeadHtml(true, false, "Ada");
  expectAbsent(quiet, "Welcome", "no greeting for a known owner");
  expectContains(quiet, "</script>", "hotspot head whole without greeting");
  expectContains(services::portal::hotspotHeadHtml(false, false, ""), "Step 1 of 2",
                 "hotspot step 1 without greeting");
  // Without a greeting the head is a constant in flash, not the heap buffer.
  if (services::portal::hotspotHeadHtml(true, false, "") ==
      services::portal::hotspotHeadHtml(true, true, "")) {
    std::printf("FAIL  hotspot head without a greeting came from the greeting buffer\n");
    ++g_failures;
  }
  const std::string greeted = services::portal::hotspotHeadHtml(true, true, "Happy birthday!");
  expectContains(greeted, "<b>Happy birthday!</b> It shows", "hotspot: no doubled '!'");
  expectContains(scriptMarkup(greeted), "<div class=st><b>Wi-Fi setup:</b>",
                 "hotspot greeting before the step");

  // The home-network Wi-Fi page explains instead of taking credentials.
  const std::string wifi = render(server, "/wifi");
  expectFewChunks(server, wifi, "Wi-Fi page");
  expectContains(wifi, "10 seconds", "how to change Wi-Fi");
  expectAbsent(wifi, "name='p'", "no password field");

  // A client that stops reading: on the radar every write then waits 10 s.
  // Past the 10 s budget nothing more is written, not even the chunked end,
  // and the connection itself is shut down (client() is only a copy of it).
  int pair[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
    std::printf("FAIL  no socketpair for the slow-client check\n");
    return 1;
  }
  server.connection.socket = pair[0];
  server.ms_per_chunk = 4000;
  const unsigned long clock = g_host_millis;
  const std::string slow = render(server, "/settings");
  server.ms_per_chunk = 0;
  g_host_millis = clock;
  server.connection.socket = -1;
  // Chunks begin at 0, 4 and 8 s; the one due at 12 s is past the budget.
  if (server.chunks != 3) {
    std::printf("FAIL  slow client got %zu chunks; only the 3 begun within 10 s may go\n",
                server.chunks);
    ++g_failures;
  }
  expectAbsent(slow, "</html>", "slow client's page cut short");
  char byte = 0;
  if (recv(pair[1], &byte, 1, MSG_DONTWAIT) != 0) {
    std::printf("FAIL  slow client's connection was not shut down\n");
    ++g_failures;
  }
  close(pair[0]);
  close(pair[1]);
  const std::string whole = render(server, "/settings");
  expectContains(whole, "</html>", "page after a cut one whole");
  expectFewChunks(server, whole, "page after a cut one");

  if (g_failures != 0) {
    std::printf("\n%d check(s) failed; page was %zu bytes\n", g_failures,
                page.size());
    return 1;
  }
  std::printf("portal settings page: %zu bytes, %zu controls, all present\n",
              page.size(), controls.size());
  std::printf("guide: %zu bytes, %zu words; hotspot head: %zu bytes at most\n", guide.size(),
              words, worst.size());
  return 0;
}
