/**
 * Host tests for how the radar treats its data feeds over months unattended:
 * hours-long rests for refusals, the adsb.lol daily allowance and its pace,
 * all kept across a restart, days of outages run through the radar's own
 * decisions, plus the self-repair rule that waits rests out.
 */
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "util/daily_budget.h"
#include "util/feed_failover.h"
#include "util/feed_turn.h"
#include "util/health.h"
#include "util/solar.h"

namespace {
constexpr unsigned long kMin = 60UL * 1000UL;
constexpr unsigned long kHour = 60UL * kMin;
util::FeedFailover makeFailover() { return util::FeedFailover(2, 2, 5 * kMin); }
}  // namespace

void setUp() {}
void tearDown() {}

// --- refusals ------------------------------------------------------------------------

void test_refusal_rests_the_feed_for_hours_and_moves_on() {
  auto f = makeFailover();
  f.refused(1000);
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  TEST_ASSERT_TRUE(f.resting(0, 1000 + 6 * kHour - 1));
  TEST_ASSERT_FALSE(f.resting(0, 1000 + 6 * kHour));
}

void test_repeated_refusals_double_up_to_two_days() {
  auto f = makeFailover();
  const unsigned long expect[] = {6 * kHour, 12 * kHour, 24 * kHour, 48 * kHour, 48 * kHour};
  unsigned long t = 1000;
  for (unsigned long rest : expect) {
    f.select(t);
    TEST_ASSERT_EQUAL_UINT32(0, f.current());
    f.refused(t);
    TEST_ASSERT_EQUAL_UINT32(rest, f.restLeftMs(0, t));
    t += rest;  // rest over: the reserve fails twice, back to the first
    f.failure(t);
    f.failure(t);
  }
}

void test_success_forgives_refusals() {
  auto f = makeFailover();
  f.refused(1000);
  f.refused(2000);  // the reserve refuses too
  f.select(1000 + 6 * kHour);
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
  f.success(1000 + 6 * kHour);
  TEST_ASSERT_EQUAL_UINT8(0, f.refusals(0));
  TEST_ASSERT_EQUAL_UINT8(1, f.refusals(1));
}

void test_all_resting_when_every_feed_said_stay_away() {
  auto f = makeFailover();
  f.refused(1000);
  TEST_ASSERT_FALSE(f.allResting(1000));
  f.failure(2000, 15 * kMin);  // the reserve rate-limits
  TEST_ASSERT_TRUE(f.allResting(3000));
  TEST_ASSERT_FALSE(f.select(3000));
  TEST_ASSERT_FALSE(f.allResting(2000 + 15 * kMin));
}

void test_own_rest_moves_on_without_counting_a_failure() {
  auto f = makeFailover();
  f.failure(1000);
  f.failure(2000);  // on the reserve
  f.rest(3000, 2 * kHour);  // its allowance is used up
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
  TEST_ASSERT_TRUE(f.resting(1, 3000 + kHour));
  f.success(4000);
  TEST_ASSERT_FALSE(f.allFailing());
}

void test_failover_state_survives_a_restart() {
  auto f = makeFailover();
  f.refused(10 * kHour);  // adsb.fi refused: on the reserve, first rests 6 h
  const auto snap = f.save(11 * kHour);
  auto g = makeFailover();
  g.restore(snap, 500);  // new clock after the restart
  TEST_ASSERT_EQUAL_UINT32(1, g.current());
  TEST_ASSERT_EQUAL_UINT8(1, g.refusals(0));
  TEST_ASSERT_TRUE(g.resting(0, 500 + 5 * kHour - 1));
  TEST_ASSERT_FALSE(g.resting(0, 500 + 5 * kHour));
}

// --- daily allowance ----------------------------------------------------------------

void test_budget_stops_at_its_limit() {
  util::DailyBudget b(3);
  TEST_ASSERT_TRUE(b.take(0));
  TEST_ASSERT_TRUE(b.take(1000));
  TEST_ASSERT_TRUE(b.take(2000));
  TEST_ASSERT_FALSE(b.take(3000));
  TEST_ASSERT_EQUAL_UINT16(3, b.used(3000));
}

