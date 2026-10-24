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
                    static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
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
  Serial.printf("tls: record buffers reserved (2 x %u bytes), heap free %u (largest %u)\n",
                static_cast<unsigned>(kRecordBytes), static_cast<unsigned>(ESP.getFreeHeap()),
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
  s_no_room_run.noRoom();
  s_no_room = s_no_room_run.inARow();
}

void noteNoRoomForAnswer() {
  s_no_room_run.noRoomForAnswer();
  s_no_room = s_no_room_run.inARow();
}

uint8_t noRoomInARow() { return s_no_room; }

bool verifying(Host host) { return s_verify[indexOf(host)]; }

void reserveRecordBuffers() { s_reserve_wanted = true; }

void beginSessions() {
  s_sessions_have_turn = xSemaphoreTake(turn(), pdMS_TO_TICKS(kPageWaitMs)) == pdTRUE;
  if (!s_sessions_have_turn) {
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
    s_free_after_sessions = ESP.getFreeHeap();
    xSemaphoreGive(turn());
    // A page waiting in the backlog goes next, not after the session that
    // follows (a fetch's routes): the main loop asks for the turn every 10 ms.
    delay(kYieldMs);
  }
}

bool beginPages() { return xSemaphoreTake(turn(), 0) == pdTRUE; }

void endPages() { xSemaphoreGive(turn()); }

bool enoughMemory() {
  portENTER_CRITICAL(&s_slots_lock);
  const bool reserved = s_slots.freeCount() == util::RecordSlots::kMaxSlots;
  portEXIT_CRITICAL(&s_slots_lock);
  const size_t min_block = reserved ? kReservedMinLargestBlock : kMinLargestBlock;
  const size_t min_free = reserved ? kReservedMinFreeHeap : kMinFreeHeap;
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= min_block &&
         ESP.getFreeHeap() >= min_free;
}

}  // namespace services::tls
