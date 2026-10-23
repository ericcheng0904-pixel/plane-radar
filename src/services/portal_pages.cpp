#include "services/portal_pages.h"

#include <Arduino.h>
#include <WiFi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>

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
#include "services/welcome.h"
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
constexpr char kGuidePath[] = "/guide";

/** What the pages call the radar when its owner has not named it. */
constexpr char kUnnamed[] = "your Plane Radar";

/**
 * The page on its way out: text is gathered here and sent one TCP segment's
 * worth at a time, as one chunk of the chunked response.
 *
 * Speed. WebServer::sendContent() makes three socket writes (chunk size,
 * text, CRLF), and with TCP_NODELAY on (WebServer::begin() and
 * WiFiServer::available() set it on every connection; WiFiManager's server
 * inherits it) each leaves as a TCP segment of its own. lwIP stops a writer at
 * 8 segments waiting for an ACK, so the settings page, written in some sixty
 * pieces, went out as ~190 segments and took about 5 s. Gathered, it is six
 * full-size chunks, which a phone acknowledges at once. Turning Nagle on
 * instead would leave the ~190 send calls and lean on lwIP's internals.
 *
 * Stack. Everything is formatted (putf) and escaped (putEscaped) straight into
 * this buffer, never into char arrays on the stack. The handlers run on
 * loopTask, which has 8 KB; in v2.0.13 the compiler inlined the settings
 * page's sections into one 3.5 KB frame, the About section's 2.5 KB went under
 * it and printf under that, and a request overflowed the stack.
 *
 * One buffer for all pages: every handler runs on loopTask, one at a time.
 * WiFiManager::process() (from wifiLoop() and openConfigPortal()) calls
 * WebServer::handleClient(), which runs one handler and returns. Nothing a
 * handler calls serves another request: the Hooks only note what the loop is
 * to do, WiFiManager's callbacks (setAPCallback, setWebServerCallback) only
 * start mDNS and register routes, and its own pages build their own String
 * and never come here. Borrowed from the heap for the length of a page, like
 * /screen.bmp's rows, so between requests it costs nothing: the TLS session
 * needs the heap's largest block.
 */
#ifdef CONFIG_LWIP_TCP_MSS
constexpr size_t kPageChunk = CONFIG_LWIP_TCP_MSS;
#else
constexpr size_t kPageChunk = 1436;  // the radar's CONFIG_LWIP_TCP_MSS
#endif
/**
 * A page still being sent after this is cut off. Each write to a client that
 * stopped reading can hold the main loop for 10 s (WiFiClient::write: ten
 * 1 s waits), and a chunk is three writes. Past the budget nothing more of
 * the page is written, and the connection is shut down so that WebServer's
 * own writes after the handler (the chunked response's end) fail at once:
 * the main loop is free well inside the minute after which the health check
 * restarts a stuck loop.
 */
constexpr unsigned long kPageBudgetMs = 10000;

struct PageOut {
  /** kPageChunk + 1 bytes (vsnprintf's terminator); nullptr between pages. */
  char* buf;
  size_t len;
  unsigned long started_ms;
  /** Over kPageBudgetMs: the rest of the page is dropped, not written. */
  bool cut;
};
PageOut s_page = {nullptr, 0, 0, false};

/**
 * Ends the connection under the response. WebServer::client() returns a copy,
 * and WiFiClient::stop() on a copy only lets go of the copy: the server's own
 * connection stays open and every later write still waits. shutdown() acts on
 * the socket itself, so the next write fails, and WiFiClient::write() stops
 * the server's connection, which makes every write after it return at once.
 */
void dropClient(WebServer& server) {
  const int fd = server.client().fd();
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
  }
}

/** What is gathered goes out as one chunk; nothing once the page is cut. */
void flushPage(WebServer& server) {
  if (s_page.len == 0) {
    return;
  }
  if (!s_page.cut && millis() - s_page.started_ms > kPageBudgetMs) {
    s_page.cut = true;
    dropClient(server);
  }
  if (!s_page.cut) {
    server.sendContent(s_page.buf, s_page.len);
  }
  s_page.len = 0;
}

/** `n` bytes of `text` onto the page. Nothing when no page is under way. */
void put(WebServer& server, const char* text, size_t n) {
  if (s_page.buf == nullptr) {
    return;
  }
  while (n > 0) {
    if (s_page.len == kPageChunk) {
      flushPage(server);
    }
    const size_t k = std::min(n, kPageChunk - s_page.len);
    memcpy(s_page.buf + s_page.len, text, k);
    s_page.len += k;
    text += k;
    n -= k;
  }
}

void put(WebServer& server, const char* text) { put(server, text, strlen(text)); }

