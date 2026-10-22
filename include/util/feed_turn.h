#pragma once

#include <cstddef>
#include <cstdint>

#include "util/daily_budget.h"
#include "util/feed_failover.h"

/**
 * What the radar does at the start of each fetch pass, before any request:
 * which feed to ask, or why none is asked this time. FeedFailover chooses
 * the feed; one with a daily allowance (DailyBudget) is rested when that is
 * used up, and held back while its pace says wait. Kept apart from the
 * request itself so the host tests run the radar's own decisions over
 * simulated days of outages. Free of Arduino headers.
 */
namespace util::feed_turn {

enum class Turn : uint8_t {
  kAsk,         // send `feed` a request now
  kPaced,       // `feed` is rationed and its next request is due in `wait_ms`: none now
  kUsedUp,      // `feed`'s allowance is used up: it rests `wait_ms`, the next pass asks another
  kAllResting,  // every feed has asked to be left alone
};

struct Decision {
  Turn turn;
  /** The feed it is about; for kAsk, failover.current(). */
  size_t feed;
  unsigned long wait_ms;
};

/**
 * While the feed in use is held back by its pace, with at least this long
 * still to wait, the first feed is tried again this long after the move off
 * it (or after its last failed try there), not after FeedFailover's usual
 * primary_retry_ms. A pass held back sends nothing, so the try costs the
 * rationed feed nothing. At five minutes, each short outage of the first
 * feed after the ration's burst was spent kept the radar on the paced
 * reserve, which had answered once, for five minutes after the first feed
 * was back: "No flight data" for 3-4 minutes each time. At 30 s aircraft
 * come again within ~40 s of a short outage's end, inside the 60 s before
 * "No flight data" shows. In a long outage most of those tries fail, on
 * passes that would have sent nothing; none is started with less than this
 * left on the pace, as one can take ~30 s to fail and would hold up the
 * answer due next.
 */
constexpr unsigned long kPacedPrimaryRetryMs = 30UL * 1000UL;

/**
 * Decide for a pass at `now_ms`; `budgets` holds one per feed (limit 0: no
 * allowance). A feed held back by its pace keeps the radar on it, as the
 * data comes from there, but costs the first feed nothing: once
 * kPacedPrimaryRetryMs has passed (FeedFailover::retryPrimary), the first
 * is asked instead.
 */
Decision decide(FeedFailover& failover, DailyBudget* budgets, unsigned long now_ms);

}  // namespace util::feed_turn
