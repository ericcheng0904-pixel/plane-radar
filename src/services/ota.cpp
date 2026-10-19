#include "services/ota.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "ota_public_key.h"
#include "services/adsb_client.h"
#include "services/tls.h"
#include "util/release_version.h"

/**
 * Arduino marks every image valid before setup() unless this says otherwise;
 * a new image must earn it (checkTrial), or the bootloader rolls it back.
 */
extern "C" bool verifyRollbackLater() { return true; }

namespace services::ota {

namespace {

constexpr char kSite[] = "https://ericcheng0904-pixel.github.io/plane-radar/ota/";
constexpr unsigned long kMinute = 60UL * 1000UL;
constexpr unsigned long kHour = 60UL * kMinute;
/**
 * First look 2-5 min after every boot, then daily (the daily restart makes it
 * once a day anyway). Every boot: a release that is confirmed but crashes
 * every so often then still gets to fetch its own fix.
 */
constexpr unsigned long kFirstCheckMs = 2 * kMinute;
constexpr unsigned long kFirstCheckSpreadMs = 3 * kMinute;
constexpr unsigned long kCheckEveryMs = 24 * kHour;
constexpr unsigned long kCheckSpreadMs = 2 * kHour;
/**
 * Trial: a new image is confirmed once Wi-Fi, a drawn frame and a flight
 * fetch have happened AND it has run kConfirmAfterMs (one that crashes after
 * a few minutes must not get confirmed), and rolled back if that has not
 * happened kTrialMs after Wi-Fi first came up, or kTrialCapMs after boot
 * whatever happened (an image that cannot even join Wi-Fi sits in the setup
 * hotspot, where loop() never runs: a timer catches that).
 */
constexpr unsigned long kConfirmAfterMs = 15 * kMinute;
constexpr unsigned long kTrialMs = 20 * kMinute;
constexpr unsigned long kTrialCapMs = 40 * kMinute;
constexpr int kManifestMaxBytes = 1024;
constexpr size_t kSlotBytes = 0x1C0000;
/** Free heap to start with (docs/OTA.md): TLS, Update's buffer, margin. */
constexpr size_t kMinFreeHeap = 56 * 1024;
constexpr size_t kMaxHeapHint = 72 * 1024;
/** A version that failed its trial this often is not tried again. */
constexpr uint8_t kMaxTrials = 3;
constexpr uint32_t kReadTimeoutMs = 10000;
constexpr unsigned long kStallMs = 15000;
/**
 * The whole download must finish within this: the self-repair restarts a
 * radar whose fetch task has not tried a flight fetch for 180 s.
 */
constexpr unsigned long kDownloadMs = 150000;

volatile bool s_trial = false;
/** Download handshake with the main loop (wantsDisplay / displayReleased). */
volatile bool s_want_display = false;
volatile bool s_display_free = false;
unsigned long s_wifi_up_ms = 0;
esp_timer_handle_t s_trial_timer = nullptr;
volatile bool s_drawn = false;
volatile bool s_check_now = false;
/**
 * "Check now" waits this long: the button is pressed on the settings page,
 * and a TLS handshake started while that page is still being served can run
 * short of memory (seen as HTTP -1).
 */
constexpr unsigned long kCheckNowDelayMs = 15000;
volatile unsigned long s_check_now_at_ms = 0;
/** A trial failed this boot: wait for the next boot (≤ a day) to try again. */
bool s_hold_this_boot = false;
unsigned long s_next_check_ms = 0;
unsigned long s_last_check_ms = 0;

/** Written by the fetch task, read by the settings page. */
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
char s_status[112] = "not checked yet";

void setStatus(const char* text) {
  Serial.printf("ota: %s\n", text);
  taskENTER_CRITICAL(&s_lock);
  snprintf(s_status, sizeof(s_status), "%s", text);
  taskEXIT_CRITICAL(&s_lock);
}

struct Manifest {
  char version[24];
  uint32_t size;
  char sha256[65];
  uint8_t sig[80];
  size_t sig_len;
  size_t heap_hint;
};

bool isHex64(const char* s) {
  if (s == nullptr || std::strlen(s) != 64) {
    return false;
  }
  for (const char* p = s; *p != '\0'; ++p) {
    if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
      return false;
    }
  }
  return true;
}

/** Fetch and check the manifest. False (with a status) when there is nothing to do. */
bool fetchManifest(Manifest* m) {
  WiFiClientSecure client;
  services::tls::configure(client, services::tls::Host::kPages, kReadTimeoutMs);
  HTTPClient http;
  char url[96];
  snprintf(url, sizeof(url), "%smanifest.json", kSite);
  if (!http.begin(client, url)) {
    setStatus("could not check for updates");
    return false;
  }
  http.setReuse(false);
  http.setUserAgent(config::kUserAgent);
  http.setConnectTimeout(8000);
  http.setTimeout(kReadTimeoutMs);
  const int code = http.GET();
  services::tls::noteResult(client, services::tls::Host::kPages, code);
  const int length = http.getSize();
  if (code != HTTP_CODE_OK || length <= 0 || length > kManifestMaxBytes) {
    char text[64];
    snprintf(text, sizeof(text), "could not check for updates (HTTP %d)", code);
    http.end();
    setStatus(text);
    return false;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    setStatus("update manifest unreadable");
    return false;
  }
  const char* version = doc["version"] | "";
  const char* sha = doc["sha256"] | "";
  const char* sig = doc["sig"] | "";
  const uint32_t size = doc["size"] | 0u;
  const uint32_t hint = doc["min_free_heap_hint"] | 0u;
  if (std::strlen(version) >= sizeof(m->version) || !isHex64(sha) || size == 0 ||
      size > kSlotBytes) {
    setStatus("update manifest unreadable");
    return false;
  }
  snprintf(m->version, sizeof(m->version), "%s", version);
  snprintf(m->sha256, sizeof(m->sha256), "%s", sha);
  m->size = size;
  // Never trusted beyond reason: an absurd hint must not stop updates for good.
  m->heap_hint = std::min(kMaxHeapHint, std::max(kMinFreeHeap, static_cast<size_t>(hint)));
  if (mbedtls_base64_decode(m->sig, sizeof(m->sig), &m->sig_len,
                            reinterpret_cast<const unsigned char*>(sig), std::strlen(sig)) != 0 ||
      m->sig_len < 8 || m->sig_len > 72) {
    setStatus("update manifest unreadable");
    return false;
  }
  return true;
}

bool signatureValid(const uint8_t digest[32], const uint8_t* sig, size_t sig_len) {
  mbedtls_ecdsa_context ctx;
  mbedtls_ecdsa_init(&ctx);
  bool ok = mbedtls_ecp_group_load(&ctx.grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
            mbedtls_ecp_point_read_binary(&ctx.grp, &ctx.Q, config::kOtaPublicKey,
                                          sizeof(config::kOtaPublicKey)) == 0 &&
            mbedtls_ecp_check_pubkey(&ctx.grp, &ctx.Q) == 0 &&
            mbedtls_ecdsa_read_signature(&ctx, digest, 32, sig, sig_len) == 0;
  mbedtls_ecdsa_free(&ctx);
  return ok;
}

/** Remember an install, so the next boot can tell whether it survived its trial. */
void recordAttempt(const Manifest& m) {
  Preferences p;
  p.begin("ota", false);
  p.putString("try_ver", m.version);
  p.putString("try_sha", m.sha256);
  p.end();
}

/** One more strike against version + image under the given NVS keys; returns the count. */
uint8_t countFailure(const char* ver_key, const char* sha_key, const char* n_key,
                     const Manifest& m) {
  Preferences p;
  p.begin("ota", false);
  const bool again = p.getString(ver_key, "") == m.version && p.getString(sha_key, "") == m.sha256;
  const uint8_t n = again ? p.getUChar(n_key, 0) + 1 : 1;
  p.putString(ver_key, m.version);
  p.putString(sha_key, m.sha256);
  p.putUChar(n_key, n);
  p.end();
  return n;
}

/** Strikes recorded under the keys for exactly this version + image. */
uint8_t failures(Preferences& p, const char* ver_key, const char* sha_key, const char* n_key,
                 const Manifest& m) {
  return p.getString(ver_key, "") == m.version && p.getString(sha_key, "") == m.sha256
             ? p.getUChar(n_key, 0)
             : 0;
}

/**
 * Ask the main loop for the frame buffer's 57 KB: a TLS session, Update's
 * buffer and the radar's own needs do not fit together otherwise. False if
 * the loop did not hand it over in time.
 */
bool takeDisplay() {
  s_want_display = true;
  const unsigned long start = millis();
  while (!s_display_free) {
    if (millis() - start > 3000) {
      s_want_display = false;
      return false;
    }
    delay(20);
  }
  return true;
}

/**
 * The screen is gone for this boot once the download has started: restart,
 * and skip updates on the next boot so a link that keeps failing cannot turn
 * into a restart every few minutes.
 */
[[noreturn]] void failAndRestart(const char* why) {
  setStatus(why);
  Preferences p;
  p.begin("ota", false);
  p.putBool("skip_boot", true);
  p.end();
  delay(300);
  ESP.restart();
  for (;;) {
  }
}

/** Download, verify and install. Restarts either way once it has the screen. */
bool install(const Manifest& m) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (next == nullptr || next == running || next->size < m.size) {
    setStatus("no room for an update on this radar");
    return false;
  }
  if (!takeDisplay()) {
    delay(200);
    if (s_display_free) {
      // Handed over just as we gave up: the screen is gone all the same.
      failAndRestart("update postponed: display busy");
    }
    setStatus("update postponed: display busy");
    return false;
  }
  // Judged now, with the frame buffer handed back and the main loop quiet:
  // before, a settings page being served could make room look short.
  if (ESP.getFreeHeap() < m.heap_hint || !services::tls::enoughMemory()) {
    failAndRestart("update postponed: not enough memory");
  }
  WiFiClientSecure client;
  services::tls::configure(client, services::tls::Host::kPages, kReadTimeoutMs);
  HTTPClient http;
  char url[112];
  snprintf(url, sizeof(url), "%sfirmware-%s.bin", kSite, m.version);
  if (!http.begin(client, url)) {
    failAndRestart("update download failed");
  }
  http.setReuse(false);
  http.setUserAgent(config::kUserAgent);
  http.setConnectTimeout(8000);
  http.setTimeout(kReadTimeoutMs);
  const int code = http.GET();
  services::tls::noteResult(client, services::tls::Host::kPages, code);
  if (code != HTTP_CODE_OK || http.getSize() != static_cast<int>(m.size)) {
    http.end();
    failAndRestart("update download failed");
  }
  if (!Update.begin(m.size, U_FLASH)) {
    http.end();
    failAndRestart("update could not start");
  }

  // The image must carry its own version in its User-Agent: a signed image
  // offered under another version's name is refused.
  char marker[96];
  const int marker_len =
      snprintf(marker, sizeof(marker), "PlaneRadar/%s (+%s)", m.version, config::kRepoUrl);
  int matched = 0;
  bool marker_seen = false;

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  constexpr size_t kBufBytes = 4096;  // from the heap the frame buffer gave back
  auto* buf = static_cast<uint8_t*>(malloc(kBufBytes));
  if (buf == nullptr) {
    http.end();
    Update.abort();
    failAndRestart("update could not start");
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  const unsigned long started = millis();
  unsigned long last_data = started;
  bool failed = false;
  while (got < static_cast<size_t>(m.size)) {
    if (millis() - started > kDownloadMs) {
      failed = true;
      break;
    }
    const int avail = stream->available();
    if (avail <= 0) {
      if (!stream->connected() || millis() - last_data > kStallMs) {
        failed = true;
        break;
      }
      delay(5);
      continue;
    }
    const size_t want = std::min({static_cast<size_t>(avail), kBufBytes,
                                   static_cast<size_t>(m.size) - got});
    // read(buf, n), not readBytes(): that goes a byte at a time through TLS.
    const int got_now = stream->read(buf, want);
    if (got_now <= 0) {
      continue;
    }
    const size_t n = static_cast<size_t>(got_now);
    mbedtls_sha256_update_ret(&sha, buf, n);
    for (size_t i = 0; i < n && !marker_seen; ++i) {
      // The marker's first character does not recur in it, so a mismatch can
      // restart the match at this byte without missing an overlap.
      matched = buf[i] == static_cast<uint8_t>(marker[matched])
                    ? matched + 1
                    : (buf[i] == static_cast<uint8_t>(marker[0]) ? 1 : 0);
      marker_seen = matched == marker_len;
    }
    if (Update.write(buf, n) != n) {
      failed = true;
      break;
    }
    got += n;
    last_data = millis();
  }
  http.end();  // give the TLS session's memory back before verifying
  free(buf);
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);

  char hex[65];
  for (int i = 0; i < 32; ++i) {
    snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  }
  const char* why = nullptr;
  bool bad_image = false;  // as published, not a bad connection
  if (failed || got != static_cast<size_t>(m.size) || !Update.isFinished()) {
    why = "update download failed";
  } else if (std::strcmp(hex, m.sha256) != 0) {
    why = "update download damaged";
    bad_image = true;
  } else if (!signatureValid(digest, m.sig, m.sig_len)) {
    why = "update refused: bad signature";
    bad_image = true;
  } else if (!marker_seen) {
    why = "update refused: wrong version inside";
    bad_image = true;
  }
  if (why != nullptr) {
    Update.abort();  // the slot keeps no first bytes, so it can never boot
    // Not again this boot (the next is at most a day away); and a release
    // that keeps failing its checks is dropped after kMaxTrials.
    s_hold_this_boot = true;
    if (bad_image) {
      countFailure("vfail_ver", "vfail_sha", "vfails", m);
    }
    failAndRestart(why);
  }
  // Recorded before the boot slot changes: a power cut right after it must
  // still leave the next boot able to tell how this install went.
  recordAttempt(m);
  if (!Update.end(true)) {
    Preferences p;
    p.begin("ota", false);
    p.remove("try_ver");
    p.remove("try_sha");
    p.end();
    failAndRestart("update could not be finished");
  }
  char text[64];
  snprintf(text, sizeof(text), "installed %s - restarting", m.version);
  setStatus(text);
  delay(500);
  ESP.restart();
  return true;
}

void check() {
  Manifest m{};
  if (!fetchManifest(&m)) {
    return;
  }
  s_last_check_ms = millis();
  if (!util::release_version::shouldInstall(config::kFirmwareVersion, m.version)) {
    setStatus("up to date");
    return;
  }
  Preferences p;
  p.begin("ota", true);
  const uint8_t fails = failures(p, "fail_ver", "fail_sha", "fails", m);
  const uint8_t bad = failures(p, "vfail_ver", "vfail_sha", "vfails", m);
  p.end();
  char text[96];
  if (bad >= kMaxTrials) {
    snprintf(text, sizeof(text), "%s did not pass its checks %u times - not tried again",
             m.version, static_cast<unsigned>(bad));
    setStatus(text);
    return;
  }
  if (fails >= kMaxTrials) {
    snprintf(text, sizeof(text), "%s failed on this radar %u times - not tried again",
             m.version, static_cast<unsigned>(fails));
    setStatus(text);
    return;
  }
  if (s_hold_this_boot) {
    snprintf(text, sizeof(text), "%s available - trying again tomorrow", m.version);
    setStatus(text);
    return;
  }
  snprintf(text, sizeof(text), "installing %s", m.version);
  setStatus(text);
  install(m);
}

/** Give up on a trial image: back to the one before it. */
void rollBack(const char* why) {
  Serial.printf("ota: new firmware %s - rolling back\n", why);
  esp_ota_mark_app_invalid_rollback_and_reboot();
  // Only returns if it could not (no image to go back to): restart anyway;
  // the bootloader then drops an image still on trial.
  esp_restart();
}

void onTrialTimeout(void*) {
  if (s_trial) {
    rollBack("was not confirmed in time");
  }
}

}  // namespace