void test_budget_frees_as_the_oldest_hour_leaves_the_window() {
  util::DailyBudget b(3);
  b.take(0);                  // hour 0
  b.take(5 * kHour);          // hour 5
  b.take(5 * kHour + 1000);
  TEST_ASSERT_EQUAL_UINT32(19 * kHour - 5000, b.msUntilFree(5 * kHour + 5000));
  TEST_ASSERT_FALSE(b.take(24 * kHour - 1));
  TEST_ASSERT_TRUE(b.take(24 * kHour));  // hour 0 left the window
  TEST_ASSERT_EQUAL_UINT16(3, b.used(24 * kHour));
  TEST_ASSERT_EQUAL_UINT16(1, b.used(29 * kHour));
  TEST_ASSERT_EQUAL_UINT16(0, b.used(60 * kHour));
}

void test_budget_survives_a_restart() {
  util::DailyBudget b(400);
  for (int i = 0; i < 150; ++i) {
    b.take(static_cast<unsigned long>(i) * 10000UL);
  }
  const auto snap = b.save(2 * kHour + 30 * kMin);
  util::DailyBudget c(400);
  c.restore(snap, 200);
  TEST_ASSERT_EQUAL_UINT16(150, c.used(200));
  // The 150 were all in the first 25 min of hour 0: gone 21.5 h after restore.
  TEST_ASSERT_EQUAL_UINT16(150, c.used(200 + 21 * kHour + 29 * kMin));
  TEST_ASSERT_EQUAL_UINT16(0, c.used(200 + 21 * kHour + 30 * kMin));
}

void test_budget_survives_millis_wrap() {
  util::DailyBudget b(2);
  const unsigned long t0 = 0xFFFFFFFFUL - 1000UL;
  b.take(t0);
  b.take(t0 + 500);
  TEST_ASSERT_FALSE(b.take(t0 + 2000));  // past the wrap
  TEST_ASSERT_TRUE(b.take(t0 + 24 * kHour));
}

// --- adsb.lol's pace --------------------------------------------------------------

namespace {
// adsb.lol's ration, as kFeeds in src/services/adsb_client.cpp sets it.
constexpr uint16_t kLolLimit = 400;
constexpr uint16_t kLolBurst = 60;
constexpr unsigned long kLolPaceMs = 270UL * 1000UL;
util::DailyBudget makeLolBudget() { return util::DailyBudget(kLolLimit, kLolBurst, kLolPaceMs); }
using util::feed_turn::Turn;
}  // namespace

void test_pace_lets_a_burst_go_then_one_request_per_pace() {
  auto b = makeLolBudget();
  unsigned long t = 1000;
  for (int i = 0; i < kLolBurst; ++i, t += 10000) {
    TEST_ASSERT_EQUAL_UINT32(0, b.paceLeftMs(t));
    TEST_ASSERT_TRUE(b.take(t));
  }
  const unsigned long last = t - 10000;
  TEST_ASSERT_EQUAL_UINT32(kLolPaceMs - 10000, b.paceLeftMs(last + 10000));
  TEST_ASSERT_EQUAL_UINT32(1, b.paceLeftMs(last + kLolPaceMs - 1));
  TEST_ASSERT_EQUAL_UINT32(0, b.paceLeftMs(last + kLolPaceMs));
  TEST_ASSERT_TRUE(b.take(last + kLolPaceMs));
  TEST_ASSERT_EQUAL_UINT32(kLolPaceMs, b.paceLeftMs(last + kLolPaceMs));
}

void test_pace_never_lifts_the_limit() {
  util::DailyBudget b(3, 1, 1000);
  TEST_ASSERT_TRUE(b.take(1000));
  TEST_ASSERT_TRUE(b.take(2000));
  TEST_ASSERT_TRUE(b.take(3000));
  TEST_ASSERT_EQUAL_UINT32(0, b.paceLeftMs(4000));  // the pace would allow one...
  TEST_ASSERT_FALSE(b.take(4000));                  // ...the limit does not
  TEST_ASSERT_TRUE(b.msUntilFree(4000) > 0);
}

void test_pace_survives_a_restart() {
  auto b = makeLolBudget();
  for (int i = 0; i < kLolBurst; ++i) {
    b.take(1000 + static_cast<unsigned long>(i) * 10000UL);
  }
  const unsigned long last = 1000 + (kLolBurst - 1) * 10000UL;
  const auto snap = b.save(last + 100000);  // restarted 100 s after the last request
  auto c = makeLolBudget();
  c.restore(snap, 500);  // a new clock
  TEST_ASSERT_EQUAL_UINT32(kLolPaceMs - 100000, c.paceLeftMs(500));
  TEST_ASSERT_EQUAL_UINT32(0, c.paceLeftMs(500 + kLolPaceMs - 100000));
  // Nothing asked yet: nothing to wait for after a restart either.
  auto fresh = makeLolBudget();
  const auto none = fresh.save(1000);
  auto d = makeLolBudget();
  d.restore(none, 200);
  TEST_ASSERT_EQUAL_UINT32(0, d.paceLeftMs(200));
}

