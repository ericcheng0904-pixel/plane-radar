#include "services/tls.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <mbedtls/x509.h>

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
 * handshake; below these the attempt would fail part way through.
 */
constexpr size_t kMinLargestBlock = 20 * 1024;
constexpr size_t kMinFreeHeap = 48 * 1024;

/**
 * Kept per server: a success with one says nothing about another's
 * certificate, and must not reset the count that would let it fall back.
 */
bool s_verify[kHostCount] = {true, true, true, true, true};
int s_verify_failures[kHostCount] = {};

size_t indexOf(Host host) { return static_cast<size_t>(host); }

}  // namespace

void configure(WiFiClientSecure& client, Host host, uint32_t read_timeout_ms) {
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

void noteResult(WiFiClientSecure& client, Host host, int http_code) {
  const size_t i = indexOf(host);
  if (http_code != HTTPC_ERROR_CONNECTION_REFUSED) {
    // HTTPClient got past connecting, so the handshake went through; any
    // error came later (a timeout, a lost connection, a bad body).
    s_verify_failures[i] = 0;
    return;
  }
  char msg[80];
  const int err = client.lastError(msg, sizeof(msg));
  if (err >= -1) {
    return;  // no TLS error: DNS, TCP or a handshake timeout
  }
  Serial.printf("tls: %s: %s (-0x%04x)\n", kHostNames[i], msg, -err);
  // The bundle check reports a root it does not know as X509_FATAL_ERROR, not
  // CERT_VERIFY_FAILED.
  const bool cert_rejected =
      err == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || err == MBEDTLS_ERR_X509_FATAL_ERROR;
  if (!s_verify[i] || !cert_rejected) {
    return;  // the network, not the certificate
  }
  if (++s_verify_failures[i] >= kVerifyFailLimit) {
    s_verify[i] = false;
    Serial.printf("tls: %s certificate cannot be verified - continuing without checking\n",
                  kHostNames[i]);
  }
}

bool verifying(Host host) { return s_verify[indexOf(host)]; }

bool enoughMemory() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= kMinLargestBlock &&
         ESP.getFreeHeap() >= kMinFreeHeap;
}

}  // namespace services::tls
