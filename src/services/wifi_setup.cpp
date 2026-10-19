#include "services/wifi_setup.h"

#include <WiFi.h>
#include <nvs_flash.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/device_health.h"
#include "services/portal_pages.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "util/hold_detector.h"
#include "util/setup_text.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootTapMaxMs &&
        now >= config::kBootIgnoreAfterPowerUpMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

/**
 * A network from the hotspot form gets this long to connect. A wrong password
 * usually shows in a few seconds (see kJoinVerdictDisconnects); WiFiManager's
 * own save waited up to a minute with nothing on the screen.
 */
constexpr unsigned long kJoinAttemptMs = 20000;
/**
 * Disconnects (the core retries in between) before a join is judged failed.
 * One can be a fluke of a busy router; two of the same kind are an answer.
 */
constexpr uint32_t kJoinVerdictDisconnects = 2;
/**
 * Pause between the form arriving and the attempt: trying a network moves the
 * radio to that network's channel, which drops the phone off the hotspot, so
 * the "watch the radar's screen" page has to reach the phone first.
 */
constexpr unsigned long kJoinStartDelayMs = 1500;
/** How long "Could not join" stays up before step 1 comes back. */
constexpr unsigned long kJoinFailedScreenMs = 20000;
/** Between the "Erasing" page leaving and the reset that follows it. */
constexpr unsigned long kFactoryResetDelayMs = 800;

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();
void applyTxPower();

/** WPA2 minimum is 8 characters. */
constexpr size_t kApPasswordLen = 8;
char s_ap_password[kApPasswordLen + 1] = {};

/**
 * WiFiManager registers every route without authentication — its own auth hook
 * is a no-op in 2.0.17 — and the default menu even links an OTA upload form.
 * Anyone who can reach the device on the LAN could otherwise push firmware,
 * wipe credentials, or reboot it. These routes serve nothing this firmware
 * needs: there is no second OTA slot to flash into, and a credential reset is
 * available by holding BOOT on the device itself.
 */
const char* const kBlockedPortalRoutes[] = {"/update", "/u", "/erase",
                                            "/restart"};

/**
 * Portal menu. No "param" entry: the settings live on our own /settings page,
 * which the navigation bar links from every page. No OTA entry either — that
 * route is blocked, see kBlockedPortalRoutes.
 */
const char* kPortalMenu[] = {"wifi", "info"};

bool s_display_settings_changed = false;

/** Runs after /settingssave has stored everything the form carried. */
void onSettingsSaved() {
  // Transmit power is the one setting that can take hold without a redraw.
  applyTxPower();
  s_display_settings_changed = true;
}

/** millis() when "Reset for a new owner" was confirmed; 0 = not asked. */
unsigned long s_factory_reset_at = 0;
bool s_factory_reset_keep_location = false;

void onFactoryResetConfirmed(bool keep_location) {
  s_factory_reset_keep_location = keep_location;
  s_factory_reset_at = millis();
  if (s_factory_reset_at == 0) {
    s_factory_reset_at = 1;
  }
}

/** A network and password from the hotspot form, waiting to be tried. */
struct JoinRequest {
  char ssid[33];
  char password[65];
  bool pending;
  unsigned long at_ms;
};
JoinRequest s_join = {};

void onJoinRequested(const char* ssid, const char* password) {
  snprintf(s_join.ssid, sizeof(s_join.ssid), "%s", ssid);
  snprintf(s_join.password, sizeof(s_join.password), "%s", password);
  s_join.pending = true;
  s_join.at_ms = millis();
}

/**
 * The station's last disconnect reason, kept by the firmware itself.
 * WiFiManager has a getLastConxResult(), but in 2.0.17 the event handler that
 * feeds it is only installed by autoConnect(), which this firmware does not
 * use. Written from the WiFi event task, read on the main loop.
 */
volatile uint8_t s_last_disconnect_reason = 0;
volatile uint32_t s_disconnect_count = 0;