// services::adsb saves the budget as soon as a request is counted, before it
// goes: a restart while it is under way (the daily one, a crash reading the
// answer) keeps both its count and the pace it starts.
void test_pace_survives_a_restart_during_a_request() {
  auto b = makeLolBudget();
  for (int i = 0; i < kLolBurst; ++i) {
    b.take(1000 + static_cast<unsigned long>(i) * 10000UL);
  }
  const unsigned long sent = 1000 + (kLolBurst - 1) * 10000UL + kLolPaceMs;
  TEST_ASSERT_EQUAL_UINT32(0, b.paceLeftMs(sent));
  TEST_ASSERT_TRUE(b.take(sent));
  const auto snap = b.save(sent);
  auto c = makeLolBudget();
  c.restore(snap, 700);
  TEST_ASSERT_EQUAL_UINT16(kLolBurst + 1, c.used(700));
  TEST_ASSERT_EQUAL_UINT32(kLolPaceMs, c.paceLeftMs(700));
}

void test_turn_holds_a_paced_reserve_back_without_a_failure() {
  auto f = makeFailover();
  util::DailyBudget budgets[2] = {util::DailyBudget(0), makeLolBudget()};
  for (int i = 0; i < kLolBurst; ++i) {
    budgets[1].take(1000 + static_cast<unsigned long>(i) * 10000UL);  // an earlier outage
  }
  const unsigned long t = 2 * kHour;
  f.failure(t);
  f.failure(t + 10000);  // adsb.fi down again: on adsb.lol
  auto d = util::feed_turn::decide(f, budgets, t + 20000);
  TEST_ASSERT_TRUE(d.turn == Turn::kAsk);
  TEST_ASSERT_EQUAL_UINT32(1, d.feed);
  budgets[1].take(t + 20000);
  f.success(t + 21000);
  d = util::feed_turn::decide(f, budgets, t + 30000);
  TEST_ASSERT_TRUE(d.turn == Turn::kPaced);
  TEST_ASSERT_EQUAL_UINT32(1, d.feed);
  TEST_ASSERT_EQUAL_UINT32(kLolPaceMs - 10000, d.wait_ms);
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  TEST_ASSERT_FALSE(f.allFailing());
}

// The pace holds adsb.lol back, not the search for adsb.fi: while adsb.lol
// waits, adsb.fi is tried 30 s after the move (not the usual five minutes),
// a failed try hands straight back, and the next comes 30 s after that. None
// with the pace nearly over, as a slow failure would hold up adsb.lol's answer.
void test_turn_retries_the_first_feed_soon_while_paced() {
  constexpr unsigned long kRetry = util::feed_turn::kPacedPrimaryRetryMs;
  auto f = makeFailover();
  util::DailyBudget budgets[2] = {util::DailyBudget(0), makeLolBudget()};
  for (int i = 0; i < kLolBurst; ++i) {
    budgets[1].take(1000 + static_cast<unsigned long>(i) * 10000UL);
  }
  const unsigned long t = 2 * kHour;
  f.failure(t);
  f.failure(t + 10000);  // on adsb.lol from t + 10 s
  auto d = util::feed_turn::decide(f, budgets, t + 20000);
  TEST_ASSERT_TRUE(d.turn == Turn::kAsk);
  TEST_ASSERT_EQUAL_UINT32(1, d.feed);
  budgets[1].take(t + 20000);  // paced until t + 290 s
  f.success(t + 21000);
  d = util::feed_turn::decide(f, budgets, t + 10000 + kRetry - 1);
  TEST_ASSERT_TRUE(d.turn == Turn::kPaced);
  d = util::feed_turn::decide(f, budgets, t + 10000 + kRetry);
  TEST_ASSERT_TRUE(d.turn == Turn::kAsk);
  TEST_ASSERT_EQUAL_UINT32(0, d.feed);
  f.failure(t + 45000);  // still down: straight back
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  d = util::feed_turn::decide(f, budgets, t + 45000 + kRetry - 1);
  TEST_ASSERT_TRUE(d.turn == Turn::kPaced);
  d = util::feed_turn::decide(f, budgets, t + 45000 + kRetry);
  TEST_ASSERT_TRUE(d.turn == Turn::kAsk);
  TEST_ASSERT_EQUAL_UINT32(0, d.feed);
  f.failure(t + 80000);
  d = util::feed_turn::decide(f, budgets, t + 215000);  // 75 s of pace left
  TEST_ASSERT_TRUE(d.turn == Turn::kAsk);
  TEST_ASSERT_EQUAL_UINT32(0, d.feed);
  f.failure(t + 245000);  // a slow failure: back on adsb.lol with 45 s of pace left
  d = util::feed_turn::decide(f, budgets, t + 245000 + kRetry);
  TEST_ASSERT_TRUE(d.turn == Turn::kPaced);  // 15 s left: adsb.lol's answer is next
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  d = util::feed_turn::decide(f, budgets, t + 290000);
  TEST_ASSERT_TRUE(d.turn == Turn::kAsk);
  TEST_ASSERT_EQUAL_UINT32(1, d.feed);
}

