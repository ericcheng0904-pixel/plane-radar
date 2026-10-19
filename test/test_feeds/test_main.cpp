/**
 * Host tests for how the radar treats its data feeds over months unattended:
 * hours-long rests for refusals, the adsb.lol daily allowance, and both kept
 * across a restart, plus the self-repair rule that waits them out.
 */
#include <unity.h>

#include "util/daily_budget.h"
#include "util/feed_failover.h"
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
  RUN_TEST(test_no_data_restart_waits_while_feeds_rest);
  RUN_TEST(test_sun_high_at_midsummer_noon_and_low_at_midnight);
  RUN_TEST(test_night_starts_after_dusk);
  return UNITY_END();
}