void onStaDisconnected(arduino_event_id_t, arduino_event_info_t info) {
  const uint8_t reason = info.wifi_sta_disconnected.reason;
  if (reason == WIFI_REASON_ASSOC_LEAVE) {
    return;  // our own disconnect() (or the core's, before it retries): no verdict
  }
  s_last_disconnect_reason = reason;
  s_disconnect_count = s_disconnect_count + 1;
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

/** The saved network's name (for the screen), "" if none. */
void savedSsid(char* out, size_t len) {
  out[0] = '\0';
  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || len == 0) {
    return;
  }
  const size_t n = strnlen(reinterpret_cast<const char*>(conf.sta.ssid), sizeof(conf.sta.ssid));
  const size_t copy = n < len - 1 ? n : len - 1;
  memcpy(out, conf.sta.ssid, copy);
  out[copy] = '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  // Left true. The driver takes its storage mode from this flag each time it
  // starts from off, and the hotspot that follows a reset starts it: with the
  // flag false it ran on RAM-only storage, so the network chosen next was
  // forgotten at the next restart (the daily one at the latest). WiFiManager's
  // erase() sets it false behind our back, hence the explicit reset here.
  WiFi.persistent(true);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  // Location and display settings stay: re-joining Wi-Fi is the only thing a
  // reset is for, and they are a nuisance to re-enter after an accidental one.
  Serial.println("WiFi credentials cleared (location and settings kept)");
}

/**
 * Reset for a new owner (settings page, confirmed): location, display
 * settings and Wi-Fi all go, and the radar restarts at setup step 1.
 */
[[noreturn]] void factoryReset(bool keep_location) {
  // What to put back afterwards, if the giver chose to keep the location.
  const bool keep = keep_location && services::location::isSet();
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  char place[48];
  snprintf(place, sizeof(place), "%s", services::location::label());

  services::location::clear();
  ui::radar::unitsReset();
  eraseWifiCredentials();
  // Then all of the settings storage: display settings, update records, and
  // anything an earlier firmware left there (the vendor's firmware kept Wi-Fi
  // passwords of its own, in a namespace this one never reads).
  nvs_flash_deinit();
  nvs_flash_erase();
  nvs_flash_init();
  if (keep) {
    services::location::save(lat, lon, place);
  }
  markForceConfigPortal();
  Serial.printf("Reset for a new owner: settings and Wi-Fi erased%s\n",
                keep ? ", location kept" : "");
  statusScreenErased();
  delay(1500);
  esp_restart();
}

/**
 * Runs after WiFiManager creates its web server but before it registers its own
 * routes. ESP32's WebServer dispatches to the first handler that matches, so
 * claiming the dangerous URIs here shadows the library's versions for good.
 */
void onWebServerStarted() {
  if (s_wm.server == nullptr) {
    return;
  }
  for (const char* route : kBlockedPortalRoutes) {
    s_wm.server->on(route, HTTP_ANY, []() {
      s_wm.server->send(404, "text/plain", "Not found");
    });
  }
  services::portal::Hooks hooks;
  hooks.on_settings_saved = onSettingsSaved;
  hooks.on_factory_reset = onFactoryResetConfirmed;
  hooks.on_join = onJoinRequested;
  // The config portal is flagged active before its server is created; the
  // LAN portal only after.
  if (s_wm.getConfigPortalActive()) {
    services::portal::registerHotspotRoutes(*s_wm.server, hooks);
  } else {
    services::portal::registerLanRoutes(*s_wm.server, hooks);
  }
}

/**
 * Transmit power from the portal setting. The firmware has always run at
 * 8.5 dBm — a cap that keeps the Super Mini's regulator out of trouble at the
 * cost of link margin, which is the first thing to raise when the connection
 * drops in a weak spot.
 */
void applyTxPower() {
  wifi_power_t power = WIFI_POWER_8_5dBm;
  switch (ui::radar::txPowerDbm()) {
    case 13:
      power = WIFI_POWER_13dBm;
      break;
    case 19:
      power = WIFI_POWER_19_5dBm;
      break;
    default:
      break;
  }
  WiFi.setTxPower(power);
}

