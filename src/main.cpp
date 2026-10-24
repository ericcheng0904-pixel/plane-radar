/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>

#include <algorithm>

#if __has_include(<driver/temp_sensor.h>)
#include <driver/temp_sensor.h>
#define PLANE_RADAR_HAS_TEMP_SENSOR 1
#endif

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/device_health.h"
#include "services/geocode.h"
#include "services/ota.h"
#include "services/routes.h"
#include "services/radar_location.h"
#include "services/tls.h"
#include "services/welcome.h"
#include "services/wifi_setup.h"
#include "ui/alert.h"
#include "ui/info_card.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "ui/trails.h"
#include "util/setup_text.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_redraw_ms = 0;
/** lastUpdateMs() of the sweep trails/alerts last saw; 0 = none yet. */
unsigned long g_last_sweep_ms = 0;
unsigned long g_last_temp_log_ms = 0;
/** The background fetch task; nullptr until it has started. */
TaskHandle_t g_adsb_task = nullptr;
unsigned long g_last_task_try_ms = 0;
/** The info screen (hold the button 2-9 s) is up until then; 0 = not shown. */
unsigned long g_info_until_ms = 0;
/** The updater has the frame buffer; nothing is drawn until it restarts the radar. */
bool g_updating = false;
constexpr unsigned long kInfoShowMs = 20000;

/** Put up the info screen: where the settings are, the version, the data. */
void showInfoScreen() {
  char version[40];
  snprintf(version, sizeof(version), "Plane Radar %s", config::kFirmwareVersion);
  char status[48];
  const unsigned long last = services::adsb::lastUpdateMs();
  if (last == 0) {
    snprintf(status, sizeof(status), "no flight data yet");
  } else {
    snprintf(status, sizeof(status), "%s, data %lu s ago", services::adsb::dataFeedName(),
             (millis() - last) / 1000);
  }
  statusScreenInfo(WiFi.localIP().toString().c_str(), version, status);
  g_info_until_ms = millis() + kInfoShowMs;
  g_radar_visible = false;
}

/** A setup screen shown in place of the radar while Wi-Fi is up. */
enum class SetupScreen : uint8_t { kNone, kLocationStep, kLocationSaved, kWelcome };
SetupScreen g_setup_screen = SetupScreen::kNone;
/** The address setup step 2 or the welcome shows; drawn again if DHCP hands out another. */
uint32_t g_setup_screen_ip = 0;
/** "Location saved" stays up until then. */
unsigned long g_location_saved_until_ms = 0;
constexpr unsigned long kLocationSavedScreenMs = 3000;

/**
 * The welcome (services::welcome) is still to come or up this boot; decided
 * in setup(). The radar is not drawn meanwhile.
 */
bool g_welcome_pending = false;
/** When the welcome first appeared; 0 = not yet. Its time runs on behind "Wi-Fi lost". */
unsigned long g_welcome_since_ms = 0;
/** A tap on the welcome, for showWelcome(). */
bool g_welcome_tapped = false;
constexpr unsigned long kWelcomeShowMs = 3UL * 60UL * 1000UL;

/** Pin the core clock to its maximum; see config::kCpuFreqFullMhz. */
void applyCpuClock() {
  setCpuFrequencyMhz(config::kCpuFreqFullMhz);
  Serial.printf("CPU clock: %u MHz\n",
                static_cast<unsigned>(getCpuFrequencyMhz()));
}

/**
 * Chip temperature to the serial log, so power tuning can be measured, with
 * the heap and the least stack each task has had left since boot (bytes).
 * "loop stack left" is loopTask's 8 KB, which the web pages share with
 * everything else on the main loop. Not inlined: its printf's arguments would
 * widen loop()'s frame, which is under every web request.
 */
