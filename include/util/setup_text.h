#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Small pieces of the guided first-time setup that can be checked without a
 * board: what a failed join means, what the Wi-Fi QR code says, which network
 * names and passwords are worth trying, and text made safe for a page. Free of
 * Arduino headers so the host test suite can cover it (test/test_setup).
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

}  // namespace util::setup