void onConfigPortalApStarted(WiFiManager*) {
  // No screen here: openConfigPortal() has already drawn the one that fits
  // (step 1, or "can't reach" while a saved network is retried), and drawing
  // step 1 here used to cover the latter the moment the hotspot came up.
  applyTxPower();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  s_wm.setWebServerCallback(onWebServerStarted);
  s_wm.setTitle("Plane Radar");
  s_wm.setCustomHeadElement(services::portal::headHtml());
  s_wm.setMenu(kPortalMenu, sizeof(kPortalMenu) / sizeof(kPortalMenu[0]));
  // Both buttons lead to routes this firmware refuses to serve, so the info
  // page should not advertise them. The "Available pages" table that used to
  // list /u, /restart and /erase alongside them is compiled out by WM_NOHELP.
  s_wm.setShowInfoErase(false);
  s_wm.setShowInfoUpdate(false);
  // No setSaveConnectTimeout(): /wifisave is answered by portal_pages, so the
  // library's own save-then-connect, which that would bound, never runs.
  WiFi.onEvent(onStaDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  applyTxPower();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

/** What a reconnect shows while it waits. */
enum class ConnectUi : uint8_t { kNone, kConnecting, kLost };

bool s_lost_screen_up = false;
unsigned long s_lost_tick_ms = 0;
/** When the link dropped, during wifiReconnect(); 0 otherwise. */
unsigned long s_lost_at_ms = 0;

bool waitForLinkWithUi(ConnectUi ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    services::health::heartbeat();  // a reconnect blocks the loop for a while
    bootButtonPollLongPress();
    // A quiet reconnect started inside the grace period can outlast it: the
    // screen should still change when the outage turns into a real one.
    if (ui == ConnectUi::kNone && s_lost_at_ms != 0 &&
        millis() - s_lost_at_ms >= config::kWifiConnectingScreenDelayMs) {
      wifiShowLostScreen();
      ui = ConnectUi::kLost;
    }
    if (ui == ConnectUi::kConnecting) {
      statusScreenConnectingTick();
    } else if (ui == ConnectUi::kLost) {
      statusScreenWifiLostTick();
    }
    if (ui != ConnectUi::kNone) {
      statusScreenHoldOverlay(bootButtonHeldMs());
    }
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, ConnectUi ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (ui == ConnectUi::kConnecting) {
    statusScreenConnectingBegin(ui_ssid);
  } else if (ui == ConnectUi::kLost) {
    wifiShowLostScreen();
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(ConnectUi ui) {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }

  if (conf.sta.ssid[0] == '\0') {
    return false;
  }

  // ESP-IDF stores the SSID in a fixed 32-byte field. A maximum-length
  // SSID has no room for a trailing NUL, so copy it to a larger buffer
  // and explicitly terminate it before constructing an Arduino String.
  char ssid_buf[sizeof(conf.sta.ssid) + 1] = {};
  memcpy(ssid_buf, conf.sta.ssid, sizeof(conf.sta.ssid));
  ssid_buf[sizeof(conf.sta.ssid)] = '\0';

  char pass_buf[sizeof(conf.sta.password) + 1] = {};
  memcpy(pass_buf, conf.sta.password, sizeof(conf.sta.password));
  pass_buf[sizeof(conf.sta.password)] = '\0';

  const String ssid(ssid_buf);
  const String pass(pass_buf);

  s_last_disconnect_reason = 0;
  return tryConnectWithUi(ssid, pass, ui);
}

/**
 * Try the network from the hotspot form, and keep it only if it works.
 *
 * WiFiManager stored the password before trying it and then waited up to a
 * minute with nothing on the screen; a typo was kept, and the radar later
 * reported "can't reach" a network it could see perfectly well. Here the
 * attempt runs with its own screen and a bounded wait, the disconnect reason
 * says what went wrong, and a failure puts back whatever was stored before.
 */
bool attemptJoin(const char* ssid, const char* password, util::setup::JoinFailure* why) {
  Serial.printf("Joining '%s'\n", ssid);
  statusScreenConnectingBegin(ssid);

  // Station on (the hotspot stays: AP + station) before its stored network
  // is read, and later put back, through it.
  WiFi.enableSTA(true);
  wifi_config_t previous = {};
  esp_wifi_get_config(WIFI_IF_STA, &previous);
  // Flash, whatever the driver started with: see eraseWifiCredentials().
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);

  s_disconnect_count = 0;
  s_last_disconnect_reason = 0;
  applyTxPower();
  WiFi.setAutoReconnect(true);  // the core retries the reasons worth a retry
  WiFi.begin(ssid, password);

  const unsigned long start = millis();
  bool joined = false;
  while (millis() - start < kJoinAttemptMs) {
    if (wifiLinkUp()) {
      joined = true;
      break;
    }
    // Two failures, or one the core will not retry (an outright AUTH_FAIL).
    if (s_disconnect_count >= kJoinVerdictDisconnects ||
        (s_disconnect_count > 0 && WiFi.status() == WL_CONNECT_FAILED)) {
      break;
    }
    services::health::heartbeat();
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    statusScreenHoldOverlay(bootButtonHeldMs());
    delay(config::kWifiConnectingFrameMs);
  }
  if (joined) {
    Serial.printf("Joined '%s'  IP %s\n", ssid, WiFi.localIP().toString().c_str());
    return true;
  }

  *why = util::setup::classifyDisconnect(s_last_disconnect_reason);
  Serial.printf("Could not join '%s' (reason %u)\n", ssid,
                static_cast<unsigned>(s_last_disconnect_reason));
  WiFi.setAutoReconnect(false);
  esp_wifi_disconnect();
  delay(100);
  if (previous.sta.ssid[0] != '\0') {
    esp_wifi_set_config(WIFI_IF_STA, &previous);  // the network that worked before
  } else {
    WiFi.disconnect(false, true);  // nothing before: nothing now
  }
  // Hotspot only: a station still searching drags the hotspot's channel along.
  WiFi.enableSTA(false);
  return false;
}

/**
 * What the round screen shows while the hotspot is open. A tap on BOOT steps
 * through the views (the QR code, the password in words, and in retry mode the
 * "can't reach" message), a phone joining the hotspot switches to "carry on on
 * the phone", and a failed join shows why for a while before step 1 returns.
 */
class PortalScreen {
 public:
  PortalScreen(const char* retry_ssid, bool password_suspect)
      : retry_(retry_ssid != nullptr), suspect_(password_suspect) {
    snprintf(ssid_, sizeof(ssid_), "%s", retry_ssid != nullptr ? retry_ssid : "");
    view_ = home();
  }

  void draw() const {
    switch (view_) {
      case View::kRetry:
        statusScreenPortalRetrying(ssid_, suspect_);
        break;
      case View::kText:
        statusScreenPortalText();
        break;
      case View::kPhone:
        statusScreenPortalPhone();
        break;
      case View::kFailed:
        break;  // drawn by the caller, which knows why
      case View::kCode:
      default:
        statusScreenPortal();
        break;
    }
  }

  /** The caller has just drawn a failure; it stays until `until_ms` or a tap. */
  void failed(unsigned long until_ms) {
    view_ = View::kFailed;
    failed_until_ms_ = until_ms;
  }

  /** One pass of the portal loop. */
  void update(bool tap, int stations, unsigned long now) {
    if (view_ == View::kFailed) {
      stations_ = stations;
      if (tap || static_cast<long>(now - failed_until_ms_) >= 0) {
        show(home());
      }
      return;
    }
    if ((stations > 0) != (stations_ > 0)) {
      stations_ = stations;
      show(home());
      return;
    }
    stations_ = stations;
    if (tap) {
      show(next());
    }
  }

 private:
  enum class View : uint8_t { kRetry, kCode, kText, kPhone, kFailed };

  View home() const {
    if (stations_ > 0) {
      return View::kPhone;
    }
    return retry_ ? View::kRetry : View::kCode;
  }

  View next() const {
    switch (view_) {
      case View::kRetry:
        return View::kCode;
      case View::kCode:
      case View::kPhone:
        return View::kText;
      case View::kText:
      default:
        return home();
    }
  }

  void show(View v) {
    view_ = v;
    draw();
  }

  bool retry_;
  bool suspect_;
  char ssid_[33] = {};
  View view_ = View::kCode;
  int stations_ = 0;
  unsigned long failed_until_ms_ = 0;
};

/**
 * Run the setup portal until Wi-Fi is configured, or — with `retry_ssid` set
 * (a saved network exists but was out of reach) — until kSavedWifiRetryMs
 * passes with nobody on the hotspot, so the caller can try the saved network
 * again. Without that, a radar that booted before its router would sit in
 * setup mode for good.
 */
bool openConfigPortal(const char* retry_ssid = nullptr, bool password_suspect = false) {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  PortalScreen screen(retry_ssid, password_suspect);
  screen.draw();  // before the hotspot: starting it takes a second or two
  s_join.pending = false;
  s_wm.setCustomHeadElement(services::portal::hotspotHeadHtml(services::location::isSet()));
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName, wifiSetupApPassword());
  unsigned long quiet_since = millis();
  const unsigned long opened_ms = millis();
  bool joined = false;
  while (s_wm.getConfigPortalActive()) {
    services::health::heartbeat();
    bootButtonPollLongPress();
    s_wm.process();  // serves the pages; a submitted network lands in s_join
    if (s_join.pending && millis() - s_join.at_ms >= kJoinStartDelayMs) {
      s_join.pending = false;
      util::setup::JoinFailure why = util::setup::JoinFailure::kNone;
      joined = attemptJoin(s_join.ssid, s_join.password, &why);
      memset(s_join.password, 0, sizeof(s_join.password));
      if (joined) {
        break;
      }
      statusScreenJoinFailed(s_join.ssid, why);
      screen.failed(millis() + kJoinFailedScreenMs);
      quiet_since = millis();
    }
    const int stations = WiFi.softAPgetStationNum();
    screen.update(bootButtonConsumeTap(), stations, millis());
    statusScreenHoldOverlay(bootButtonHeldMs());
    if (stations > 0 &&
        (retry_ssid == nullptr || millis() - opened_ms < config::kSavedWifiRetryHoldMaxMs)) {
      quiet_since = millis();  // someone is setting it up: do not pull the rug
    } else if (retry_ssid != nullptr &&
               millis() - quiet_since >= config::kSavedWifiRetryMs) {
      s_wm.stopConfigPortal();
      break;
    }
    delay(10);
  }
  if (joined) {
    s_wm.stopConfigPortal();  // the hotspot goes; the new link stays
  }
  // Taps here switched views; left latched, the radar would read the last
  // one as a range change once it appears.
  bootButtonConsumeTap();
  s_wm.setCustomHeadElement(services::portal::headHtml());
  return joined && wifiLinkUp();
}

/** The last attempt on the saved network ended the way a changed password does. */
bool passwordSuspect() {
  return util::setup::classifyDisconnect(s_last_disconnect_reason) ==
         util::setup::JoinFailure::kWrongPassword;
}

/** Setup is over and the link is up: from now on it reconnects by itself. */
bool connectedNow() {
  WiFi.setAutoReconnect(true);
  // Taps during setup switched views or did nothing; left latched, the radar
  // would read the last one as a range change the moment it appears.
  bootButtonConsumeTap();
  Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
  return true;
}

}  // namespace

