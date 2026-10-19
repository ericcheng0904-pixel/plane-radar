#pragma once

#include <cstdint>

/**
 * Counts requests to one service over the last 24 hours, in hourly buckets, so
 * a published daily allowance (adsb.lol: 500 per address) is never used up by
 * this device alone. The window slides an hour at a time. Times are millis()
 * values; save()/restore() carry the count across a restart, where millis()
 * starts again from zero. Free of Arduino headers so the host test suite can
 * cover it.
 */
namespace util {

class DailyBudget {
 public:
  static constexpr int kHours = 24;
  static constexpr unsigned long kHourMs = 3600UL * 1000UL;

  explicit DailyBudget(uint16_t limit) : limit_(limit) {}

  /** Count one request at `now_ms` if the allowance has room; false if not. */
  bool take(unsigned long now_ms);

  /** Requests in the last 24 hours. */
  uint16_t used(unsigned long now_ms);
  uint16_t limit() const { return limit_; }

  /** How long until take() can succeed again; 0 if it can now. */
  unsigned long msUntilFree(unsigned long now_ms);

  struct Snapshot {
    uint16_t counts[kHours];
    uint8_t head;
    uint32_t into_hour_ms;
  };
  Snapshot save(unsigned long now_ms);
  void restore(const Snapshot& s, unsigned long now_ms);

 private:
  /** Move the window up to `now_ms`, emptying the hours that left it. */
  void advance(unsigned long now_ms);

  uint16_t limit_;
  uint16_t counts_[kHours] = {};
  /** Bucket of the current hour, which began at hour_start_ms_. */
  int head_ = 0;
  unsigned long hour_start_ms_ = 0;
  bool started_ = false;
};

}  // namespace util
