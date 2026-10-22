#pragma once

#include <Arduino.h>

#include <cstddef>

namespace services::http {

/**
 * A response body as deserializeJson() input, ended after `max_bytes` or once
 * `budget_ms` have passed, whichever comes first.
 *
 * Content-Length only says how big a body claims to be: it can be missing
 * (HTTP/1.0 ends a body by closing the connection) or wrong, and the parser
 * reads until the JSON ends. Values the filter drops cost no memory (their
 * keys are read in full first, within the document's budget), so without
 * these limits a server could keep the fetch task reading -- an endless body,
 * or a byte every few seconds, each inside the socket's timeout -- until the
 * self-repair restarts the radar.
 */
class CappedReader {
 public:
  CappedReader(Stream& stream, size_t max_bytes, unsigned long budget_ms)
      : stream_(&stream), left_(max_bytes), start_ms_(millis()), budget_ms_(budget_ms) {}

  /** Next byte, or -1 at the end of the body, on a timeout, or at either limit. */
  int read() {
    if (left_ == 0 || millis() - start_ms_ >= budget_ms_) {
      return -1;
    }
    // Stream::read() gives up at once on a byte that is still on its way;
    // readBytes() waits for it up to the socket's timeout.
    char c = 0;
    if (stream_->readBytes(&c, 1) != 1) {
      return -1;
    }
    --left_;
    return static_cast<unsigned char>(c);
  }

  size_t readBytes(char* out, size_t length) {
    size_t n = 0;
    while (n < length) {
      const int c = read();
      if (c < 0) {
        break;
      }
      out[n++] = static_cast<char>(c);
    }
    return n;
  }

 private:
  Stream* stream_;
  size_t left_;
  unsigned long start_ms_;
  unsigned long budget_ms_;
};

}  // namespace services::http