const char* wifiSetupApPassword() {
  if (s_ap_password[0] != '\0') {
    return s_ap_password;
  }

  // Ambiguous glyphs (0/O, 1/I/l) are left out: the password is read off a
  // 1.28" screen and typed on a phone.
  static constexpr char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
  constexpr uint64_t kAlphabetMask = 31;  // sizeof(kAlphabet) - 1 == 32 chars

  uint8_t mac[6] = {};
  WiFi.macAddress(mac);

  uint64_t bits = 0;
  for (uint8_t octet : mac) {
    bits = (bits << 8) | octet;
  }
  for (size_t i = 0; i < kApPasswordLen; ++i) {
    s_ap_password[i] = kAlphabet[bits & kAlphabetMask];
    bits >>= 5;
  }
  s_ap_password[kApPasswordLen] = '\0';
  return s_ap_password;
}

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

namespace {
// Long-press detection, kept apart from the tap ISR: a hold is measured only
// from samples taken here (util::HoldDetector), so a missed interrupt can
// never leave a stale press time behind that a later touch would complete.
util::HoldDetector s_hold(config::kBootResetHoldMs, config::kBootPollGapMs,
                          config::kBootIgnoreAfterPowerUpMs);
}  // namespace

unsigned long bootButtonHeldMs() { return s_hold.heldMs(millis()); }

