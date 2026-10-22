#include "util/feed_turn.h"

namespace util::feed_turn {

Decision decide(FeedFailover& failover, DailyBudget* budgets, unsigned long now_ms) {
  if (!failover.select(now_ms)) {
    return {Turn::kAllResting, failover.current(), 0};
  }
  const size_t feed = failover.current();
  DailyBudget& budget = budgets[feed];
  if (budget.limit() == 0) {
    return {Turn::kAsk, feed, 0};
  }
  if (budget.used(now_ms) >= budget.limit()) {
    const unsigned long rest_ms = budget.msUntilFree(now_ms);
    failover.rest(now_ms, rest_ms);
    return {Turn::kUsedUp, feed, rest_ms};
  }
  const unsigned long pace_left_ms = budget.paceLeftMs(now_ms);
  if (pace_left_ms == 0) {
    return {Turn::kAsk, feed, 0};
  }
  // Not with the pace nearly over (see kPacedPrimaryRetryMs).
  // retryPrimary() only ever moves to the first feed from another one, so
  // this decides again at most once.
  if (pace_left_ms >= kPacedPrimaryRetryMs &&
      failover.retryPrimary(now_ms, kPacedPrimaryRetryMs)) {
    return decide(failover, budgets, now_ms);
  }
  return {Turn::kPaced, feed, pace_left_ms};
}

}  // namespace util::feed_turn
