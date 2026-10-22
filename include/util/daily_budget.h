#pragma once

#include <cstdint>

/**
 * Counts requests to one service over the last 24 hours, in hourly buckets, so
 * a published daily allowance (adsb.lol: 500 per address) is never used up by
 * this device alone. The window slides an hour at a time. Times are millis()
 * values; save()/restore() carry the count across a restart, where millis()
 * starts again from zero. Free of Arduino headers so the host test suite can
 * cover it.
 *
 * A hard limit alone does not ration: polled every 10 s, a reserve feed used
 * up 400 requests in about 70 minutes and was then out of reach for the 22 h
 * its oldest hour took to leave the window. So the budget also paces: the
 * first `burst` requests in the window go as fast as they are asked for (a
 * short outage of the main feed), and after that one goes per `pace_ms` at
 * most, which a whole day of asking stays within. Pacing counts only the
 * requests themselves, so nothing the caller does in between (switching
 * feeds, restarting) can speed it up.
 */
namespace util {

class DailyBudget {
 public:
  static constexpr int kHours = 24;
  static constexpr unsigned long kHourMs = 3600UL * 1000UL;

  /** At most `limit` requests in 24 hours, unpaced. */
  explicit DailyBudget(uint16_t limit) : DailyBudget(limit, limit, 0) {}

  /**
   * At most `limit` requests in 24 hours; once `burst` of them are in the
   * window, at most one per `pace_ms` (0: no pacing).
   */
  DailyBudget(uint16_t limit, uint16_t burst, unsigned long pace_ms)
      : limit_(limit), burst_(burst), pace_ms_(pace_ms) {}

  /**
   * Count one request at `now_ms` if the allowance has room; false if not.
   * The hard limit: pacing is for the caller to ask about (paceLeftMs()).
   */
  bool take(unsigned long now_ms);

  /** Requests in the last 24 hours. */
  uint16_t used(unsigned long now_ms);
  uint16_t limit() const { return limit_; }

  /** How long until take() can succeed again; 0 if it can now. */
  unsigned long msUntilFree(unsigned long now_ms);

  /**
   * How long the next request should wait for the pace; 0 if it may go now
   * (as far as pacing goes: the limit is msUntilFree()'s business).
   */
  unsigned long paceLeftMs(unsigned long now_ms);

  struct Snapshot {
    uint16_t counts[kHours];
    uint8_t head;
    uint32_t into_hour_ms;
    /** Time since the last request; kNoRequest when there was none. */
    uint32_t since_last_ms;
  };
  static constexpr uint32_t kNoRequest = 0xFFFFFFFFu;
  Snapshot save(unsigned long now_ms);
  void restore(const Snapshot& s, unsigned long now_ms);

 private:
  /** Move the window up to `now_ms`, emptying the hours that left it. */
  void advance(unsigned long now_ms);

  uint16_t limit_;
  uint16_t burst_;
  unsigned long pace_ms_;
  uint16_t counts_[kHours] = {};
  /** Bucket of the current hour, which began at hour_start_ms_. */
  int head_ = 0;
  unsigned long hour_start_ms_ = 0;
  bool started_ = false;
  /** When the last request was counted, if any was (has_last_). */
  unsigned long last_ms_ = 0;
  bool has_last_ = false;
};

}  // namespace util
