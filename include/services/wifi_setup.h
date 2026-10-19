#pragma once

/**
 * WPA2 password for the setup access point, derived from this device's MAC so
 * it is stable across reboots and can be printed on the setup screen. An open
 * AP would let anyone in radio range hand the radar a network of their choice.
 */
const char* wifiSetupApPassword();

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/**
 * Reconnect using saved creds; never opens the captive portal.
 *
 * @param lost_at_ms millis() when the link dropped. Once the outage passes
 *        config::kWifiConnectingScreenDelayMs — before or during the attempt —
 *        the "Wi-Fi lost" screen replaces the radar. Short drops are better
 *        ridden out with the last picture still on screen.
 */
bool wifiReconnect(unsigned long lost_at_ms);
/**
 * Replace the radar with "Wi-Fi lost / <network> / Trying again..." until the
 * link is back; wifiLoop() keeps its spinner turning. Idempotent.
 */
void wifiShowLostScreen();
/** Keeps the LAN config portal alive; call every loop() iteration. */
void wifiLoop();
/** Latched: a portal save changed a display option, so the radar needs a redraw. */
bool wifiConsumeDisplaySettingsChanged();
bool wifiBootButtonPressed();
/** GPIO + interrupt setup; call once early in setup(). */
void bootButtonInit();
/** Latched short tap (survives blocking HTTP/display work). */
bool bootButtonConsumeTap();
/** Call each loop iteration; triggers WiFi reset on long hold. */
void bootButtonPollLongPress();
/** How long BOOT has been held continuously, as bootButtonPollLongPress()
 *  measures it; 0 when it is up. For the on-screen countdown. */
unsigned long bootButtonHeldMs();

/**
 * True once after BOOT was held at least config::kBootResetWarnMs and let go
 * before the Wi-Fi reset: the owner asked for the info screen.
 */
bool bootButtonConsumeInfo();