void test_turn_rests_a_used_up_feed_and_moves_on() {
  auto f = makeFailover();
  util::DailyBudget budgets[2] = {util::DailyBudget(0), util::DailyBudget(2)};
  budgets[1].take(1000);
  budgets[1].take(2000);
  f.failure(3000);
  f.failure(4000);  // on adsb.lol, whose allowance is gone
  const auto d = util::feed_turn::decide(f, budgets, 5000);
  TEST_ASSERT_TRUE(d.turn == Turn::kUsedUp);
  TEST_ASSERT_EQUAL_UINT32(1, d.feed);
  TEST_ASSERT_EQUAL_UINT32(24 * kHour - 4000, d.wait_ms);
  TEST_ASSERT_TRUE(f.resting(1, 5000));
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
}

// --- days of outages, as the radar lives them ---------------------------------------

namespace sim {

constexpr unsigned long kSec = 1000;
constexpr unsigned long kIntervalMs = 10 * kSec;    // config::kAdsbFetchIntervalMs
constexpr unsigned long kMaxBackoffMs = 60 * kSec;  // config::kAdsbMaxBackoffMs
constexpr unsigned long kAnsweredMs = 1500;         // a request that is answered
constexpr unsigned long kUnansweredMs = 5 * kSec;   // one that is not: the connect timeout
constexpr unsigned long kBootMs = 15 * kSec;        // a restart, to the first fetch
/**
 * How late aircraft may come past adsb.lol's pace: the pass it falls between
 * (10 s), and a failed adsb.fi retry (5 s) or a restart (15 s) before it.
 */
constexpr unsigned long kLateMs = 30 * kSec;

struct Run {
  /** When each adsb.lol request went, and when aircraft came (any feed, adsb.fi). */
  std::vector<unsigned long> lol, data, fi_data;
  int used_up = 0;
  int paced = 0;
  int restarts = 0;
};

/**
 * The fetch task as src/main.cpp runs it, with fetchUpdate()'s decisions
 * (util::feed_turn) on the radar's FeedFailover and DailyBudgets, from `from`
 * to `to` (simulated ms). adsb.lol always answers, adsb.fi when fi_up(t).
 * After `restart_every` up (0: never) the radar restarts, as the daily
 * restart does, and its feed state is carried over as services::adsb keeps
 * it in RTC memory, onto a new millis() clock.
 */
template <typename FiUp>
Run run(unsigned long from, unsigned long to, FiUp fi_up, unsigned long restart_every = 0) {
  Run r;
  util::FeedFailover failover = makeFailover();
  util::DailyBudget budgets[2] = {util::DailyBudget(0), makeLolBudget()};
  unsigned long t = from;
  unsigned long offset = kBootMs - t;  // millis() = t + offset, wrapping as it may
  unsigned long interval = kIntervalMs;
  while (t < to) {
    if (restart_every > 0 && t + offset >= restart_every) {
      const unsigned long now = t + offset;
      const auto f_snap = failover.save(now);
      const util::DailyBudget::Snapshot b_snap[2] = {budgets[0].save(now), budgets[1].save(now)};
      t += kBootMs;
      offset = kBootMs - t;
      failover = makeFailover();
      failover.restore(f_snap, t + offset);
      budgets[0] = util::DailyBudget(0);
      budgets[1] = makeLolBudget();
      budgets[0].restore(b_snap[0], t + offset);
      budgets[1].restore(b_snap[1], t + offset);
      interval = kIntervalMs;
      ++r.restarts;
    }
    const util::feed_turn::Decision d = util::feed_turn::decide(failover, budgets, t + offset);
    bool aircraft = false;
    bool paced = false;
    switch (d.turn) {
      case Turn::kAllResting:
        failover.failure(t + offset);
        break;
      case Turn::kUsedUp:
        ++r.used_up;
        break;
      case Turn::kPaced:
        ++r.paced;
        paced = true;
        break;
      case Turn::kAsk: {
        if (budgets[d.feed].limit() > 0) {
          budgets[d.feed].take(t + offset);
        }
        if (d.feed == 1) {
          r.lol.push_back(t);
        }
        const bool answered = d.feed == 1 || fi_up(t);
        t += answered ? kAnsweredMs : kUnansweredMs;
        if (answered) {
          failover.success(t + offset);
          aircraft = true;
          r.data.push_back(t);
          if (d.feed == 0) {
            r.fi_data.push_back(t);
          }
        } else {
          failover.failure(t + offset);
        }
        break;
      }
    }
    if (aircraft || paced) {
      interval = kIntervalMs;
    } else if (failover.allFailing()) {
      interval = std::min(interval * 2, kMaxBackoffMs);
    }
    t += interval;
  }
  return r;
}

/** The most of `at` in any 24 hours, the window sliding by the millisecond. */
size_t mostInADay(const std::vector<unsigned long>& at) {
  size_t most = 0;
  for (size_t i = 0, j = 0; j < at.size(); ++j) {
    while (at[j] - at[i] >= 24 * kHour) {
      ++i;
    }
    most = std::max(most, j - i + 1);
  }
  return most;
}

/** The longest stretch of [from, to] without one of `at`. */
unsigned long longestGap(const std::vector<unsigned long>& at, unsigned long from,
                         unsigned long to) {
  unsigned long prev = from;
  unsigned long longest = 0;
  for (unsigned long t : at) {
    if (t < from) {
      continue;
    }
    if (t > to) {
      break;
    }
    longest = std::max(longest, t - prev);
    prev = t;
  }
  return std::max(longest, to - prev);
}

/** The first of `at` from `from` on, or `to` without one. */
unsigned long firstFrom(const std::vector<unsigned long>& at, unsigned long from,
                        unsigned long to) {
  for (unsigned long t : at) {
    if (t >= from) {
      return t;
    }
  }
  return to;
}

}  // namespace sim