/** printf onto the page. Each piece must stay under kPageChunk (all are under 1 KB). */
__attribute__((format(printf, 2, 3))) void putf(WebServer& server, const char* format, ...) {
  if (s_page.buf == nullptr) {
    return;
  }
  for (int pass = 0; pass < 2; ++pass) {
    const size_t room = kPageChunk - s_page.len;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(s_page.buf + s_page.len, room + 1, format, args);
    va_end(args);
    if (n < 0) {
      return;
    }
    if (static_cast<size_t>(n) <= room || s_page.len == 0) {
      s_page.len += std::min(static_cast<size_t>(n), room);
      return;
    }
    // It did not fit after what is there: send that, and format it again
    // at the start of the empty buffer.
    flushPage(server);
  }
}

/** `text` with & < > " ' as entities, as util::setup::htmlEscape writes it. */
void putEscaped(WebServer& server, const char* text) {
  if (text == nullptr) {
    return;
  }
  const char* plain = text;
  for (const char* p = text; *p != '\0'; ++p) {
    const char* entity = util::setup::htmlEntity(*p);
    if (entity != nullptr) {
      put(server, plain, static_cast<size_t>(p - plain));
      put(server, entity);
      plain = p + 1;
    }
  }
  put(server, plain);
}

/**
 * On a section sender that has a buffer of its own (an OTA status, a feed
 * line): kept a function of its own, so its buffer is on the stack only while
 * it runs. Inlined into the page's handler, GCC gives each inlined section's
 * locals a slot of their own for the whole page (the exception paths of the
 * String locals keep them all live), and the frames add up — that was the
 * 3.5 KB handleSettings of v2.0.13.
 */
#define PORTAL_SECTION __attribute__((noinline))

/** The radar's name, escaped; kUnnamed when it has none. */
void putName(WebServer& server) {
  const char* name = welcome::name();
  if (name != nullptr && name[0] != '\0') {
    putEscaped(server, name);
  } else {
    put(server, kUnnamed);
  }
}

/**
 * Navigation shown on every portal page. WiFiManager renders each page as a
 * dead end with at most a Back button, which is how the settings ended up
 * hidden; these five links are the whole map of the interface.
 */
void sendNav(WebServer& server, const char* active) {
  static const char* const kLinks[][2] = {{"/", "Home"},
                                          {"/wifi", "Wi-Fi"},
                                          {"/settings", "Settings"},
                                          {"/guide", "Guide"},
                                          {"/info", "System"}};
  put(server, "<div id='nv'>");
  for (const auto& link : kLinks) {
    putf(server, "<a%s href='%s'>%s</a>", strcmp(link[0], active) == 0 ? " class='on'" : "",
         link[0], link[1]);
  }
  put(server, "</div>");
}

/** Styles for our own pages: same palette as WiFiManager, nothing loaded. */
constexpr char kStyleHtml[] =
    "<style>"
    ":root{color-scheme:light}"
    "body{background:#fff;color:#222;font-family:system-ui,-apple-system,"
    "sans-serif;margin:0;padding:16px}"
    ".wrap{max-width:430px;margin:0 auto;text-align:left}"
    "h1{font-size:20px;margin:0 0 14px}"
    "h2{font-size:18px;margin:4px 0 10px}"
    "p,li{font-size:15px;line-height:1.45}"
    "ul{padding-left:20px;margin:8px 0}"
    "li{margin:0 0 6px}"
    // Five links share a phone's width: each takes its word's width plus a
    // share of what is left (all fit at 320 px), and on anything narrower a
    // word is cut rather than pushing the page sideways.
    "#nv{display:flex;gap:4px;margin:0 0 16px}"
    "#nv a{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;"
    "white-space:nowrap;text-align:center;padding:9px 4px;border-radius:8px;"
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
    "a.nh{display:block;margin:0 0 14px;padding:10px 12px;border-radius:6px;font-size:15px;"
    "background:#e8f3fd;border:1px solid #b5d9f5;color:#0b4f8a;text-decoration:none}"
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
  putf(server, "<form method='POST' action='%s'><input type='hidden' name='tok' value='%s'>",
       action, formToken());
}

/**
 * Status line, chunked document start, styles and heading. `refresh_s` > 0
 * reloads the page by itself (a lookup under way); `nav` is the active link,
 * or nullptr on the hotspot, where there is nowhere else to go. Every page
 * that starts here ends with sendPageEnd(), which gives the buffer back.
 * False when the page could not start (503 sent): the handler's writes are
 * dropped, and nothing should count as shown.
 */
bool sendPageStart(WebServer& server, const char* title, const char* nav, int refresh_s = 0) {
  if (s_page.buf == nullptr) {
    s_page.buf = static_cast<char*>(malloc(kPageChunk + 1));
  }
  if (s_page.buf == nullptr) {
    // Not even 1.4 KB free: a plain answer, rather than a page that stops
    // short. Everything the handler goes on to write is dropped.
    server.send(503, "text/plain", "busy: try again in a moment");
    return false;
  }
  s_page.len = 0;
  s_page.started_ms = millis();
  s_page.cut = false;
  // Chunked output: the page never exists in RAM as a whole, so its length is
  // independent of how fragmented the heap is by the time it is requested.
  server.sendHeader("Cache-Control", "no-store");
  // No other page may frame these: "Reset for a new owner" is one click away.
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("Content-Security-Policy", "frame-ancestors 'none'");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  put(server,
      "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (refresh_s > 0) {
    putf(server, "<meta http-equiv='refresh' content='%d'>", refresh_s);
  }
  putf(server, "<title>%s - Plane Radar</title>", title);
  put(server, kStyleHtml, sizeof(kStyleHtml) - 1);
  put(server, "</head><body><div class='wrap'><h1>Plane Radar</h1>");
  if (nav != nullptr) {
    sendNav(server, nav);
  }
  return true;
}

