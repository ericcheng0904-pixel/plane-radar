#include "util/feed_failover.h"

namespace util {

FeedFailover::FeedFailover(size_t feed_count, int failover_after,
                           unsigned long primary_retry_ms)
    : count_(feed_count == 0 ? 1 : (feed_count > kMaxFeeds ? kMaxFeeds : feed_count)),
      failover_after_(failover_after < 1 ? 1 : failover_after),
      primary_retry_ms_(primary_retry_ms) {}

bool FeedFailover::resting(size_t feed, unsigned long now_ms) const {
  // Measured from when the rest began, so a rest that spans millis()
  // wrap-around still ends on time.
  return feed < count_ && resting_[feed] && now_ms - rest_from_ms_[feed] < rest_ms_[feed];
}

unsigned long FeedFailover::restLeftMs(size_t feed, unsigned long now_ms) const {
  return resting(feed, now_ms) ? rest_ms_[feed] - (now_ms - rest_from_ms_[feed]) : 0;
}

void FeedFailover::startRest(size_t feed, unsigned long now_ms, unsigned long rest_ms) {
  resting_[feed] = true;
  rest_from_ms_[feed] = now_ms;
  rest_ms_[feed] = rest_ms;
}

void FeedFailover::endRests(unsigned long now_ms) {
  for (size_t feed = 0; feed < count_; ++feed) {
    if (resting_[feed] && !resting(feed, now_ms)) {
      resting_[feed] = false;
    }
  }
}

void FeedFailover::switchTo(size_t feed, unsigned long now_ms) {
  current_ = feed;
  feed_failures_ = 0;
  since_ms_ = now_ms;
}

bool FeedFailover::switchToNext(unsigned long now_ms) {
  for (size_t k = 1; k < count_; ++k) {
    const size_t feed = (current_ + k) % count_;
    if (!resting(feed, now_ms)) {
      switchTo(feed, now_ms);
      return true;
    }
  }
  return false;
}

bool FeedFailover::select(unsigned long now_ms) {
  endRests(now_ms);
  if (!resting(current_, now_ms)) {
    return true;
  }
  retrying_primary_ = false;
  return switchToNext(now_ms);
}

void FeedFailover::success(unsigned long now_ms) {
  endRests(now_ms);
  failures_ = 0;
  feed_failures_ = 0;
  refusals_[current_] = 0;
  retrying_primary_ = false;
  retryPrimary(now_ms);
}

bool FeedFailover::retryPrimary(unsigned long now_ms, unsigned long after_ms) {
  if (current_ == 0 || now_ms - since_ms_ < after_ms || resting(0, now_ms)) {
    return false;
  }
  switchTo(0, now_ms);
  retrying_primary_ = true;
  return true;
}

void FeedFailover::failure(unsigned long now_ms, unsigned long rest_ms) {
  endRests(now_ms);
  ++failures_;
  if (rest_ms > 0) {
    startRest(current_, now_ms, rest_ms);
  } else if (++feed_failures_ < (retrying_primary_ ? 1 : failover_after_)) {
    return;
  }
  retrying_primary_ = false;
  if (!switchToNext(now_ms)) {
    feed_failures_ = 0;  // nowhere better to go: keep trying this one
  }
}

void FeedFailover::refused(unsigned long now_ms) {
  unsigned long rest_ms = kRefusedRestMs;
  for (uint8_t i = 0; i < refusals_[current_] && rest_ms < kRefusedRestMaxMs; ++i) {
    rest_ms *= 2;
  }
  if (rest_ms > kRefusedRestMaxMs) {
    rest_ms = kRefusedRestMaxMs;
  }
  if (refusals_[current_] < 255) {
    ++refusals_[current_];
  }
  failure(now_ms, rest_ms);
}

void FeedFailover::rest(unsigned long now_ms, unsigned long rest_ms) {
  endRests(now_ms);
  startRest(current_, now_ms, rest_ms);
  retrying_primary_ = false;
  switchToNext(now_ms);
}

bool FeedFailover::allResting(unsigned long now_ms) const {
  for (size_t feed = 0; feed < count_; ++feed) {
    if (!resting(feed, now_ms)) {
      return false;
    }
  }
  return true;
}

FeedFailover::Snapshot FeedFailover::save(unsigned long now_ms) const {
  Snapshot s{};
  s.current = static_cast<uint8_t>(current_);
  for (size_t feed = 0; feed < count_; ++feed) {
    s.refusals[feed] = refusals_[feed];
    s.rest_left_ms[feed] = static_cast<uint32_t>(restLeftMs(feed, now_ms));
  }
  return s;
}

void FeedFailover::restore(const Snapshot& s, unsigned long now_ms) {
  switchTo(s.current < count_ ? s.current : 0, now_ms);
  for (size_t feed = 0; feed < count_; ++feed) {
    refusals_[feed] = s.refusals[feed];
    resting_[feed] = false;
    if (s.rest_left_ms[feed] > 0) {
      startRest(feed, now_ms, s.rest_left_ms[feed]);
    }
  }
}

bool FeedFailover::allFailing() const {
  return failures_ >= static_cast<int>(count_) * failover_after_;
}

}  // namespace util
