#pragma once

#include <WiFiClientSecure.h>

#include <cstddef>
#include <cstdint>

/**
 * HTTPS for the fetch task. The ADS-B feeds stopped answering plain HTTP in
 * October 2026, so every request that leaves the house is now TLS.
 *
 * Servers are checked against Mozilla's root list, built into the firmware
 * (scripts/build_cert_bundle.py). The radar has to keep working for years
 * without anyone updating it, and the feeds may move to a root that list does
 * not have yet: after kVerifyFailLimit handshakes with one server in a row fail
 * only because its certificate could not be verified, it stops checking that
 * server and says so on the settings page. The data is public and nothing secret is sent, so a radar
 * that still works beats one that is correct and dark.
 *
 * One connection at a time: a handshake needs ~45 KB, and the C3 has room for
 * one. Fetch task only.
 */
namespace services::tls {

/** The servers reached over HTTPS. Each keeps its own certificate record. */
enum class Host : uint8_t { kAdsbFi, kAdsbLol, kAdsbIm, kOpenMeteo, kPages };

/**
 * Prepare `client` for a request to `host`: trusted roots (or none, see
 * above), handshake timeout, and `read_timeout_ms` between received bytes.
 */
void configure(WiFiClientSecure& client, Host host, uint32_t read_timeout_ms);

/**
 * Report how a request to `host` went: `http_code` as HTTPClient returned it.
 * Only a handshake that failed because the certificate was rejected counts
 * toward giving up on checking that server.
 */
void noteResult(WiFiClientSecure& client, Host host, int http_code);

/** False once the radar has given up checking `host`'s certificates. */
bool verifying(Host host);

/**
 * Whether a handshake can be afforded right now. Starting one without the
 * room ends in an allocation failure half way through, so the caller skips
 * the request instead.
 */
bool enoughMemory();

}  // namespace services::tls
