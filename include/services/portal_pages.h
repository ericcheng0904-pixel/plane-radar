#pragma once

#include <WebServer.h>

/**
 * The firmware's own pages inside the Wi-Fi portal.
 *
 * WiFiManager builds each page by concatenating one Arduino String and sends it
 * in a single response. Arduino's String::concat fails silently when the heap
 * cannot grow the buffer, so once the settings form outgrew the largest free
 * block the page simply stopped mid-way — no error, no Save button. The 240x240
 * frame sprite alone holds 115 KB, so that block is not coming back.
 *
 * The settings page here is written straight to the socket in small chunks
 * instead, so the memory it needs does not depend on how long the page is.
 *
 * Two sets of routes, for the two ways a phone reaches the radar: the setup
 * hotspot (step 1: only the Wi-Fi form is offered) and the home network
 * (step 2 and everything after it).
 */
namespace services::portal {

/** What the pages hand back to the Wi-Fi side (wifi_setup.cpp). */
struct Hooks {
  /** After /settingssave stored the display settings. */
  void (*on_settings_saved)() = nullptr;
  /**
   * "Reset for a new owner" was confirmed; `keep_location` when the giver
   * asked to keep where the radar is. The reply is already on its way; the
   * reset itself happens from the caller's loop, so it gets there.
   */
  void (*on_factory_reset)(bool keep_location) = nullptr;
  /** Hotspot: a network and a plausible password were submitted. */
  void (*on_join)(const char* ssid, const char* password) = nullptr;
};

/**
 * Markup injected into the <head> of WiFiManager's own pages on the home
 * network: the shared styles plus the script that prepends the navigation
 * bar. Our pages emit the same navigation directly and do not need the script.
 */
const char* headHtml();

/**
 * The same for the setup hotspot: a "Step 1 of 2" header (plain "Wi-Fi
 * setup" when `location_set`, since then Wi-Fi is the only step), the SSID
 * field labelled "Network", WiFiManager's status box hidden and no
 * navigation, so the Wi-Fi form is all there is. Under ~1.5 KB: WiFiManager
 * adds it to every page it builds as one String.
 */
const char* hotspotHeadHtml(bool location_set);

/**
 * Home network: /settings with /settingssave, /locationsave and /reset,
 * /screen.bmp, the /param redirect, and /wifi (with /0wifi and /wifisave)
 * replaced by how to change networks — saving there would keep credentials
 * nobody had checked. Call from WiFiManager's web-server callback, which runs
 * before the library registers its own routes: the ESP32 web server
 * dispatches to the first handler that matches, so these shadow the
 * library's for good.
 */
void registerLanRoutes(WebServer& server, const Hooks& hooks);

/**
 * Setup hotspot: / goes straight to the Wi-Fi form, and /wifisave is
 * answered here (hooks.on_join) instead of by WiFiManager, which would store
 * the password before knowing it works and then block for a minute.
 */
void registerHotspotRoutes(WebServer& server, const Hooks& hooks);

}  // namespace services::portal