void begin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    s_trial = true;
    setStatus("new firmware on trial");
    const esp_timer_create_args_t args = {onTrialTimeout, nullptr, ESP_TIMER_TASK, "ota_trial", false};
    if (esp_timer_create(&args, &s_trial_timer) == ESP_OK) {
      esp_timer_start_once(s_trial_timer, static_cast<uint64_t>(kTrialCapMs) * 1000ULL);
    }
  } else {
    // A confirmed image (or one flashed over USB): settle the last install.
    Preferences p;
    p.begin("ota", false);
    if (p.getBool("skip_boot", false)) {
      p.remove("skip_boot");
      s_hold_this_boot = true;
      setStatus("the last update attempt failed - trying again tomorrow");
    }
    const String tried = p.getString("try_ver", "");
    if (tried.length() > 0) {
      const String sha = p.getString("try_sha", "");
      if (tried == config::kFirmwareVersion) {
        p.remove("fail_ver");
        p.remove("fail_sha");
        p.remove("fails");
      } else {
        // The update rolled back. Count it against that version and image;
        // one failure may just be someone unplugging the radar mid-trial.
        const bool again = p.getString("fail_ver", "") == tried && p.getString("fail_sha", "") == sha;
        const uint8_t fails = again ? p.getUChar("fails", 0) + 1 : 1;
        p.putString("fail_ver", tried);
        p.putString("fail_sha", sha);
        p.putUChar("fails", fails);
        s_hold_this_boot = true;
        char text[96];
        snprintf(text, sizeof(text), "%s did not pass its trial (%u time%s) - back on %s",
                 tried.c_str(), static_cast<unsigned>(fails), fails == 1 ? "" : "s",
                 config::kFirmwareVersion);
        setStatus(text);
      }
      p.remove("try_ver");
      p.remove("try_sha");
    }
    p.end();
  }
  s_next_check_ms = kFirstCheckMs + esp_random() % kFirstCheckSpreadMs;
}

