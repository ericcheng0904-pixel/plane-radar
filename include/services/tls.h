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
 * one. Its two 16.7 KB record buffers are reserved once, as the radar starts
 * (reserveRecordBuffers()), and lent to every session. Fetch task only, but
 * for reserveRecordBuffers().
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
 * @return true when the handshake failed for lack of memory on this radar:
 *         not the server's fault, so no reason to try another one.
 */
bool noteResult(WiFiClientSecure& client, Host host, int http_code);

/**
 * A request not sent because enoughMemory() said no: it counts as an attempt
 * that failed for lack of memory, like a handshake that did not fit. A heap
 * too cut up for any handshake then ends in the low-memory restart
 * (util::health::Limits::tls_no_room), not in skips until the 30-minute
 * no-data one.
 */
void noteNoRoom();

/**
 * The request just reported to noteResult() got through the handshake, but
 * there was no room to read its answer: it counts as an attempt that failed
 * for lack of memory, as if noteResult() had not ended the run of them.
 */
void noteNoRoomForAnswer();

/** HTTPS attempts in a row, to any server, that failed for lack of memory. */
uint8_t noRoomInARow();

/** False once the radar has given up checking `host`'s certificates. */
bool verifying(Host host);

/**
 * Main loop, as the radar starts, once the frame buffer is taken: from the
 * next HTTPS session on, mbedTLS's two record buffers (2 x 16.7 KB) are set
 * aside for good and lent to every session, instead of being found anew each
 * time in a heap the radar's running cuts into smaller pieces. The fetch task
 * takes them, before that session; the frame buffer comes first because it
 * needs the larger block. Without room for them, sessions take the buffers
 * from the heap, as before, and each later one tries again.
 */
void reserveRecordBuffers();

/**
 * HTTPS and the web pages take turns. A session needs 12-15 KB of heap
 * besides the reserved buffers and a page being sent ~15 KB more (lwIP's send
 * buffer and Wi-Fi's copies of its frames): the two at once left ~1 KB free in
 * a test that loaded pages during fetches, too little for Wi-Fi, and pages
 * came out cut short. The fetch task holds the turn for each HTTPS session
 * (with an update check or a town search, for its whole pass), and the main
 * loop serves pages only when it can have it: a page request waits in the
 * backlog for one session at most (~0.5-2 s). A session that has held it for
 * 3 s is waiting out a timeout, not streaming an answer, and pages are served
 * beside it rather than leave a browser waiting up to half a minute.
 *
 * Fetch task, before a session: waits up to 1 s for a page being sent to
 * finish, then goes ahead anyway (a slow client must not hold the radar's
 * flights up), and up to 0.5 s more for the page's send buffers to come back.
 */
void beginSessions();
/** Fetch task, after the session. */
void endSessions();
/** Main loop: whether web pages may be served now. If so, endPages() after. */
bool beginPages();
void endPages();

/**
 * Whether a handshake can be afforded right now. Starting one without the
 * room ends in an allocation failure half way through, so the caller skips
 * the request instead.
 */
bool enoughMemory();

}  // namespace services::tls
