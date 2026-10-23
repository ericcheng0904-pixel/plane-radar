#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Small pieces of the guided first-time setup that can be checked without a
 * board: what a failed join means, what the Wi-Fi QR code says, which network
 * names and passwords are worth trying, text made safe for a page, and when the
 * welcome that follows setup goes. Free of Arduino headers so the host test
 * suite can cover it (test/test_setup).
 */
namespace util::setup {

/** What to tell the person whose network the radar could not join. */
enum class JoinFailure : uint8_t {
  kNone,           // no reason recorded (timed out, or no address from the router)
  kWrongPassword,  // "Check the password"
  kNotFound,       // "Use a 2.4 GHz network": the radar cannot see 5 GHz
  kOther,
};

/**
 * From the station's last disconnect reason (ESP-IDF wifi_err_reason_t):
 * 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE_TIMEOUT, 202 AUTH_FAIL and 204
 * HANDSHAKE_TIMEOUT are what a wrong password produces, depending on the
 * router; 201 NO_AP_FOUND is a name the radar cannot see. 0 = none recorded.
 */
JoinFailure classifyDisconnect(uint8_t reason);

/**
 * "WIFI:T:WPA;S:<ssid>;P:<password>;;" — the text a phone camera turns into a
 * "Join network" prompt. \ ; , : and " are escaped as the format requires.
 * False, with `out` emptied, if it does not fit.
 */
bool wifiQrText(const char* ssid, const char* password, char* out, size_t len);

/** 1 to 32 bytes: what a network name can be. */
bool plausibleSsid(const char* ssid);

/**
 * Empty (an open network), 8 to 63 characters, or 64 hex digits: anything
 * else no WPA2 router accepts, and is better refused on the phone than after
 * twenty seconds of trying.
 */
bool plausiblePassword(const char* password);

/**
 * `in` for an HTML page: & < > " ' become entities. Stops before an entity
 * or character that does not fit; always terminates.
 */
void htmlEscape(const char* in, char* out, size_t len);

/**
 * What htmlEscape() writes for `c`: its entity ("&amp;"), or nullptr for a
 * character that stands for itself. For escaping straight onto a page,
 * without a buffer as long as the escaped text.
 */
const char* htmlEntity(char c);

/**
 * `in` for markup that a script puts on the page from a single-quoted string
 * (the hotspot's step header): as htmlEscape, plus \ as an entity and control
 * characters left out, so the text can end neither the string nor the line.
 */
void htmlEscapeInScript(const char* in, char* out, size_t len);

/** Why the one-time welcome screen goes, or kNotYet while it stays. */
enum class WelcomeEnd : uint8_t {
  kNotYet,
  kTapped,       // a tap on the button skips it
  kGuideOpened,  // the guide it points to was opened: it has done its job
  kTimedOut,
};

/**
 * The welcome, shown in place of the radar once for a new owner, ends on a
 * tap, when its guide page is opened, or `show_ms` after it first appeared at
 * `shown_at_ms` (millis(), across a wrap).
 */
WelcomeEnd welcomeEnd(unsigned long shown_at_ms, unsigned long now_ms, bool tapped,
                      bool guide_opened, unsigned long show_ms);

}  // namespace util::setup
