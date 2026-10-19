#pragma once

#include <cstdint>

#include <driver/gpio.h>

// Set at build time from `git describe` (scripts/version.py).
#ifndef PLANE_RADAR_VERSION
#define PLANE_RADAR_VERSION "dev"
#endif
#define PLANE_RADAR_REPO_URL "https://github.com/ericcheng0904-pixel/plane-radar"

namespace config {

// --- Identity ---
constexpr char kFirmwareVersion[] = PLANE_RADAR_VERSION;
/** Where this firmware lives: the settings page, the User-Agent, updates. */
constexpr char kRepoUrl[] = PLANE_RADAR_REPO_URL;
/**
 * Sent with every request. The feeds ask clients to say who they are and how
 * to reach them (adsb.lol answers 403 to HTTPClient's default
 * "ESP32HTTPClient"), and a name of our own keeps a block aimed at another
 * project's devices from hitting this one.
 */
constexpr char kUserAgent[] = "PlaneRadar/" PLANE_RADAR_VERSION " (+" PLANE_RADAR_REPO_URL ")";

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
/** With a saved network out of reach, retry it after the hotspot has sat unused this long. */
constexpr unsigned long kSavedWifiRetryMs = 180000;
/**
 * A phone on the hotspot holds off those retries (someone may be setting the
 * radar up), but only this long: a phone that once joined the hotspot rejoins
 * it by itself whenever the home Wi-Fi drops, and must not keep the radar off
 * a network that has come back.
 */
constexpr unsigned long kSavedWifiRetryHoldMaxMs = 900000;
constexpr unsigned long kWifiConnectingFrameMs = 50;
/**
 * Wait after a disconnect before reconnecting actively. The ESP32's own
 * auto-reconnect usually restores the link inside a few seconds, and jumping in
 * earlier only tears down a connection that was about to come back.
 */
constexpr unsigned long kWifiDownGraceMs = 10000;
/**
 * Keep the radar on screen for this long into an outage. Only a drop that
 * outlasts it is worth replacing the picture with the connecting animation.
 */
constexpr unsigned long kWifiConnectingScreenDelayMs = 25000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- BOOT button (ESP32-C3 Super Mini, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
/**
 * Holding BOOT this long clears the saved Wi-Fi. It was 3 s and wiped Wi-Fi,
 * location and settings; the button sits where the radar is gripped to swap
 * its cable, so handling it was enough. Now it takes a deliberate 10 s, shows a
 * countdown after kBootResetWarnMs, and keeps location and settings.
 */
constexpr unsigned long kBootResetHoldMs = 10000UL;
/** The on-screen "keep holding" countdown appears after this long. */
constexpr unsigned long kBootResetWarnMs = 2000UL;
/** A hold only counts while the button is watched: a longer gap between
 *  samples (the loop was busy) restarts it. */
constexpr unsigned long kBootPollGapMs = 400UL;
/** Presses in the first seconds after power-up are ignored: that is when the
 *  radar is being handled to plug it in. */
constexpr unsigned long kBootIgnoreAfterPowerUpMs = 5000UL;
/** Ignore BOOT taps shorter than this (debounce). */
constexpr unsigned long kBootTapMinMs = 40UL;
/** A press longer than this is not a tap, so it does not change the range. */
constexpr unsigned long kBootTapMaxMs = 1500UL;

// --- Power ---
/**
 * Core clock. 160 MHz is the ESP32-C3 maximum and what the firmware runs at.
 * Halving it to 80 MHz was tried as a heat lever and measured no difference on
 * the board (still ~70 C), so the setting was removed rather than kept as a
 * control that costs responsiveness and buys nothing.
 */
constexpr uint32_t kCpuFreqFullMhz = 160;
/** How often the chip temperature goes to the serial log. */
constexpr unsigned long kTempLogIntervalMs = 60000;

// --- Display: GC9A01 1.28" round 240×240 (SPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_0;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_1;
constexpr gpio_num_t kDisplayPinDc = GPIO_NUM_10;
constexpr gpio_num_t kDisplayPinMosi = GPIO_NUM_3;  // display SDA
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_4;  // display SCL

constexpr int kDisplayWidth = 240;
constexpr int kDisplayHeight = 240;

constexpr uint32_t kDisplaySpiWriteHz = 40000000;
// GC9A01 modules often need invert + BGR for correct black/green output
constexpr bool kDisplayInvert = true;
constexpr bool kDisplayRgbOrder = true;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Poll adsb.lol. It publishes no limit, but at 5 s about half the requests
 *  came back 429; ~10 s is what it sustains per IP. Dead reckoning hides the
 *  gap between fixes. */
constexpr unsigned long kAdsbFetchIntervalMs = 10000;
/** On HTTP 429 the interval doubles, up to this, until a fetch succeeds. */
constexpr unsigned long kAdsbMaxBackoffMs = 60000;
/** Fetch task stack. Measured peak use is ~6.5 KB (plain HTTP + filtered JSON);
 *  it was 16 KB, a contiguous block the heap does not always have. */
constexpr uint32_t kAdsbTaskStackBytes = 12288;
/** "No flight data" shows on the radar after this long without a good fetch. */
constexpr unsigned long kNoDataNoticeMs = 60000;
/** Dead reckoning never extrapolates a fix further than this. */
constexpr unsigned long kDeadReckonMaxMs = 2 * kAdsbFetchIntervalMs;
/** With no successful fetch for this long the radar shows no aircraft at all. */
constexpr unsigned long kAircraftStaleMs = 90000;
/** Redraw cadence; aircraft are dead-reckoned along their track/speed so motion
 *  is smooth. ~4 Hz = 250 ms (panel scanout caps at ~24 Hz; the full-frame
 *  recompose+present is the practical limit ~10-15 Hz). */
constexpr unsigned long kRadarRedrawIntervalMs = 250;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
