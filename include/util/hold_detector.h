#pragma once

/**
 * Detects a deliberate long press from periodic samples of a button.
 *
 * A hold only counts while it is watched: every sample must see the button
 * down, a gap between samples longer than `max_gap_ms` restarts it (whoever
 * was sampling was busy and cannot vouch for the button in between), and
 * nothing counts before `ignore_until_ms` (power-up, when the device is being
 * handled). Fires once per press. Free of Arduino headers so the host test
 * suite can cover it.
 */
namespace util {

class HoldDetector {
 public:
  HoldDetector(unsigned long hold_ms, unsigned long max_gap_ms,
               unsigned long ignore_until_ms);

  /** One sample at time `now_ms`. True exactly once, when the hold completes. */
  bool update(bool down, unsigned long now_ms);

  /** Length of the current watched hold at `now_ms`; 0 when released. */
  unsigned long heldMs(unsigned long now_ms) const;

 private:
  unsigned long hold_ms_;
  unsigned long max_gap_ms_;
  unsigned long ignore_until_ms_;
  bool holding_ = false;
  bool fired_ = false;
  bool sampled_ = false;
  unsigned long start_ms_ = 0;
  unsigned long last_ms_ = 0;
};

}  // namespace util