__attribute__((noinline)) void logChipTemperature() {
  if (millis() - g_last_temp_log_ms < config::kTempLogIntervalMs) {
    return;
  }
  g_last_temp_log_ms = millis();
#ifdef PLANE_RADAR_HAS_TEMP_SENSOR
  float celsius = 0.0f;
  if (temp_sensor_read_celsius(&celsius) == ESP_OK) {
    const unsigned long last = services::adsb::lastUpdateMs();
    Serial.printf("Chip temperature: %.1f C, heap free %u (largest %u, min %u), "
                  "fetch task %s (stack left %u), loop stack left %u, last data %s%lu s%s\n",
                  celsius, static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(ESP.getMinFreeHeap()),
                  g_adsb_task != nullptr ? "running" : "NOT RUNNING",
                  g_adsb_task != nullptr
                      ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(g_adsb_task))
                      : 0u,
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                  last == 0 ? "never (up " : "",
                  (millis() - last) / 1000, last == 0 ? ")" : " ago");
  }
#endif
}

void initTemperatureSensor() {
#ifdef PLANE_RADAR_HAS_TEMP_SENSOR
  temp_sensor_config_t cfg = TSENS_CONFIG_DEFAULT();
  temp_sensor_set_config(cfg);
  temp_sensor_start();
#endif
}

/**
 * The radar's two big blocks, taken once and while the heap is still whole:
 * the frame buffer (57.6 KB), then mbedTLS's record buffers (2 x 16.7 KB),
 * which the fetch task sets aside before its next session (services::tls).
 * Later calls cost nothing.
 */
void reserveMemory() {
  if (ui::radarDisplayReserve()) {
    services::tls::reserveRecordBuffers();
  }
}

