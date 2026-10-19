#include "services/device_health.h"

#include <Arduino.h>
#include <WiFi.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "util/health.h"

namespace services::health {

namespace {

// Survives esp_restart() (not a power cut): why we restarted ourselves.
constexpr uint32_t kRtcMagic = 0x52414452;  // "RADR"
RTC_NOINIT_ATTR uint32_t s_rtc_magic;
RTC_NOINIT_ATTR uint8_t s_rtc_reason;

const char* s_last_restart = "power on";

volatile unsigned long s_heartbeat_ms = 0;
unsigned long s_wifi_up_since_ms = 0;
/** When the main loop first saw the link down; 0 = up. */
unsigned long s_wifi_down_since_ms = 0;
bool s_setup_pending = false;
/** When a location was first saved after setup step 2 had been waiting for one. */
unsigned long s_setup_done_ms = 0;
unsigned long s_low_memory_since_ms = 0;
unsigned long s_last_check_ms = 0;

[[noreturn]] void restart(util::health::Reason reason) {
  Serial.printf("health: restarting - %s\n", util::health::reasonText(reason));
  s_rtc_magic = kRtcMagic;
  s_rtc_reason = static_cast<uint8_t>(reason);
  delay(200);  // let the log line out
  esp_restart();
}

const char* resetReasonText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power on";
    case ESP_RST_PANIC:
      return "crash";
    case ESP_RST_BROWNOUT:
      return "power dip";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_SW:
      return "restart";
    default:
      return "reset";
  }
}

}  // namespace

void begin() {
  const esp_reset_reason_t hw = esp_reset_reason();
  if (hw == ESP_RST_SW && s_rtc_magic == kRtcMagic) {
    s_last_restart = util::health::reasonText(static_cast<util::health::Reason>(s_rtc_reason));
  } else {
    s_last_restart = resetReasonText(hw);
  }
  s_rtc_magic = 0;
  Serial.printf("Last restart: %s\n", s_last_restart);
}

void heartbeat() { s_heartbeat_ms = millis(); }

void checkFromLoop() {
  const unsigned long now = millis();
  heartbeat();

  if (WiFi.status() == WL_CONNECTED) {
    if (s_wifi_up_since_ms == 0) {
      s_wifi_up_since_ms = now;
    }
    s_wifi_down_since_ms = 0;
  } else {
    s_wifi_up_since_ms = 0;
    if (s_wifi_down_since_ms == 0) {
      s_wifi_down_since_ms = now != 0 ? now : 1;
    }
  }

  if (now - s_last_check_ms < 1000) {
    return;
  }
  s_last_check_ms = now;

  const size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  util::health::State s;
  s.now_ms = now;
  s.loop_heartbeat_ms = 0;  // the loop cannot judge itself; see checkFromFetchTask
  s.wifi_up_since_ms = s_wifi_up_since_ms;
  s.last_attempt_ms = adsb::lastAttemptMs();
  s.last_success_ms = adsb::lastUpdateMs();
  s.feeds_resting = adsb::allFeedsResting();
  s.largest_free_block = block;
  if (block < util::health::Limits{}.low_memory_block) {
    if (s_low_memory_since_ms == 0) {
      s_low_memory_since_ms = now;
    }
  } else {
    s_low_memory_since_ms = 0;
  }
  s.low_memory_since_ms = s_low_memory_since_ms;
  const bool pending = !location::isSet();
  if (s_setup_pending && !pending) {
    s_setup_done_ms = now;
  }
  s_setup_pending = pending;
  s.setup_pending = pending;
  s.setup_done_ms = s_setup_done_ms;
  s.wifi_down_since_ms = s_wifi_down_since_ms;

  const util::health::Reason r = util::health::check(s);
  if (r != util::health::Reason::kNone) {
    restart(r);
  }
}

void checkFromFetchTask() {
  util::health::State s;
  s.now_ms = millis();
  s.loop_heartbeat_ms = s_heartbeat_ms;
  // Only the loop rule applies from here; the rest is the loop's job.
  if (util::health::check(s) == util::health::Reason::kLoopStalled) {
    restart(util::health::Reason::kLoopStalled);
  }
}

const char* lastRestartText() { return s_last_restart; }

}  // namespace services::health