void sendPageEnd(WebServer& server) {
  if (s_page.buf == nullptr) {
    return;  // sendPageStart() answered 503: that was the whole reply
  }
  put(server, "</div></body></html>");
  flushPage(server);
  const bool cut = s_page.cut;
  free(s_page.buf);
  s_page = {nullptr, 0, 0, false};
  if (!cut) {
    server.sendContent("", 0);  // the chunked response's end
  }
}

/**
 * What follows the name in "Welcome to <name>!": nothing when the name ends in
 * its own . ! or ? ("Happy birthday!"), which would otherwise read "!!".
 */
const char* afterName(const char* name) {
  const size_t n = name != nullptr ? strlen(name) : 0;
  return n > 0 && strchr(".!?", name[n - 1]) != nullptr ? "" : "!";
}

/**
 * The one line that tells a new owner what this is, before the step they are
 * on: "Welcome to <name>! It shows the planes flying near you, live." and
 * `then`. Only while the welcome is still to come: an owner who knows the
 * radar is not greeted again.
 */
void sendWelcomeIntro(WebServer& server, const char* then) {
  if (!welcome::due()) {
    return;
  }
  put(server, "<p>Welcome to <b>");
  putName(server);
  putf(server, "</b>%s It shows the planes flying near you, live. %s</p>",
       afterName(welcome::name()), then);
}

void sendCheckbox(WebServer& server, const char* name, const char* label,
                  bool checked) {
  putf(server,
       "<label class='cb'><input type='checkbox' name='%s' value='T'%s>"
       "<span>%s</span></label>",
       name, checked ? " checked" : "", label);
}

/**
 * A drop-down over a fixed set of numbers. The stored value is validated again
 * on save, so a hand-crafted POST gains nothing by picking another option.
 */