namespace {
/** The last watched hold's length, to see it end; and an info request. */
unsigned long s_last_held_ms = 0;
bool s_info_pending = false;
}  // namespace

void bootButtonPollLongPress() {
  const unsigned long now = millis();
  if (s_hold.update(wifiBootButtonPressed(), now)) {
    Serial.println("BOOT held 10 s — clearing saved Wi-Fi");
    wifiResetCredentialsAndReboot();
  }
  // Let go after the countdown appeared but before the reset: show the info.
  const unsigned long held = s_hold.heldMs(now);
  if (held == 0 && s_last_held_ms >= config::kBootResetWarnMs) {
    s_info_pending = true;
  }
  s_last_held_ms = held;
}

bool bootButtonConsumeInfo() {
  const bool info = s_info_pending;
  s_info_pending = false;
  return info;
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect(unsigned long lost_at_ms) {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  const bool show_ui =
      lost_at_ms != 0 && millis() - lost_at_ms >= config::kWifiConnectingScreenDelayMs;
  s_lost_at_ms = lost_at_ms;
  const bool up = connectSavedNetwork(show_ui ? ConnectUi::kLost : ConnectUi::kNone);
  s_lost_at_ms = 0;
  return up;
}

void wifiShowLostScreen() {
  if (s_lost_screen_up) {
    return;
  }
  char ssid[33] = {};
  savedSsid(ssid, sizeof(ssid));
  statusScreenWifiLost(ssid);
  s_lost_screen_up = true;
  s_lost_tick_ms = millis();
}

bool wifiConsumeDisplaySettingsChanged() {
  const bool changed = s_display_settings_changed;
  s_display_settings_changed = false;
  return changed;
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    s_lost_screen_up = false;  // whatever comes next draws over it
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
    if (s_lost_screen_up && millis() - s_lost_tick_ms >= config::kWifiConnectingFrameMs) {
      s_lost_tick_ms = millis();
      statusScreenWifiLostTick();
      statusScreenHoldOverlay(bootButtonHeldMs());
    }
  }
  // Out here, not in the handler: the reset tears down the web server the
  // handler runs in, and the "Erasing" page has to reach the phone first.
  if (s_factory_reset_at != 0 && millis() - s_factory_reset_at >= kFactoryResetDelayMs) {
    factoryReset(s_factory_reset_keep_location);
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      return connectedNow();
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    return connectedNow();
  }

  if (storedWifiCredentials() && connectSavedNetwork(ConnectUi::kConnecting)) {
    return connectedNow();
  }

  if (storedWifiCredentials()) {
    // Out of reach, not wrong: keep the saved network and keep trying it, with
    // the hotspot open in between in case the radar has moved house.
    char ssid[33] = {};
    savedSsid(ssid, sizeof(ssid));
    Serial.printf("Saved WiFi '%s' out of reach - setup portal open, retrying every %lu s\n",
                  ssid, config::kSavedWifiRetryMs / 1000);
    for (;;) {
      if (openConfigPortal(ssid, passwordSuspect()) && wifiLinkUp()) {
        return connectedNow();
      }
      // A network picked on the hotspot that failed put the saved one back,
      // so this is still the network to retry.
      savedSsid(ssid, sizeof(ssid));
      Serial.printf("Retrying saved WiFi '%s'\n", ssid);
      if (connectSavedNetwork(ConnectUi::kConnecting)) {
        return connectedNow();
      }
    }
  }
  Serial.println("No saved WiFi — opening setup portal");

  if (openConfigPortal() && wifiLinkUp()) {
    return connectedNow();
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
