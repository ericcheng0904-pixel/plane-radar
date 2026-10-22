#include "util/health.h"

namespace util::health {

const char* reasonText(Reason r) {
  switch (r) {
    case Reason::kLoopStalled:
      return "display loop stalled";
    case Reason::kFetchStalled:
      return "flight data task stopped";
    case Reason::kNoData:
      return "no flight data for 30 min";
    case Reason::kLowMemory:
      return "low memory";
    case Reason::kDaily:
      return "daily refresh";
    case Reason::kWifiLost:
      return "Wi-Fi lost for 10 min";
    case Reason::kNone:
    default:
      return "none";
  }
}

namespace {

/** now - then, or `now` itself when `then` is 0 (never). */
unsigned long since(unsigned long now, unsigned long then) {
  return then == 0 ? now : now - then;
}

}  // namespace

Reason check(const State& s, const Limits& limits) {
  if (s.loop_heartbeat_ms != 0 && s.now_ms - s.loop_heartbeat_ms > limits.loop_stall_ms) {
    return Reason::kLoopStalled;
  }

  // Fetching can only start once Wi-Fi is up and setup step 2 is done, so
  // that is where its clocks start (a radar that waited an hour for its
  // location must not restart the moment it gets one).
  const unsigned long up_ref =
      s.setup_done_ms > s.wifi_up_since_ms ? s.setup_done_ms : s.wifi_up_since_ms;
  const bool wifi_settled =
      s.wifi_up_since_ms != 0 && s.now_ms - up_ref >= limits.wifi_settle_ms;
  // While setup waits for a location nothing is fetched, by design.
  if (wifi_settled && !s.setup_pending) {
    // Measured from whichever is later: the last attempt, or Wi-Fi coming up
    // (a fetch task that never ran must not count from boot).
    const unsigned long attempt_ref = s.last_attempt_ms > up_ref ? s.last_attempt_ms : up_ref;
    if (since(s.now_ms, attempt_ref) > limits.fetch_stall_ms) {
      return Reason::kFetchStalled;
    }
    const unsigned long data_ref = s.last_success_ms > up_ref ? s.last_success_ms : up_ref;
    const unsigned long no_data_ms =
        s.feeds_resting ? limits.no_data_resting_ms : limits.no_data_ms;
    if (since(s.now_ms, data_ref) > no_data_ms) {
      return Reason::kNoData;
    }
  }

  if (s.wifi_down_since_ms != 0 && s.now_ms - s.wifi_down_since_ms > limits.wifi_lost_ms) {
    return Reason::kWifiLost;
  }

  if (s.low_memory_since_ms != 0 && s.largest_free_block < limits.low_memory_block &&
      s.now_ms - s.low_memory_since_ms > limits.low_memory_ms) {
    return Reason::kLowMemory;
  }
  if (s.tls_no_room >= limits.tls_no_room) {
    return Reason::kLowMemory;
  }

  if (s.now_ms > limits.daily_ms) {
    return Reason::kDaily;
  }
  return Reason::kNone;
}

}  // namespace util::health