bool runIfDue() {
  if (s_trial || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  const unsigned long now = millis();
  const bool asked = s_check_now && static_cast<long>(now - s_check_now_at_ms) >= 0;
  if (!asked && static_cast<long>(now - s_next_check_ms) < 0) {
    return false;
  }
  s_check_now = false;
  s_next_check_ms = now + kCheckEveryMs - kCheckSpreadMs + esp_random() % (2 * kCheckSpreadMs);
  if (config::kOtaPublicKeyIsPlaceholder) {
    setStatus("updates off: this build has no signing key");
    return false;
  }
  util::release_version::Version v{};
  if (!util::release_version::parse(config::kFirmwareVersion, v)) {
    setStatus("updates off: development build");
    return false;
  }
  check();
  return true;
}

void noteRadarDrawn() { s_drawn = true; }

void checkTrial() {
  if (!s_trial) {
    return;
  }
  const unsigned long now = millis();
  if (WiFi.status() == WL_CONNECTED && s_wifi_up_ms == 0) {
    s_wifi_up_ms = now | 1UL;
  }
  if (WiFi.status() == WL_CONNECTED && s_drawn && services::adsb::lastUpdateMs() != 0 &&
      now >= kConfirmAfterMs) {
    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
      return;  // try again next pass; the trial deadline still applies
    }
    s_trial = false;
    if (s_trial_timer != nullptr) {
      esp_timer_stop(s_trial_timer);
    }
    Preferences p;
    p.begin("ota", false);
    p.remove("try_ver");
    p.remove("try_sha");
    p.remove("fail_ver");
    p.remove("fail_sha");
    p.remove("fails");
    p.end();
    char text[64];
    snprintf(text, sizeof(text), "updated to %s", config::kFirmwareVersion);
    setStatus(text);
    return;
  }
  if (s_wifi_up_ms != 0 && now - s_wifi_up_ms > kTrialMs) {
    rollBack("did not get going");
  }
}

void requestCheck() {
  s_check_now_at_ms = millis() + kCheckNowDelayMs;
  s_check_now = true;
}

bool wantsDisplay() { return s_want_display; }

void displayReleased() { s_display_free = true; }

void statusText(char* out, size_t n) {
  if (n == 0) {
    return;
  }
  char status[sizeof(s_status)];
  taskENTER_CRITICAL(&s_lock);
  memcpy(status, s_status, sizeof(status));
  taskEXIT_CRITICAL(&s_lock);
  if (s_last_check_ms != 0) {
    const unsigned long h = (millis() - s_last_check_ms) / kHour;
    snprintf(out, n, "%s (checked %lu h ago)", status, h);
  } else {
    snprintf(out, n, "%s", status);
  }
}

}  // namespace services::ota