// adsb.fi down for two whole days. adsb.lol carries them, rationed: polled
// every 10 s it was used up in ~75 min and then out of reach for ~22 h.
void test_a_two_day_adsb_fi_outage_never_uses_up_adsb_lol() {
  const unsigned long down = kHour;
  const unsigned long up = down + 48 * kHour;
  const unsigned long end = up + kHour;
  const auto r = sim::run(0, end, [&](unsigned long t) { return t < down || t >= up; },
                          24 * kHour);
  const size_t most = sim::mostInADay(r.lol);
  const unsigned long gap = sim::longestGap(r.data, down, up);
  const unsigned long back = sim::firstFrom(r.fi_data, up, end) - up;
  char msg[200];
  snprintf(msg, sizeof(msg),
           "two-day outage: %u adsb.lol requests, at most %u in 24 h; aircraft at least every "
           "%lu s; adsb.fi back after %lu s; %d restarts",
           static_cast<unsigned>(r.lol.size()), static_cast<unsigned>(most), gap / 1000,
           back / 1000, r.restarts);
  TEST_MESSAGE(msg);
  TEST_ASSERT_EQUAL_INT(0, r.used_up);
  TEST_ASSERT_TRUE(most <= kLolLimit);
  TEST_ASSERT_TRUE(gap <= kLolPaceMs + sim::kLateMs);
  TEST_ASSERT_TRUE(back <= 5 * kMin + sim::kLateMs);
  TEST_ASSERT_EQUAL_INT(2, r.restarts);
}

