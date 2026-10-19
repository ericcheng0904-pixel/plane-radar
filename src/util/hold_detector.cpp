#include "util/hold_detector.h"

namespace util {

HoldDetector::HoldDetector(unsigned long hold_ms, unsigned long max_gap_ms,
                           unsigned long ignore_until_ms)
    : hold_ms_(hold_ms), max_gap_ms_(max_gap_ms), ignore_until_ms_(ignore_until_ms) {}

bool HoldDetector::update(bool down, unsigned long now_ms) {
  const bool gap = sampled_ && now_ms - last_ms_ > max_gap_ms_;
  sampled_ = true;
  last_ms_ = now_ms;

  if (!down) {
    holding_ = false;
    fired_ = false;
    return false;
  }
  // (Re)start: first sample down, an unwatched gap, or still in the
  // power-up window.
  if (!holding_ || gap || now_ms < ignore_until_ms_) {
    holding_ = true;
    start_ms_ = now_ms;
    return false;
  }
  if (!fired_ && now_ms - start_ms_ >= hold_ms_) {
    fired_ = true;
    return true;
  }
  return false;
}

unsigned long HoldDetector::heldMs(unsigned long now_ms) const {
  return holding_ ? now_ms - start_ms_ : 0;
}

}  // namespace util
