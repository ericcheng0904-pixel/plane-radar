#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Self-repair: decides when the radar should restart itself.
 *
 * The radar sits on a shelf with nobody watching a log, so every way it can
 * end up stuck — the fetch task gone, the main loop hung, memory too
 * fragmented to work, a data feed that stopped answering — has to end in a
 * restart, which takes a few seconds and clears all of them. Free of Arduino
 * headers so the host test suite can cover it.
 */
namespace util::health {

enum class Reason : uint8_t {
  kNone = 0,
  kLoopStalled,    // main loop stopped running (display frozen)
  kFetchStalled,   // Wi-Fi up, but no fetch even attempted for minutes
  kNoData,         // Wi-Fi up, fetches attempted, nothing good for a long time
  kLowMemory,      // largest free heap block too small for too long
  kDaily,          // scheduled: once a day, as a safety net
  kWifiLost,       // Wi-Fi down for minutes: the boot path reopens the hotspot
};

/** Short text for logs and the settings page. */
const char* reasonText(Reason r);

struct Limits {
  unsigned long loop_stall_ms = 60000;
  unsigned long fetch_stall_ms = 180000;
  unsigned long no_data_ms = 1800000;       // 30 min
  /**
   * While every feed has asked us to stay away (refused, rate-limited, or
   * its daily allowance used), a restart cannot bring data back and would
   * only send the next requests sooner; wait this long instead.
   */
  unsigned long no_data_resting_ms = 6UL * 3600UL * 1000UL;
  unsigned long wifi_settle_ms = 120000;    // Wi-Fi must be up this long first
  size_t low_memory_block = 8 * 1024;
  /**
   * HTTPS attempts in a row that found no room for a session. mbedTLS's two
   * 16.7 KB record buffers are reserved at boot (services::tls), so this is
   * the ~28 KB the rest of a session and Wi-Fi need beside it, or, if the
   * reserve did not fit, the buffers themselves: free memory can be
   * plentiful but cut into pieces too small for them, and each failed try
   * leaves a closed connection in the heap for two minutes that keeps it
   * cut; a restart puts it back together. A flight fetch not sent for want
   * of room for a handshake counts too, and so does a flight feed's answer
   * with no room left to read one aircraft of it (NoRoomRun).
   */
  uint8_t tls_no_room = 3;
  unsigned long low_memory_ms = 60000;
  unsigned long daily_ms = 24UL * 3600UL * 1000UL;
  /**
   * Reconnecting in place only ever retries the saved network. A restart
   * goes through the boot path instead, which keeps retrying it but opens
   * the setup hotspot in between — the way out after a router change.
   */
  unsigned long wifi_lost_ms = 10UL * 60UL * 1000UL;
};

/** A snapshot of the radar's state. Times are millis(); 0 = never. */
struct State {
  unsigned long now_ms = 0;
  unsigned long loop_heartbeat_ms = 0;  // last main-loop pass (0: not started)
  unsigned long wifi_up_since_ms = 0;   // when the current Wi-Fi link came up
  unsigned long last_attempt_ms = 0;    // last fetch attempt, any outcome
  unsigned long last_success_ms = 0;    // last fetch that delivered data
  size_t largest_free_block = SIZE_MAX;
  unsigned long low_memory_since_ms = 0;  // when the block first went low; 0 = fine
  uint8_t tls_no_room = 0;  // HTTPS attempts in a row without room (Limits::tls_no_room)
  // Guided setup.
  /** No location yet: the radar waits on setup step 2 and fetches nothing. */
  bool setup_pending = false;
  /** When step 2 ended; fetching is judged from then, not from Wi-Fi coming up. */
  unsigned long setup_done_ms = 0;
  unsigned long wifi_down_since_ms = 0;  // when the link dropped; 0 = up
  /** Every feed is resting at its own request (see Limits::no_data_resting_ms). */
  bool feeds_resting = false;
};

/** First reason that applies, or Reason::kNone. */
Reason check(const State& s, const Limits& limits = Limits{});

/**
 * HTTPS attempts in a row that found no room (State::tls_no_room), as
 * services::tls keeps them. Three ways to find none count alike: a request
 * not sent because the heap could not hold a handshake, a handshake that
 * failed for lack of memory, and one that went through with no room left to
 * read its answer. Only a handshake that went through ends the run, which
 * the last kind then undoes.
 */
class NoRoomRun {
 public:
  /** A request was not sent, or its handshake failed, for lack of memory. */
  void noRoom() { in_a_row_ = bump(in_a_row_); }
  /** A handshake went through: the run ends, unless noRoomForAnswer() follows. */
  void handshakeDone() {
    before_handshake_ = in_a_row_;
    in_a_row_ = 0;
  }
  /** The request whose handshake just went through had no room for its answer. */
  void noRoomForAnswer() { in_a_row_ = bump(before_handshake_); }
  uint8_t inARow() const { return in_a_row_; }

 private:
  static uint8_t bump(uint8_t n) { return n < UINT8_MAX ? static_cast<uint8_t>(n + 1) : n; }

  uint8_t in_a_row_ = 0;
  /** The run as it stood before the last handshake that went through. */
  uint8_t before_handshake_ = 0;
};

}  // namespace util::health