void sendChoiceSelect(WebServer& server, const char* name, const char* label,
                      const uint8_t* choices, size_t count, uint8_t current,
                      const char* const* option_labels) {
  putf(server, "<label for='%s'>%s</label><select id='%s' name='%s'>", name, label, name, name);
  for (size_t i = 0; i < count; ++i) {
    putf(server, "<option value='%u'%s>%s</option>", static_cast<unsigned>(choices[i]),
         choices[i] == current ? " selected" : "", option_labels[i]);
  }
  put(server, "</select>");
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
PORTAL_SECTION void sendLocationNotice(WebServer& server, const String& code) {
  // A new owner's radar says welcome before it shows planes (main.cpp,
  // showWelcome): say so, or the phone and the screen disagree.
  const char* next = welcome::due()
                         ? "The radar says welcome first: scan its code for a short guide, "
                           "or tap its button. Then the planes appear."
                         : "The radar shows planes near it now.";
  if (code == "saved") {
    put(server, "<div class='ok'>Location saved: <b>");
    putEscaped(server, location::label());
    putf(server, "</b>. %s</div>", next);
  } else if (code == "looking") {
    char asked[util::geocode::kMaxQuery + 1];
    geocode::asked(asked, sizeof(asked));
    switch (geocode::state()) {
      case geocode::State::kWaiting:
        put(server, "<div class='st'>Looking up &ldquo;");
        putEscaped(server, asked);
        put(server, "&rdquo;&hellip;</div>");
        break;
      case geocode::State::kFound:
        put(server, "<div class='ok'>Found <b>");
        putEscaped(server, location::label());
        putf(server,
             "</b> (%.4f, %.4f). %s Not the right place? "
             "Add the state (Evanston IL) or paste coordinates.</div>",
             location::lat(), location::lon(), next);
        break;
      case geocode::State::kNotFound:
        put(server, "<div class='er'>Could not find &ldquo;");
        putEscaped(server, asked);
        put(server,
            "&rdquo;. Add the state "
            "(Evanston IL; for a neighbourhood the state, not the city: "
            "Logan Square IL) or the country, or paste coordinates.</div>");
        break;
      case geocode::State::kFailed:
        put(server,
            "<div class='er'>The town search did not answer. Check that the "
            "radar's Wi-Fi reaches the internet, or paste coordinates "
            "instead.</div>");
        break;
      case geocode::State::kIdle:
      default:
        break;
    }
  } else if (code == "badpair") {
    put(server,
        "<div class='er'>Could not read those coordinates. Paste two numbers, "
        "latitude first (41.88, -87.63), or a Google Maps link.</div>");
  } else if (code == "badll") {
    put(server,
        "<div class='er'>Fill in both latitude and longitude as decimal numbers, "
        "negative for south and west.</div>");
  } else if (code == "badtown") {
    put(server, "<div class='er'>Type a town name, or a five-digit ZIP code.</div>");
  } else if (code == "busy") {
    put(server,
        "<div class='er'>Still looking up the last one; try again in a few "
        "seconds.</div>");
  } else if (code == "empty") {
    put(server,
        "<div class='er'>Nothing entered: type a town or ZIP code, or paste "
        "coordinates.</div>");
  } else if (code == "same") {
    put(server, "<div class='ok'>Location unchanged.</div>");
  }
}

/**
 * The location form. Its own form and its own endpoint: when the coordinates
 * sat in the one big form, saving any display option saved them too — and
 * with nothing stored they were pre-filled with the default, so the radar
 * quietly moved to Amsterdam.
 */
void sendLocationSection(WebServer& server) {
  const bool set = location::isSet();
  put(server, "<div class='sc'>Location</div>");
  if (set) {
    // Set already: say where, and keep the form folded away, so the page
    // does not read as a request to type coordinates.
    put(server, "<div class='hn'>Showing planes near <b>");
    putEscaped(server, location::label());
    put(server, "</b>.</div><details><summary>Change the location</summary>");
  }
  sendFormStart(server, kLocationSavePath);
  put(server,
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
  put(server,
      "<label for='lat'>Latitude</label>"
      "<input id='lat' name='lat' type='number' step='0.000001' "
      "min='-90' max='90' value='");
  if (set) {
    putf(server, "%.6f", location::lat());
  }
  put(server,
      "'><label for='lon'>Longitude</label>"
      "<input id='lon' name='lon' type='number' step='0.000001' "
      "min='-180' max='180' value='");
  if (set) {
    putf(server, "%.6f", location::lon());
  }
  put(server,
      "'></details><button type='submit'>Save location</button></form>"
      "<div class='hn'>The lake and shoreline layer covers the Chicago area only; "
      "elsewhere the radar simply shows no water.</div>");
  if (set) {
    put(server, "</details>");
  }
}

void sendDisplaySections(WebServer& server) {
  sendFormStart(server, kSavePath);
  put(server, "<div class='sc'>Display</div>");
  // No pattern= here: a name the save refuses (util::portal::radarName,
  // which makes a phone's curly quotes plain first) is said on the page,
  // and the rest of this form is still saved.
  putf(server,
       "<label for='rname'>Name of this radar (optional)</label>"
       "<input id='rname' name='rname' type='text' maxlength='%u' value='",
       static_cast<unsigned>(util::portal::kMaxRadarName));
  putEscaped(server, welcome::name());
  putf(server,
       "' placeholder='Ada&#39;s radar' autocomplete='off'>"
       "<div class='hn'>Read as &ldquo;Welcome to Ada&#39;s radar&rdquo; on the "
       "setup page, the welcome screen and the guide. Empty: &ldquo;Welcome to "
       "%s&rdquo;. Plain letters, no accents or emoji.</div>",
       kUnnamed);
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

  put(server,
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

  put(server,
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

  put(server,
      "<button type='submit'>Save</button></form>"
      "<div class='hn'>Saved settings survive a restart. Hold the button on the "
      "radar for ten seconds to clear the saved Wi-Fi; location and these "
      "settings are kept.</div>");
}

PORTAL_SECTION void sendStatusLine(WebServer& server) {
  // Status in words, for whoever is looking after the radar.
  const unsigned long last = services::adsb::lastUpdateMs();
  const unsigned long up_min = millis() / 60000;
  put(server, "<div class='hn'>Status: ");
  if (last == 0) {
    putf(server, "no flight data yet (trying %s)", services::adsb::feedName());
  } else {
    // The feed being asked can differ from the one the data came from
    // around a failover; name both then.
    const char* from = services::adsb::dataFeedName();
    const char* next = services::adsb::feedName();
    putf(server, "flight data from %s %lu s ago%s", from, (millis() - last) / 1000,
         services::adsb::dataVerified() ? "" : " (certificate not checked)");
    if (strcmp(from, next) != 0) {
      putf(server, ", now trying %s", next);
    }
  }
  putf(server, " &middot; running %lu h %lu min &middot; last restart: %s</div>", up_min / 60,
       up_min % 60, services::health::lastRestartText());
  char feeds[160];
  services::adsb::feedStatusText(feeds, sizeof(feeds));
  if (feeds[0] != '\0') {
    // Feed names and numbers only: nothing in it needs escaping.
    putf(server, "<div class='hn'>Feeds: %s</div>", feeds);
  }
}

PORTAL_SECTION void sendAboutSection(WebServer& server);

void handleSettings(WebServer& server) {
  // A lookup that finished since the last loop pass shows as applied.
  geocode::applyResult();
  const String loc = server.arg("loc");
  const bool looking = loc == "looking" && geocode::state() == geocode::State::kWaiting;
  sendPageStart(server, "Settings", kSettingsPath, looking ? 2 : 0);

  if (server.hasArg("saved")) {
    put(server, "<div class='ok'>Settings saved.</div>");
  }
  if (server.arg("name") == "bad") {
    put(server,
        "<div class='er'>The radar's name was not changed. Use plain letters, numbers "
        "and punctuation (no accents or emoji), up to 32 of them.</div>");
  }
  sendLocationNotice(server, loc);

  if (!location::isSet()) {
    // Setup step 2: the location is the only thing asked for, so it is the
    // only thing on the page.
    sendWelcomeIntro(server, "One more step, then it says welcome and offers a short guide.");
    put(server,
        "<div class='st'><b>Step 2 of 2:</b> tell the radar where it is. Type "
        "your town or ZIP code, or paste coordinates from a map app, then tap "
        "<b>Save location</b>.</div>");
    sendLocationSection(server);
    put(server,
        "<div class='hn'>The other settings appear here once the radar knows "
        "where it is.</div>");
    sendPageEnd(server);
    return;
  }

  if (!welcome::guideSeen()) {
    // Until it has been read once: whoever opens this page first may well
    // have been handed the radar without a word about it.
    put(server,
        "<a class='nh' href='/guide'><b>New here?</b> A short guide &rsaquo;</a>");
  }
  sendStatusLine(server);
  sendLocationSection(server);
  sendDisplaySections(server);

  sendAboutSection(server);

  put(server,
      "<div class='sc'>Giving the radar away</div>"
      "<div class='hn'>Erases the location, the name, the display settings and the "
      "Wi-Fi, and restarts at setup step 1 for the next person, who is then "
      "welcomed with a short guide.</div>");
  sendFormStart(server, kResetPath);
  put(server,
      "<button class='d' type='submit'>Reset for a new owner&hellip;</button></form>");
  sendPageEnd(server);
}

/**
 * Version, updates, where the code lives, and whose data the radar shows
 * (adsb.fi's terms ask for a link). URLs are spelled out as well as linked:
 * a phone on the radar's hotspot has no internet to follow them.
 */
PORTAL_SECTION void sendAboutSection(WebServer& server) {
  putf(server,
       "<div class='sc'>About</div>"
       "<div class='hn'>Plane Radar %s &middot; <a href='%s'>%s</a><br>Updates: ",
       config::kFirmwareVersion, config::kRepoUrl, config::kRepoUrl + 8);
  char status[160];
  services::ota::statusText(status, sizeof(status));
  putEscaped(server, status);
  put(server,
      ". Signed updates install by themselves, about once a day; "
      "Wi-Fi, location and settings are kept.</div>");
  sendFormStart(server, kUpdateCheckPath);
  put(server,
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
  put(server,
      "<div class='ok'>Looking for an update in a moment. This page goes back to the "
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
  // Free text, so a refusal is said on the page: an accent is not an obvious
  // reason. A form without the field (an older page) leaves the name alone.
  const bool name_ok = !server.hasArg("rname") || welcome::saveName(server.arg("rname").c_str());

  if (on_saved != nullptr) {
    on_saved();
  }

  char location[64];
  snprintf(location, sizeof(location), "%s?saved=1%s", kSettingsPath,
           name_ok ? "" : "&name=bad");
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
    put(server,
        "<div class='sc'>Reset for a new owner</div>"
        "<p>This erases where the radar is, its name, its display settings and its "
        "Wi-Fi. It restarts into setup, ready for the next person to set it up with "
        "their phone. Once it is set up, it welcomes them and offers a short "
        "guide.</p>"
        "<p>Keeping the location and the name leaves them only the Wi-Fi to do.</p>"
        // The welcome is used up by whoever sees it first (welcome::markShown).
        "<p><b>Do this last.</b> The welcome shows once, to whoever sets the radar "
        "up next. If you try it out yourself afterwards, reset it again before you "
        "give it away.</p>");
    sendFormStart(server, kResetPath);
    put(server,
        "<input type='hidden' name='confirm' value='yes'>"
        "<label class='cb'><input type='checkbox' name='keep' value='1'>"
        "Keep the location and the radar&#39;s name (it is staying in this "
        "area)</label>"
        "<button class='d' type='submit'>Yes, erase everything</button></form>"
        "<a class='bt' href='/settings'>Cancel</a>");
    sendPageEnd(server);
    return;
  }
  const bool keep = server.arg("keep") == "1";
  sendPageStart(server, "Reset", nullptr);
  // What the round screen will say: with the location kept, Wi-Fi is the
  // only step left.
  put(server, keep && location::isSet()
                  ? "<div class='ok'>Erasing. The radar restarts in a few seconds "
                    "and shows <b>Wi-Fi setup</b>. You can close this page.</div>"
                  : "<div class='ok'>Erasing. The radar restarts in a few seconds "
                    "and shows <b>Step 1 of 2</b>. You can close this page.</div>");
  sendPageEnd(server);
  if (on_reset != nullptr) {
    on_reset(keep);
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
  put(server, "<div class='sc'>Wi-Fi</div><p>Connected to <b>");
  putEscaped(server, WiFi.SSID().c_str());
  const int rssi = WiFi.RSSI();
  const char* strength = rssi >= -60 ? "strong" : rssi >= -72 ? "fair" : "weak";
  putf(server, "</b>; the signal is %s (%d dBm).</p>", strength, rssi);
  put(server,
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
 * The guide: what the radar is and how to read it, for someone who was handed
 * one. Checked against the code it describes (the range steps and their miles
 * in ui/radar_range.h, the button's timings in config.h and its countdown in
 * ui/status_screens.cpp, the height colours in ui/aircraft_glyph.cpp, the tag,
 * card, brackets and "No flight data" in ui/radar_display.cpp and
 * ui/info_card.cpp); change them together. Kept in flash and sent as it is,
 * piece by piece, so it costs no RAM to serve.
 */
constexpr char kGuideIntro[] =
    "<p>It shows the planes flying near you, live, from the radio signals planes "
    "send out. Nothing to sign up for or log into.</p>"
    "<div class='sc'>Reading the screen</div><ul>"
    "<li><b>You are in the middle</b>, with north up (N, E, S and W at the "
    "edge).</li>"
    "<li><b>The rings</b> show distance: the number on the right is the third ring "
    "from the middle; the outer ring is a third further.</li>"
    "<li><b>Each plane</b> points the way it is flying; its shape shows the kind, "
    "from small planes and helicopters to jumbo jets. A line points ahead; a faint "
    "tail shows where it has been.</li>"
    "<li><b>Colour shows height</b> (standard theme): orange near the ground, "
    "through yellow, green and blue to violet at cruising height.</li>"
    "<li><b>Next to it:</b> the flight number, where it flies from and to (or its "
    "type, such as B738) and its height in feet.</li>"
    "<li><b>The card</b> at the bottom (or top) spells out the plane marked with "
    "brackets (airline, aircraft, height, speed), taking the nearest few in "
    "turn.</li>"
    "<li><b>A dot on the edge</b> is a plane beyond the outer ring.</li>"
    "<li><b>A ring pulsing out</b> from the middle: something special just "
    "appeared, an emergency (red), a military plane (amber) or a rare one such as "
    "a 747 (cyan).</li></ul>";
constexpr char kGuideButton[] =
    "<div class='sc'>The button (marked BOOT)</div><ul>"
    "<li><b>Tap:</b> change the range: 5, 10, 15 or 25 km (3, 6, 9 or 16 in "
    "miles).</li>"
    "<li><b>Hold until a countdown appears, then let go:</b> an info screen with the "
    "settings&#39; address; a tap closes it.</li>"
    "<li><b>Hold until the countdown ends</b> (10 seconds): it forgets its Wi-Fi "
    "and asks again. For a new router or password only.</li></ul>"
    "<div class='sc'>Good to know</div><ul>"
    "<li><b>At night</b> the screen dims, from dusk to dawn.</li>"
    "<li><b>No planes?</b> Some skies are quiet: tap for a wider range.</li>"
    "<li><b>&ldquo;No flight data&rdquo;:</b> none for a minute, usually because the "
    "flight service or your internet is down. It keeps trying. &ldquo;next update in "
    "N min&rdquo; means it is on a slower backup.</li>"
    "<li><b>&ldquo;Wi-Fi lost&rdquo;:</b> it keeps trying. If it lasts, check the "
    "router, then unplug the radar and plug it back in.</li>"
    "<li><b>Updates</b> install by themselves, keeping your settings.</li>"
    "<li><b>It feels warm.</b> That is normal.</li></ul>"
    "<div class='sc'>Settings</div>";

// A plain GET counts as reading the guide (the welcome's QR code is a GET), so
// any web page could mark it read by loading this address, say as an image:
// the welcome would end early and Settings would stop pointing here. Accepted:
// that is all it does, nothing is changed, the Guide link stays in the menu,
// and the Sec-Fetch-* headers that could tell a page from a scan are only sent
// to HTTPS addresses, never to this plain-HTTP one.
void handleGuide(WebServer& server) {
  if (!sendPageStart(server, "Guide", kGuidePath)) {
    return;  // "busy": the guide was not shown, so the welcome stays
  }
  welcome::noteGuideOpened();
  put(server, "<h2>Welcome to ");
  putName(server);
  put(server, "</h2>");
  put(server, kGuideIntro, sizeof(kGuideIntro) - 1);
  put(server, kGuideButton, sizeof(kGuideButton) - 1);
  // The address too: not every phone finds plane-radar.local.
  putf(server,
       "<p>Theme, text size, miles, night dimming, the location and the name are "
       "on the <a href='%s'>settings page</a>. On the same Wi-Fi, open "
       "<b>%s/settings</b> (or <b>%s/settings</b>).</p>",
       kSettingsPath, config::kPortalHostUrl, WiFi.localIP().toString().c_str());
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
  if (!util::setup::plausibleSsid(ssid.c_str()) ||
      !util::setup::plausiblePassword(pass.c_str())) {
    sendPageStart(server, "Wi-Fi setup", nullptr);
    put(server,
        ssid.length() == 0
            ? "<div class='er'>Pick your home network from the list first.</div>"
            : "<div class='er'>That password cannot be right: Wi-Fi passwords have "
              "8 to 63 characters.</div>");
    put(server, "<a class='bt' href='/wifi'>Back</a>");
    sendPageEnd(server);
    return;
  }
  if (on_join != nullptr) {
    on_join(ssid.c_str(), pass.c_str());
  }
  const bool two_steps = !location::isSet();
  sendPageStart(server, "Wi-Fi setup", nullptr);
  putf(server, "<div class='st'><b>%s</b> the radar is joining <b>",
       two_steps ? "Step 1 of 2:" : "Wi-Fi setup:");
  putEscaped(server, ssid.c_str());
  put(server, "</b>.</div><p>Watch the radar's screen.</p>");
  if (two_steps) {
    put(server,
        "<p>When it says <b>Step 2 of 2</b>, put your phone back on your home Wi-Fi "
        "and scan the new code.</p>");
  } else if (welcome::due()) {
    // A new owner's radar greets them first (main.cpp, showWelcome), for 3
    // minutes at most: where the guide lives, for one who misses it.
    putf(server,
         "<p>When it says <b>Welcome</b>, you are done: put your phone back on your "
         "home Wi-Fi and scan the code on the radar for a short guide.</p>"
         "<p>Missed it? On your home Wi-Fi, open <b>%s/guide</b>.</p>",
         config::kPortalHostUrl);
  } else {
    put(server,
        "<p>When it shows planes again, you are done: put your phone back on your "
        "home Wi-Fi.</p>");
  }
  putf(server,
       "<div class='hn'>If it says <b>Could not join</b> or <b>Not found</b>, "
       "join <b>%s</b> again and pick the network once more.</div>",
       config::kPortalApName);
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
/** Rows converted per network write: one 720-byte write per row ran at ~45 KB/s. */
constexpr int kRowsPerWrite = 4;
constexpr size_t kScreenOutBytes = kRowsPerWrite * 240 * 3;

struct ScreenStream {
  WebServer* server;
  unsigned long started_ms;
  bool aborted;
  /** kScreenOutBytes, borrowed for the request rather than held for good. */
  uint8_t* out;
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
  uint8_t* const out = st->out;
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
    if (filled + row_bytes > kScreenOutBytes || y == rows - 1) {
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

  auto* out = static_cast<uint8_t*>(malloc(kScreenOutBytes));
  if (out == nullptr) {
    server.send(503, "text/plain", "busy: try again in a moment");
    return;
  }
  ScreenStream st{&server, millis(), false, out};
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
  const bool drawn = ui::radarDisplayCapture(sink, &gate);
  free(out);
  if (!drawn) {
    server.send(503, "text/plain", "no frame yet");
  }
  // An aborted picture needs nothing more: nothing was written after it
  // stopped, and the server closes the connection once this returns.
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
      "#nv{display:flex;gap:4px;margin:0 0 16px}"
      "#nv a{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;"
      "white-space:nowrap;text-align:center;padding:9px 4px;border-radius:8px;"
      "background:#f2f2f2;border:1px solid #dcdcdc;color:#444;"
      "text-decoration:none;font-size:14px;font-weight:400}"
      "#nv a.on{background:#1fa3ec;border-color:#1fa3ec;color:#fff}"
      "body.invert #nv a{background:#2b2b2b;border-color:#3a3a3a;color:#ddd}"
      "</style>"
      "<script>"
      "addEventListener('DOMContentLoaded',function(){"
      "var m=[['/','Home'],['/wifi','Wi-Fi'],['/settings','Settings'],"
      "['/guide','Guide'],['/info','System']],w=document.querySelector('.wrap');"
      "if(!w)return;"
      "var n=document.createElement('div');n.id='nv';"
      "m.forEach(function(e){var a=document.createElement('a');"
      "a.href=e[0];a.textContent=e[1];"
      "if(location.pathname==e[0]){a.className='on';"
      "document.title=e[1]+' - Plane Radar'}n.appendChild(a)});"
      "w.insertBefore(n,w.firstChild)});"
      "</script>";
  return kHead;
}

namespace {

// The hotspot's <head>: the light look of the home-network pages without the
// navigation (there is only the Wi-Fi form to see), WiFiManager's status box
// hidden (it reports the library's own attempts, which this firmware does not
// make), and a script that puts the step header (and for a new owner a
// greeting before it) above the form and says "Network" for SSID. The markup
// sits in the script's single-quoted string, so it has no single quotes:
// attributes go unquoted.
#define PLANE_RADAR_HOTSPOT_HEAD_START                                              \
  "<style>"                                                                         \
  ":root{color-scheme:light}"                                                       \
  "body{background:#fff;color:#222;font-family:system-ui,-apple-system,sans-serif;" \
  "padding:16px;margin:0}"                                                          \
  ".wrap{display:block;max-width:430px;margin:0 auto;text-align:left}"              \
  "button,input[type='submit']{border-radius:6px}"                                  \
  "input:not([type]),input[type=text],input[type=password]{box-sizing:border-box;"  \
  "padding:8px;font-size:16px;border:1px solid #ccc;border-radius:6px;"             \
  "background:#fff;color:#222}"                                                     \
  "a{color:#1a73c8}.msg{display:none}"                                              \
  ".wl{margin:0 0 12px;font-size:15px;line-height:1.45}"                            \
  ".st{margin:0 0 14px;padding:10px 12px;border-radius:6px;font-size:15px;"         \
  "line-height:1.4;background:#fff6e5;border:1px solid #f3d29a;color:#5a3d00}"      \
  "</style><script>"                                                                \
  "addEventListener('DOMContentLoaded',function(){"                                 \
  "var w=document.querySelector('.wrap');if(!w)return;"                             \
  "var d=document.createElement('div');d.innerHTML='"
#define PLANE_RADAR_HOTSPOT_HEAD_END                                                \
  "';w.insertBefore(d,w.firstChild);"                                               \
  "var l=document.querySelector(\"label[for='s']\");if(l)l.textContent='Network';"  \
  "document.title='Wi-Fi setup - Plane Radar'});"                                   \
  "</script>"

// The two heads as they are without a greeting: in flash, so the hotspot
// costs no RAM on the radars (nearly all of them) that are not new owners'.
constexpr char kHotspotHeadStepOne[] =
    PLANE_RADAR_HOTSPOT_HEAD_START
    "<div class=st><b>Step 1 of 2:</b> connect the radar to your home Wi-Fi. "
    "Only 2.4 GHz networks work.</div>" PLANE_RADAR_HOTSPOT_HEAD_END;
constexpr char kHotspotHeadWifiOnly[] =
    PLANE_RADAR_HOTSPOT_HEAD_START
    "<div class=st><b>Wi-Fi setup:</b> connect the radar to your Wi-Fi. "
    "Only 2.4 GHz networks work.</div>" PLANE_RADAR_HOTSPOT_HEAD_END;
/** Where a greeting goes in either: just inside the script's string. */
constexpr size_t kHotspotGreetingAt = sizeof(PLANE_RADAR_HOTSPOT_HEAD_START) - 1;

#undef PLANE_RADAR_HOTSPOT_HEAD_START
#undef PLANE_RADAR_HOTSPOT_HEAD_END

/** The greeting with the longest name (every character an entity) and wording. */
constexpr size_t kHotspotGreetingLen =
    sizeof("<p class=wl>Welcome to <b></b>! It shows the planes flying near you, live. "
           "Setup takes about two minutes.</p>") +
    util::portal::kMaxRadarName * 6;
constexpr size_t kHotspotHeadMax =
    (sizeof(kHotspotHeadStepOne) > sizeof(kHotspotHeadWifiOnly) ? sizeof(kHotspotHeadStepOne)
                                                                 : sizeof(kHotspotHeadWifiOnly)) +
    kHotspotGreetingLen;

}  // namespace

const char* hotspotHeadHtml(bool location_set, bool welcome, const char* name) {
  const char* plain = location_set ? kHotspotHeadWifiOnly : kHotspotHeadStepOne;
  if (!welcome) {
    return plain;
  }
  // WiFiManager keeps only the pointer and copies the text into every page it
  // builds, so it must outlive the portal. Taken from the heap the first time
  // a new owner's hotspot opens and kept (a later portal reuses it); that
  // setup ends in a restart. No room: the head without the greeting.
  static char* s_head = nullptr;
  if (s_head == nullptr) {
    s_head = static_cast<char*>(malloc(kHotspotHeadMax));
    if (s_head == nullptr) {
      return plain;
    }
  }
  // Written in place, piece by piece, with no copies on the stack (this runs
  // on loopTask, see PageOut). Bounded by construction: kHotspotHeadMax has
  // room for the longest greeting, and the name is cut by its escape, never
  // the script.
  size_t at = static_cast<size_t>(snprintf(s_head, kHotspotHeadMax,
                                           "%.*s<p class=wl>Welcome to <b>",
                                           static_cast<int>(kHotspotGreetingAt), plain));
  util::setup::htmlEscapeInScript(name, s_head + at, util::portal::kMaxRadarName * 6 + 1);
  const bool named = s_head[at] != '\0';
  if (!named) {
    snprintf(s_head + at, kHotspotHeadMax - at, "%s", kUnnamed);
  }
  at += strlen(s_head + at);
  snprintf(s_head + at, kHotspotHeadMax - at,
           "</b>%s It shows the planes flying near you, live. Setup takes about %s.</p>%s",
           afterName(named ? name : kUnnamed), location_set ? "a minute" : "two minutes",
           plain + kHotspotGreetingAt);
  return s_head;
}

void registerLanRoutes(WebServer& server, const Hooks& hooks) {
  server.on(kSettingsPath, HTTP_GET, [&server]() { handleSettings(server); });
  server.on(kGuidePath, HTTP_GET, [&server]() { handleGuide(server); });
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
