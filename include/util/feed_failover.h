#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Which of a short list of data feeds to ask next, in order of preference.
 *
 * Stays on a feed until it fails `failover_after` times in a row, then moves
 * to the next. Off the first feed, it goes back to see whether the first is up
 * again once `primary_retry_ms` has passed, and leaves again after a single
 * failure there, since the feed it came from was working. A feed that rate-
 * limits is rested for as long as it asks and skipped meanwhile; one that
 * refuses us outright (401/403/404: blocked, gone, or our request is wrong)
 * is rested for hours, longer each time, since asking again cannot help and
 * some feeds count those answers toward blocking the address. Times are
 * millis() values; differences are taken unsigned, so wrap-around is fine.
 * Free of Arduino headers so the host test suite can cover it.
 */
namespace util {

class FeedFailover {
 public:
  static constexpr size_t kMaxFeeds = 4;

  FeedFailover(size_t feed_count, int failover_after, unsigned long primary_retry_ms);

  /** The feed to ask next. */
  size_t current() const { return current_; }

  /**
   * Before a request: move off the current feed if it is resting. False when
   * every feed is resting; current() is then unchanged.
   */
  bool select(unsigned long now_ms);

  /** The current feed answered. */
  void success(unsigned long now_ms);

  /**
   * The current feed did not answer usefully. `rest_ms` > 0: it rate-limited,
   * leave it alone that long and move on at once.
   */
  void failure(unsigned long now_ms, unsigned long rest_ms = 0);

  /** The current feed refused the request: rest it (see kRefusedRestMs) and move on. */
  void refused(unsigned long now_ms);

  /**
   * Leave the current feed alone for `rest_ms` and move on, without counting
   * a failure (our own reason: e.g. its daily allowance is used up).
   */
  void rest(unsigned long now_ms, unsigned long rest_ms);

  /** Every feed has had its failures in turn, with no success since. */
  bool allFailing() const;

  /** Every feed is resting: they asked us to stay away, so nothing can be fetched. */
  bool allResting(unsigned long now_ms) const;

  bool resting(size_t feed, unsigned long now_ms) const;
  /** Time left on `feed`'s rest; 0 when it is not resting. */
  unsigned long restLeftMs(size_t feed, unsigned long now_ms) const;
  /** Refusals in a row from `feed` (the next rest doubles with each). */
  uint8_t refusals(size_t feed) const { return feed < count_ ? refusals_[feed] : 0; }

  /** The state worth keeping across a restart, with rests as time left. */
  struct Snapshot {
    uint8_t current;
    uint8_t refusals[kMaxFeeds];
    uint32_t rest_left_ms[kMaxFeeds];
  };
  Snapshot save(unsigned long now_ms) const;
  /** Pick up where save() left off; `now_ms` is the new clock's time. */
  void restore(const Snapshot& s, unsigned long now_ms);

  static constexpr unsigned long kRefusedRestMs = 6UL * 3600UL * 1000UL;
  static constexpr unsigned long kRefusedRestMaxMs = 48UL * 3600UL * 1000UL;

 private:
  void startRest(size_t feed, unsigned long now_ms, unsigned long rest_ms);
  /** Forget rests that are over, before a later wrap-around could revive them. */
  void endRests(unsigned long now_ms);
  void switchTo(size_t feed, unsigned long now_ms);
  /** Next feed after the current one that is not resting; false if none. */
  bool switchToNext(unsigned long now_ms);

  size_t count_;
  int failover_after_;
  unsigned long primary_retry_ms_;
  size_t current_ = 0;
  int feed_failures_ = 0;
  int failures_ = 0;
  unsigned long since_ms_ = 0;
  bool retrying_primary_ = false;
  uint8_t refusals_[kMaxFeeds] = {};
  bool resting_[kMaxFeeds] = {};
  unsigned long rest_from_ms_[kMaxFeeds] = {};
  unsigned long rest_ms_[kMaxFeeds] = {};
};

}  // namespace util
