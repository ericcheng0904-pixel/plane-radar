#include "services/tls.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/platform.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_internal.h>
#include <mbedtls/x509.h>

#include <cstring>

#include "util/health.h"
#include "util/record_slots.h"

// mbedTLS's own allocator in ESP-IDF (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC: the
// internal heap). Its header, esp_mem.h, declares it without extern "C", so
// the C symbols are named here.
void* idfMbedtlsCalloc(size_t n, size_t size) asm("esp_mbedtls_mem_calloc");
void idfMbedtlsFree(void* ptr) asm("esp_mbedtls_mem_free");

/** Mozilla's roots, embedded by platformio.ini (board_build.embed_files). */
extern const uint8_t kCertBundle[] asm("_binary_data_cert_x509_crt_bundle_bin_start");

namespace services::tls {

namespace {

/** Consecutive verification failures before a server's certificates stop being checked. */
constexpr int kVerifyFailLimit = 6;
constexpr const char* kHostNames[] = {"adsb.fi", "adsb.lol", "adsb.im",
                                     "geocoding-api.open-meteo.com", "github.io"};
constexpr size_t kHostCount = sizeof(kHostNames) / sizeof(kHostNames[0]);
/** The default is 120 s; a stalled handshake should not hold up the radar. */
constexpr unsigned long kHandshakeTimeoutS = 12;
/**
 * mbedTLS allocates a 16.7 KB record buffer each way plus ~10 KB during the
 * handshake; below these the attempt would fail part way through. Only while
 * the buffers are not reserved (reserveRecordBuffers()).
 */
constexpr size_t kMinLargestBlock = 20 * 1024;
constexpr size_t kMinFreeHeap = 48 * 1024;
/**
 * With both buffers reserved, the rest of a session takes 12-15 KB (the
 * handshake's working memory, the server's certificates, the TLS client's
 * state and HTTPClient's), and Wi-Fi needs ~10 KB beside it for the frames
 * coming in (util::heap_room::kReserve). In fault tests a session begun with
 * 24.5 KB free left ~8 KB and its connection timed out; begun with 26-31 KB,
 * every one went through. Its largest blocks are the TLS client's state
 * (2.2 KB) and one of mbedTLS's (2.3 KB), beside Wi-Fi's 1.6 KB frames.
 */
constexpr size_t kReservedMinLargestBlock = 4 * 1024;
constexpr size_t kReservedMinFreeHeap = 28 * 1024;

/**
 * mbedTLS's record buffers, one each way, asked for as calloc(1, n) by every
 * session's mbedtls_ssl_setup(). Both directions have the same size here
 * (CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN, no connection IDs), so either slot
 * serves either buffer.
 */
constexpr size_t kRecordBytes = MBEDTLS_SSL_IN_BUFFER_LEN;
static_assert(MBEDTLS_SSL_OUT_BUFFER_LEN == kRecordBytes,
              "one slot size for both record buffers");
/**
 * Heap to leave free after reserving them. The radar has ~45 KB with them
 * taken; with less it is short already, and a reserve would only move the
 * shortage. It waits for a later session instead. Above kReservedMinFreeHeap,
 * or a reserve could leave the radar skipping every request.
 */
constexpr size_t kLeftAfterReserve = 32 * 1024;
static_assert(kLeftAfterReserve > kReservedMinFreeHeap, "room for a session after the reserve");

/**
 * Kept per server: a success with one says nothing about another's
 * certificate, and must not reset the count that would let it fall back.
 */
bool s_verify[kHostCount] = {true, true, true, true, true};
int s_verify_failures[kHostCount] = {};
/** Attempts in a row without room. Fetch task only. */
util::health::NoRoomRun s_no_room_run;
/** s_no_room_run.inARow(), for the health check: one byte, no lock. */
volatile uint8_t s_no_room = 0;

/**
 * The reserved record buffers. Wi-Fi's own security code allocates through
 * mbedTLS too, on Wi-Fi's task, so lending and giving back is locked.
 */
util::RecordSlots s_slots(kRecordBytes);
portMUX_TYPE s_slots_lock = portMUX_INITIALIZER_UNLOCKED;
/** Set by the main loop once the frame buffer is taken (reserveRecordBuffers()). */
volatile bool s_reserve_wanted = false;
/** A reserve that did not fit has been logged. Fetch task only. */
bool s_reserve_failed = false;
/**
 * An mbedTLS allocation found no room since configure(), on any task. Inside
 * a certificate check that surfaces as X509_FATAL_ERROR, just like a root
 * the bundle does not know, so noteResult() asks this instead.
 */
volatile bool s_alloc_failed = false;

/** The heap Wi-Fi can use (services::memory's kWifiHeap): the RTC heap's ~7.9 KB piece is not. */
size_t largestWifiBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
}

/** mbedTLS's calloc() once the buffers are reserved: a slot for a record buffer. */
void* reservedCalloc(size_t n, size_t size) {
  portENTER_CRITICAL(&s_slots_lock);
  void* slot = s_slots.take(n, size);
  portEXIT_CRITICAL(&s_slots_lock);
  if (slot == nullptr) {
    void* block = idfMbedtlsCalloc(n, size);
    if (block == nullptr && n != 0 && size != 0) {
      s_alloc_failed = true;
    }
    return block;
  }
  memset(slot, 0, kRecordBytes);
  return slot;
}

/** mbedTLS's free(): a slot goes back to the reserve, the rest to the heap. */
void reservedFree(void* ptr) {
  portENTER_CRITICAL(&s_slots_lock);
  const bool slot = s_slots.give(ptr);
  portEXIT_CRITICAL(&s_slots_lock);
  if (!slot) {
    idfMbedtlsFree(ptr);
  }
}

/**
 * Fetch task, before each session: take the reserve once it is wanted. Only
 * this task opens sessions, so none is under way here: no record buffer is
 * out, and the heap holds nothing of a session's to be in the way.
 */
void reserveIfWanted() {
  if (!s_reserve_wanted || s_slots.count() == util::RecordSlots::kMaxSlots) {
    return;
  }
  constexpr uint32_t kCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;  // as mbedTLS's own
  void* in = nullptr;
  void* out = nullptr;
  if (ESP.getFreeHeap() >= 2 * kRecordBytes + kLeftAfterReserve) {
    in = heap_caps_malloc(kRecordBytes, kCaps);
    out = heap_caps_malloc(kRecordBytes, kCaps);
  }
  if (in == nullptr || out == nullptr) {
    heap_caps_free(in);
    heap_caps_free(out);
    if (!s_reserve_failed) {
      s_reserve_failed = true;
      Serial.printf("tls: no room to reserve the record buffers yet (free %u, largest %u)\n",
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(largestWifiBlock()));
    }
    return;
  }
  portENTER_CRITICAL(&s_slots_lock);
  s_slots.add(in);
  s_slots.add(out);
  portEXIT_CRITICAL(&s_slots_lock);
  // The free hook first: mbedtls_platform_set_calloc_free() stores calloc
  // before free, and in between a slot lent by the new calloc (to Wi-Fi's
  // task, say) must not reach the old free. It hands anything but a slot to
  // mbedTLS's own free, so blocks taken before the switch go back as before.
  mbedtls_platform_set_calloc_free(idfMbedtlsCalloc, reservedFree);
  mbedtls_platform_set_calloc_free(reservedCalloc, reservedFree);
  Serial.printf(
      "tls: record buffers reserved (2 x %u bytes), heap free %u (largest %u, %u with the RTC heap)\n",
      static_cast<unsigned>(kRecordBytes), static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(largestWifiBlock()),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

size_t indexOf(Host host) { return static_cast<size_t>(host); }

/** HTTPS and the web pages take turns (beginSessions()). */
SemaphoreHandle_t turn() {
  static StaticSemaphore_t buffer;
  static const SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&buffer);
  return handle;
}
/** The fetch task has the turn. Fetch task only. */
bool s_sessions_have_turn = false;
/**
 * A page takes ~0.3 s to hand over, and its last ~11 KB (lwIP's send buffer
 * and Wi-Fi's copies of the frames) are given back only as the phone
 * acknowledges them. A session may wait this long for both, and a fetch can
 * follow two passes of two sessions each: see the gaps adsbFetchTask() keeps
 * under the health check's 180 s.
 */
constexpr unsigned long kPageWaitMs = 1000;
constexpr unsigned long kDrainWaitMs = 500;
/**
 * Free heap as the last session gave the turn back: a page's send buffers
 * are back once the heap is within kDrainSlack of it again. Fetch task only.
 */
size_t s_free_after_sessions = 0;
constexpr size_t kDrainSlack = 2 * 1024;
/** Long enough for two passes of the main loop. */
constexpr unsigned long kYieldMs = 30;
/**
 * A session holding the turn longer than this is waiting out a timeout
 * (connect 5 s, handshake 12 s, headers and body 8 s each), not streaming
 * an answer: pages are served beside it, as before the turns, rather than
 * leave a browser waiting half a minute. Measured on the radar: an ordinary
 * flight fetch holds it 3.7-4.1 s, a route lookup that times out 6 s. Lower
 * and pages share the heap with ordinary fetches again (at 3 s the heap
 * Wi-Fi can use fell to ~1 KB under page loads, as without the turns).
 */
constexpr unsigned long kPageHoldMaxMs = 8000;
/** When the fetch task took the turn; 0 while it does not have it. */
volatile unsigned long s_sessions_since_ms = 0;
/** The main loop has the turn (not serving beside a long session). Main loop only. */
bool s_pages_have_turn = false;
/**
 * The main loop is in the web server, with the turn or beside a long session,
 * since s_page_since_ms. It asks every ~10 ms, and a pass with no browser
 * there takes ~1 ms: only one that lasts kPageRealMs is a page being sent.
 */
volatile bool s_page_in_flight = false;
volatile unsigned long s_page_since_ms = 0;
constexpr unsigned long kPageRealMs = 10;
/**
 * When the last real page was handed over. Its send buffers (~11 KB) are
 * given back only as the phone acknowledges them: for kPageAfterMs a
 * shortage is still the page's.
 */
volatile unsigned long s_page_done_ms = 0;
constexpr unsigned long kPageAfterMs = 2000;
/** The fetch task's current session went ahead without the turn. */
volatile bool s_session_shared = false;

bool pageBeingSent() {
  return s_page_in_flight && millis() - s_page_since_ms >= kPageRealMs;
}

/**
 * A web page has the heap now, or had it a moment ago. Read in the opposite
 * order to endPages()'s writes (it stamps the end, then clears in-flight), so
 * a page ending between the two reads is still seen.
 */
bool pageShares() {
  if (pageBeingSent()) {
    return true;
  }
  const unsigned long done = s_page_done_ms;
  return done != 0 && millis() - done < kPageAfterMs;
}

/**
 * Whether a shortage just met counts toward the low-memory restart: not one
 * met while a web page shares the heap, which is the page's and not a heap
 * cut into pieces.
 */
bool countsAsNoRoom() {
  if (s_session_shared || pageShares()) {
    Serial.println("tls: no room beside a web page - not counted");
    return false;
  }
  return true;
}


}  // namespace

void configure(WiFiClientSecure& client, Host host, uint32_t read_timeout_ms) {
  reserveIfWanted();
  s_alloc_failed = false;
  if (s_verify[indexOf(host)]) {
    client.setCACertBundle(kCertBundle);
  } else {
    client.setInsecure();
  }
  client.setHandshakeTimeout(kHandshakeTimeoutS);
  // WiFiClientSecure::setTimeout takes seconds and hides Stream's, which
  // stays at 1 s otherwise: HTTPClient's header and body reads would give up
  // on any 1 s pause in the response.
  client.Stream::setTimeout(read_timeout_ms);
}

bool noteResult(WiFiClientSecure& client, Host host, int http_code) {
  const size_t i = indexOf(host);
  if (http_code != HTTPC_ERROR_CONNECTION_REFUSED) {
    // HTTPClient got past connecting, so the handshake went through; any
    // error came later (a timeout, a lost connection, a bad body).
    s_verify_failures[i] = 0;
    s_no_room_run.handshakeDone();
    s_no_room = s_no_room_run.inARow();
    return false;
  }
  char msg[80];
  const int err = client.lastError(msg, sizeof(msg));
  if (err >= -1) {
    return false;  // no TLS error: DNS, TCP or a handshake timeout
  }
  Serial.printf("tls: %s: %s (-0x%04x)\n", kHostNames[i], msg, -err);
  if (err == MBEDTLS_ERR_SSL_ALLOC_FAILED || s_alloc_failed) {
    // Without the reserve, enoughMemory() can only see the largest block and
    // the record buffers need two; with it, the rest of the session did not
    // fit (a web page being sent at the same time can take ~15 KB). Not the
    // server's certificate either, when the check of it ran out: it must not
    // count toward giving up on checking. The health check restarts the
    // radar after a few of these.
    noteNoRoom();
    return true;
  }
  // The bundle check reports a root it does not know as X509_FATAL_ERROR, not
  // CERT_VERIFY_FAILED.
  const bool cert_rejected =
      err == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || err == MBEDTLS_ERR_X509_FATAL_ERROR;
  if (!s_verify[i] || !cert_rejected) {
    return false;  // the network, not the certificate
  }
  if (++s_verify_failures[i] >= kVerifyFailLimit) {
    s_verify[i] = false;
    Serial.printf("tls: %s certificate cannot be verified - continuing without checking\n",
                  kHostNames[i]);
  }
  return false;
}

void noteNoRoom() {
  if (!countsAsNoRoom()) {
    return;
  }
  s_no_room_run.noRoom();
  s_no_room = s_no_room_run.inARow();
}

void noteNoRoomForAnswer() {
  if (countsAsNoRoom()) {
    s_no_room_run.noRoomForAnswer();
  } else {
    s_no_room_run.answerUncounted();
  }
  s_no_room = s_no_room_run.inARow();
}

uint8_t noRoomInARow() { return s_no_room; }

bool verifying(Host host) { return s_verify[indexOf(host)]; }

void reserveRecordBuffers() { s_reserve_wanted = true; }

void beginSessions() {
  s_session_shared = false;
  const unsigned long start = millis();
  s_sessions_have_turn = xSemaphoreTake(turn(), pdMS_TO_TICKS(kPageWaitMs)) == pdTRUE;
  // A page served beside the last, long session may still be going out:
  // waited for in the same budget.
  while (s_sessions_have_turn && pageBeingSent() && millis() - start < kPageWaitMs) {
    delay(20);
  }
  if (s_sessions_have_turn) {
    const unsigned long now = millis();
    s_sessions_since_ms = now != 0 ? now : 1;
  }
  if (!s_sessions_have_turn || pageBeingSent()) {
    s_session_shared = true;
    Serial.printf("tls: a web page held the network for %lu s - going ahead (free %u)\n",
                  kPageWaitMs / 1000, static_cast<unsigned>(ESP.getFreeHeap()));
    return;
  }
  const size_t drained = s_free_after_sessions > kDrainSlack ? s_free_after_sessions - kDrainSlack : 0;
  const unsigned long since = millis();
  while (ESP.getFreeHeap() < drained && millis() - since < kDrainWaitMs) {
    delay(20);
  }
}

void endSessions() {
  if (s_sessions_have_turn) {
    s_sessions_have_turn = false;
    s_sessions_since_ms = 0;
    if (!pageShares()) {
      // Not while a page's send buffers are out: that level is not one to
      // wait for.
      s_free_after_sessions = ESP.getFreeHeap();
    }
    xSemaphoreGive(turn());
    // A page waiting in the backlog goes next, not after the session that
    // follows (a fetch's routes): the main loop asks for the turn every 10 ms.
    delay(kYieldMs);
  }
}

bool beginPages() {
  s_pages_have_turn = xSemaphoreTake(turn(), 0) == pdTRUE;
  if (!s_pages_have_turn) {
    const unsigned long since = s_sessions_since_ms;
    if (since == 0 || millis() - since < kPageHoldMaxMs) {
      return false;
    }
  }
  s_page_since_ms = millis();
  s_page_in_flight = true;
  return true;
}

void endPages() {
  if (pageBeingSent()) {
    const unsigned long now = millis();
    s_page_done_ms = now != 0 ? now : 1;
  }
  s_page_in_flight = false;
  if (s_pages_have_turn) {
    s_pages_have_turn = false;
    xSemaphoreGive(turn());
  }
}

bool enoughMemory() {
  portENTER_CRITICAL(&s_slots_lock);
  const bool reserved = s_slots.freeCount() == util::RecordSlots::kMaxSlots;
  portEXIT_CRITICAL(&s_slots_lock);
  const size_t min_block = reserved ? kReservedMinLargestBlock : kMinLargestBlock;
  const size_t min_free = reserved ? kReservedMinFreeHeap : kMinFreeHeap;
  return largestWifiBlock() >= min_block && ESP.getFreeHeap() >= min_free;
}

}  // namespace services::tls