void showRadarIfConnected() {
  // Without a location the radar would show the default's sky; setup step 2
  // asks for one instead (showSetupScreens). A new owner's welcome comes
  // before the radar too.
  if (WiFi.status() != WL_CONNECTED || !services::location::isSet() || g_welcome_pending) {
    g_radar_visible = false;
    return;
  }
  reserveMemory();
  ui::radarDisplayDraw();
  g_radar_visible = true;
  services::ota::noteRadarDrawn();
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

/** Portal saves land in wifiLoop(); apply new label sizes / layers right away. */
void applyDisplaySettingsIfChanged() {
  if (!wifiConsumeDisplaySettingsChanged()) {
    return;
  }
  ui::radarDisplayInvalidateStyle();
  // The radar centre may have moved with the same save, which would leave every
  // stored trail pointing at the wrong place.
  services::adsb::discardSweepsBefore(millis());
  ui::trails::clear();
  ui::alert::reset();
  ui::card::reset();
  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  const bool tap = bootButtonConsumeTap();
  if (g_info_until_ms != 0) {
    // On the info screen a tap closes it (and changes nothing else).
    if (tap || static_cast<long>(millis() - g_info_until_ms) >= 0) {
      g_info_until_ms = 0;
    }
    return;
  }
  if (bootButtonConsumeInfo() && WiFi.status() == WL_CONNECTED && g_radar_visible) {
    showInfoScreen();
    return;
  }
  if (tap && g_setup_screen == SetupScreen::kWelcome) {
    g_welcome_tapped = true;  // skips the welcome, and changes nothing else
    return;
  }
  // A tap changes the range only where the range can be seen: on a setup or
  // "Wi-Fi lost" screen it would change it unseen, behind the message.
  if (tap && g_radar_visible) {
    onRangeTap();
  }
}

/**
 * The welcome, once, for a new owner: in place of the radar, a QR code for
 * the guide page. It goes on a tap, when the guide is opened, or after
 * kWelcomeShowMs, and is then recorded as shown. Flights are fetched behind
 * it all the while, so the radar has planes to show the moment it goes. The
 * radar's first frame, one of an update's trial conditions, waits those three
 * minutes at most: well inside the 15 a trial runs before it can be confirmed,
 * and an update that starts meanwhile takes the screen as usual (loop()).
 * True while it owns the screen.
 */
bool showWelcome() {
  if (!g_welcome_pending) {
    g_setup_screen = SetupScreen::kNone;
    return false;
  }
  const unsigned long now = millis();
  if (g_welcome_since_ms == 0) {
    g_welcome_since_ms = now != 0 ? now : 1;
  }
  // A guide opened earlier this boot counts: it is what the welcome is for.
  const util::setup::WelcomeEnd end =
      util::setup::welcomeEnd(g_welcome_since_ms, now, g_welcome_tapped,
                              services::welcome::consumeGuideOpened(), kWelcomeShowMs);
  if (end != util::setup::WelcomeEnd::kNotYet) {
    static const char* const kWhy[] = {"", "tapped", "guide opened", "timed out"};
    Serial.printf("Welcome over (%s)\n", kWhy[static_cast<uint8_t>(end)]);
    services::welcome::markShown();
    g_welcome_pending = false;
    g_welcome_tapped = false;
    g_setup_screen = SetupScreen::kNone;
    return false;  // the radar is drawn on this same pass
  }
  const uint32_t ip = static_cast<uint32_t>(WiFi.localIP());
  if (g_setup_screen != SetupScreen::kWelcome || ip != g_setup_screen_ip) {
    // As on step 2: the frame buffer is taken while the heap is still whole.
    reserveMemory();
    statusScreenWelcome(services::welcome::name(), WiFi.localIP().toString().c_str());
    g_setup_screen = SetupScreen::kWelcome;
    g_setup_screen_ip = ip;
    g_radar_visible = false;
  }
  statusScreenHoldOverlay(bootButtonHeldMs());
  return true;
}

/**
 * Setup step 2 and the confirmation after it, in place of the radar while
 * Wi-Fi is up: with no location saved the screen asks for one (a QR code for
 * the settings page), and any save says for a few seconds where the radar now
 * looks. Then, once, a new owner's welcome. True while one of them owns the
 * screen.
 */
bool showSetupScreens() {
  services::geocode::applyResult();  // a town lookup that has just finished
  if (services::location::consumeChanged()) {
    // Nothing drawn or remembered for the old centre may survive the move.
    services::adsb::discardSweepsBefore(millis());
    ui::trails::clear();
    ui::alert::reset();
    ui::card::reset();
    statusScreenLocationSaved(services::location::label());
    g_location_saved_until_ms = millis() + kLocationSavedScreenMs;
    g_setup_screen = SetupScreen::kLocationSaved;
    g_radar_visible = false;
    services::geocode::poke();  // the first sweep for the new place, now
    return true;
  }
  if (g_setup_screen == SetupScreen::kLocationSaved) {
    if (static_cast<long>(millis() - g_location_saved_until_ms) < 0) {
      statusScreenHoldOverlay(bootButtonHeldMs());
      return true;
    }
    g_setup_screen = SetupScreen::kNone;
  }
  if (services::location::isSet()) {
    return showWelcome();
  }
  const uint32_t ip = static_cast<uint32_t>(WiFi.localIP());
  if (g_setup_screen != SetupScreen::kLocationStep || ip != g_setup_screen_ip) {
    reserveMemory();
    statusScreenLocationStep(WiFi.localIP().toString().c_str());
    g_setup_screen = SetupScreen::kLocationStep;
    g_setup_screen_ip = ip;
    g_radar_visible = false;
  }
  statusScreenHoldOverlay(bootButtonHeldMs());
  return true;
}

/**
 * Feed each new sweep from the fetch task to trails and alerts, once. Runs on
 * the main loop so trails/alert state is only ever touched by one thread; the
 * fetch task just publishes, and lastUpdateMs() tells us a new sweep landed.
 */
void consumeNewSweep() {
  const unsigned long sweep_ms = services::adsb::lastUpdateMs();
  if (sweep_ms == 0 || sweep_ms == g_last_sweep_ms) {
    return;
  }
  static services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  const size_t n = services::adsb::snapshotAircraft(
      planes, services::adsb::kMaxAircraft, &g_last_sweep_ms);
  const unsigned long now = millis();
  ui::trails::update(planes, n, now);
  ui::alert::scan(planes, n, now);
  ui::radarDisplayRefreshAircraft();
  g_last_redraw_ms = millis();  // that was this pass's redraw
  ui::radarDisplayStartAlert();
}

// ADS-B fetch runs on its own task: the HTTP request blocks for up to a few
// seconds, and keeping it off the main loop lets the radar keep redrawing
// (dead-reckoned) at 4 Hz throughout. The task publishes into the shared
// aircraft buffer under a lock; the render loop reads a snapshot.
void adsbFetchTask(void*) {
  unsigned long interval_ms = config::kAdsbFetchIntervalMs;
  for (;;) {
    services::health::checkFromFetchTask();
    // Web pages wait while a request is out, one at a time (services::tls).
    services::tls::beginSessions();
    // A town lookup from the settings page takes a pass of its own: one
    // connection at a time, never alongside an aircraft fetch.
    // An update check takes a pass of its own too (and restarts the radar
    // when it installs one).
    const bool looked_up = services::ota::runIfDue() || services::geocode::runPending();
    services::tls::endSessions();
    // No location yet (setup step 2): there is no sky to fetch.
    if (!looked_up && WiFi.status() == WL_CONNECTED && services::location::isSet()) {
      services::tls::beginSessions();
      const unsigned long started = millis();  // before the centre is read
      const services::adsb::FetchResult got =
          services::adsb::fetchUpdate(services::location::lat(), services::location::lon(),
                                      ui::radar::fetchRadiusKm(), started);
      services::tls::endSessions();
      if (got == services::adsb::FetchResult::kAircraft) {
        interval_ms = config::kAdsbFetchIntervalMs;
        services::tls::beginSessions();
        services::routes::refresh();  // origin/destination for new flights
        services::tls::endSessions();
      } else if (got == services::adsb::FetchResult::kPaced) {
        // adsb.lol's ration holds the next request back: not a failure, and
        // looked at again at the usual interval, so aircraft come as soon as
        // the pace allows. A pass held back sends nothing and takes no time.
        interval_ms = config::kAdsbFetchIntervalMs;
      } else if (services::adsb::allFeedsFailing()) {
        // Every feed has failed in turn (outage, rate limits): back off.
        interval_ms = std::min(interval_ms * 2, config::kAdsbMaxBackoffMs);
        Serial.printf("adsb: all feeds failing, next try in %lu s\n", interval_ms / 1000);
      }
    }
    // The fetch an update check or a town lookup displaced is due: wait no
    // longer than the usual interval for it. Waiting a whole backoff after
    // such a pass as well put up to ~195 s between fetch attempts, and the
    // health check restarts the radar after 180 s without one (measured
    // from the start of fetchUpdate). With every step of a request running
    // to its timeout (connect 5 s, handshake 12 s, headers 8 s, body 8 s), a
    // fetch takes ~33 s and a route request ~32 s; routes follow only a
    // fetch that worked, the 60 s backoff only one that failed. The longest
    // gaps are then ~93 s after a failed fetch, ~75 s after one with routes,
    // and ~143 s with an update check (~40 s) or a town search taking the
    // next pass: well under 180 s. Even a DNS server taking its full 15 s
    // for every name keeps the worst inside it (~173 s), and so does a web
    // page being sent before each of the four turns in that gap (1.5 s
    // each, services::tls::beginSessions(): ~179 s).
    // Woken early by a lookup request or a newly saved location.
    services::geocode::idle(looked_up ? std::min(interval_ms, config::kAdsbFetchIntervalMs)
                                      : interval_ms);
  }
}

/** Start the fetch task if it is not running; logs and reports failure. */
bool startAdsbTask() {
  if (g_adsb_task != nullptr) {
    return true;
  }
  g_last_task_try_ms = millis();
  // The C3 has a single core: the task time-slices with loop() at the same
  // priority. It checks Wi-Fi each cycle.
  if (xTaskCreatePinnedToCore(adsbFetchTask, "adsb", config::kAdsbTaskStackBytes,
                              nullptr, 1, &g_adsb_task, 0) != pdPASS) {
    g_adsb_task = nullptr;
    Serial.printf("adsb: fetch task did NOT start (largest free block %u) - retrying\n",
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return false;
  }
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  services::health::begin();
  services::ota::begin();
  bootButtonInit();
  services::location::init();
  services::welcome::init();
  g_welcome_pending = services::welcome::due();
  ui::radar::rangeInit();
  applyCpuClock();
  initTemperatureSensor();

  // No setPollFn(wifiLoop): the fetch runs on its own task, and calling the
  // portal's WebServer from there would race the main loop's wifiLoop().
  services::adsb::init();
  services::routes::init();
  services::geocode::init();
  // Start the fetch task now, while the heap is still in one piece: it waits
  // for Wi-Fi on its own. Started after Wi-Fi setup, as before, it failed
  // whenever setup went through the hotspot portal — the portal left no free
  // block as big as the stack, xTaskCreate failed unchecked, and the radar
  // showed an empty sky until the next power cycle.
  startAdsbTask();

  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  services::health::checkFromLoop();
  services::ota::checkTrial();
  if (services::ota::wantsDisplay()) {
    if (!g_updating) {
      g_updating = true;
      g_radar_visible = false;
      ui::radarDisplayReleaseBuffer();
      statusScreenUpdating();
      services::ota::displayReleased();
    }
    wifiLoop();  // keep the settings page answering
    delay(10);
    return;
  }
  if (g_adsb_task == nullptr && millis() - g_last_task_try_ms >= 5000) {
    startAdsbTask();
  }
  handleBootButton();
  wifiLoop();
  applyDisplaySettingsIfChanged();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
      Serial.println("WiFi lost - waiting for auto-reconnect");
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    // Home networks hiccup for a few seconds all the time. Leave the last
    // radar picture up while that plays out; only an outage that outlasts
    // kWifiConnectingScreenDelayMs is worth taking the screen away for.
    const bool show_ui = down_ms >= config::kWifiConnectingScreenDelayMs;
    if (show_ui && (g_radar_visible || g_setup_screen != SetupScreen::kNone)) {
      g_radar_visible = false;
      g_setup_screen = SetupScreen::kNone;  // drawn again once Wi-Fi is back
      wifiShowLostScreen();
    }

    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect(g_wifi_down_since)) {
        g_wifi_down_since = 0;
        // The attempt may have put up "Wi-Fi lost" over whatever was there
        // (a setup screen or the welcome is drawn again on the next pass).
        g_setup_screen = SetupScreen::kNone;
        showRadarIfConnected();
      }
    }
  } else {
    if (g_wifi_down_since != 0) {
      Serial.printf("WiFi back after %lu ms\n", millis() - g_wifi_down_since);
      g_wifi_down_since = 0;
    }
    if (showSetupScreens()) {
      // Setup step 2, "Location saved" or the welcome: the radar waits.
    } else if (g_info_until_ms != 0) {
      // The info screen is up; handleBootButton() takes it down.
    } else if (!g_radar_visible) {
      showRadarIfConnected();
    } else {
      consumeNewSweep();
      if (millis() - g_last_redraw_ms >= ui::radarDisplayRedrawIntervalMs()) {
        // Redraw at 4 Hz with dead-reckoned positions; the ADS-B fetch runs on
        // its own task (adsbFetchTask).
        g_last_redraw_ms = millis();
        ui::radarDisplayRefreshAircraft();
      }
    }
  }

  logChipTemperature();
  delay(10);
}