// adsb.fi down for the first 20 minutes of every hour, for three days.
void test_a_flapping_adsb_fi_stays_within_adsb_lol_allowance() {
  const unsigned long end = 72 * kHour;
  const auto r = sim::run(0, end, [](unsigned long t) { return t % kHour >= 20 * kMin; },
                          24 * kHour);
  const size_t most = sim::mostInADay(r.lol);
  const unsigned long gap = sim::longestGap(r.data, 0, end);
  unsigned long slowest_back = 0;
  for (unsigned long h = 0; h < end; h += kHour) {
    const unsigned long up = h + 20 * kMin;
    slowest_back = std::max(slowest_back, sim::firstFrom(r.fi_data, up, end) - up);
  }
  char msg[200];
  snprintf(msg, sizeof(msg),
           "flapping: %u adsb.lol requests, at most %u in 24 h; aircraft at least every %lu s; "
           "adsb.fi back within %lu s",
           static_cast<unsigned>(r.lol.size()), static_cast<unsigned>(most), gap / 1000,
           slowest_back / 1000);
  TEST_MESSAGE(msg);
  TEST_ASSERT_EQUAL_INT(0, r.used_up);
  TEST_ASSERT_TRUE(most <= kLolLimit);
  TEST_ASSERT_TRUE(gap <= kLolPaceMs + sim::kLateMs);
  TEST_ASSERT_TRUE(slowest_back <= 5 * kMin + sim::kLateMs);
}

// adsb.fi down for five minutes: adsb.lol serves it at the usual 10 s.
void test_a_five_minute_adsb_fi_blip_is_served_at_the_usual_interval() {
  const unsigned long down = 2 * kHour;
  const unsigned long up = down + 5 * kMin;
  const auto r =
      sim::run(0, 4 * kHour, [&](unsigned long t) { return t < down || t >= up; });
  const unsigned long gap = sim::longestGap(r.data, down - kMin, up + 10 * kMin);
  unsigned long slowest_lol = 0;
  for (size_t i = 1; i < r.lol.size(); ++i) {
    slowest_lol = std::max(slowest_lol, r.lol[i] - r.lol[i - 1]);
  }
  char msg[160];
  snprintf(msg, sizeof(msg),
           "blip: %u adsb.lol requests, %lu s apart at most; aircraft at least every %lu s",
           static_cast<unsigned>(r.lol.size()), slowest_lol / 1000, gap / 1000);
  TEST_MESSAGE(msg);
  TEST_ASSERT_EQUAL_INT(0, r.paced);
  TEST_ASSERT_TRUE(!r.lol.empty() && r.lol.size() < kLolBurst);
  TEST_ASSERT_TRUE(slowest_lol <= sim::kIntervalMs + sim::kAnsweredMs);
  // Two failed tries at adsb.fi, then adsb.lol at the next pass.
  TEST_ASSERT_TRUE(gap <= 3 * sim::kIntervalMs + 2 * sim::kUnansweredMs + sim::kAnsweredMs);
}

// adsb.fi gone for 40 s every three hours, for three days. The first blips
// spend adsb.lol's burst (about 27 requests each: adsb.fi is tried again
// after five minutes), and the rest find adsb.lol paced. adsb.fi is then
// tried 30 s after the move, so aircraft never stop for the 60 s after
// which the screen says "No flight data"; at five minutes they stopped for
// 3-4 minutes from the third blip of a day on.
void test_short_adsb_fi_blips_after_the_burst_never_leave_the_screen_without_data() {
  constexpr unsigned long kNoDataNoticeMs = 60 * sim::kSec;  // config.h
  const unsigned long first = kHour;
  const unsigned long every = 3 * kHour;
  const unsigned long blip = 40 * sim::kSec;
  const unsigned long end = 72 * kHour;
  const auto fi_up = [&](unsigned long t) { return t < first || (t - first) % every >= blip; };
  const auto r = sim::run(0, end, fi_up, 24 * kHour);
  unsigned long worst = 0;
  for (unsigned long b = first; b < end; b += every) {
    worst = std::max(worst, sim::longestGap(r.data, b - kMin, b + 10 * kMin));
  }
  char msg[200];
  snprintf(msg, sizeof(msg),
           "blips: %u adsb.lol requests, at most %u in 24 h, %d passes paced; aircraft at least "
           "every %lu s around a blip",
           static_cast<unsigned>(r.lol.size()), static_cast<unsigned>(sim::mostInADay(r.lol)),
           r.paced, worst / 1000);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(r.paced > 0);  // the burst was spent: later blips were paced
  TEST_ASSERT_EQUAL_INT(0, r.used_up);
  TEST_ASSERT_TRUE(sim::mostInADay(r.lol) <= kLolLimit);
  TEST_ASSERT_TRUE(worst <= kNoDataNoticeMs);
}

// --- self-repair ---------------------------------------------------------------------

void test_no_data_restart_waits_while_feeds_rest() {
  util::health::State s;
  s.now_ms = 3 * kHour;
  s.loop_heartbeat_ms = s.now_ms;
  s.wifi_up_since_ms = kMin;
  s.last_attempt_ms = s.now_ms - 10000;
  s.last_success_ms = 0;
  s.feeds_resting = true;
  TEST_ASSERT_EQUAL(util::health::Reason::kNone, util::health::check(s));
  s.now_ms = 7 * kHour;
  s.loop_heartbeat_ms = s.now_ms;
  s.last_attempt_ms = s.now_ms - 10000;
  TEST_ASSERT_EQUAL(util::health::Reason::kNoData, util::health::check(s));
  s.feeds_resting = false;
  s.now_ms = 3 * kHour;
  s.loop_heartbeat_ms = s.now_ms;
  s.last_attempt_ms = s.now_ms - 10000;
  TEST_ASSERT_EQUAL(util::health::Reason::kNoData, util::health::check(s));
}

// --- night dimming ------------------------------------------------------------------

void test_sun_high_at_midsummer_noon_and_low_at_midnight() {
  // Chicago O'Hare, 2026-06-21: local solar noon is ~17:50 UTC,
  // when the sun stands 90 - 41.88 + 23.44 = ~71.6 degrees high.
  const double lat = 41.9786, lon = -87.9048;
  TEST_ASSERT_DOUBLE_WITHIN(1.0, 71.6, util::solar::elevationDeg(lat, lon, 1782064200u));
  // Twelve hours later it is as far below the horizon as it can get: ~-24.7.
  TEST_ASSERT_DOUBLE_WITHIN(1.5, -24.7, util::solar::elevationDeg(lat, lon, 1782107400u));
}

void test_night_starts_after_dusk() {
  const double lat = 41.9786, lon = -87.9048;
  // 2026-12-24, Chicago (UTC-6): sunset ~16:25, civil dusk ~16:55.
  TEST_ASSERT_FALSE(util::solar::isNight(lat, lon, 1798150800u));  // 16:20 local
  TEST_ASSERT_TRUE(util::solar::isNight(lat, lon, 1798155000u));   // 17:30 local
  TEST_ASSERT_TRUE(util::solar::isNight(lat, lon, 1798185600u));   // 02:00 local
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_refusal_rests_the_feed_for_hours_and_moves_on);
  RUN_TEST(test_repeated_refusals_double_up_to_two_days);
  RUN_TEST(test_success_forgives_refusals);
  RUN_TEST(test_all_resting_when_every_feed_said_stay_away);
  RUN_TEST(test_own_rest_moves_on_without_counting_a_failure);
  RUN_TEST(test_failover_state_survives_a_restart);
  RUN_TEST(test_budget_stops_at_its_limit);
  RUN_TEST(test_budget_frees_as_the_oldest_hour_leaves_the_window);
  RUN_TEST(test_budget_survives_a_restart);
  RUN_TEST(test_budget_survives_millis_wrap);
  RUN_TEST(test_pace_lets_a_burst_go_then_one_request_per_pace);
  RUN_TEST(test_pace_never_lifts_the_limit);
  RUN_TEST(test_pace_survives_a_restart);
  RUN_TEST(test_pace_survives_a_restart_during_a_request);
  RUN_TEST(test_turn_holds_a_paced_reserve_back_without_a_failure);
  RUN_TEST(test_turn_retries_the_first_feed_soon_while_paced);
  RUN_TEST(test_turn_rests_a_used_up_feed_and_moves_on);
  RUN_TEST(test_a_two_day_adsb_fi_outage_never_uses_up_adsb_lol);
  RUN_TEST(test_a_flapping_adsb_fi_stays_within_adsb_lol_allowance);
  RUN_TEST(test_a_five_minute_adsb_fi_blip_is_served_at_the_usual_interval);
  RUN_TEST(test_short_adsb_fi_blips_after_the_burst_never_leave_the_screen_without_data);
  RUN_TEST(test_no_data_restart_waits_while_feeds_rest);
  RUN_TEST(test_sun_high_at_midsummer_noon_and_low_at_midnight);
  RUN_TEST(test_night_starts_after_dusk);
  return UNITY_END();
}
