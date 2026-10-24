/**
 * Host tests for the parsers that sit behind the Wi-Fi setup portal.
 *
 * These run on the build machine (`pio test -e native`), so every portal input
 * can be exercised — including the malformed and hostile ones — without a
 * board, a network, or a flash cycle.
 */
#include <unity.h>

#include <cstdint>
#include <cstdio>

#include "util/aircraft_class.h"
#include "util/airport_guess.h"
#include "util/callsign.h"
#include "util/coords.h"
#include "util/format.h"
#include "util/feed_failover.h"
#include "util/hold_detector.h"
#include "util/health.h"
#include "util/route_cache.h"
#include "util/portal_input.h"

void setUp() {}
void tearDown() {}

// --- checkbox ---------------------------------------------------------------

void test_checkbox_accepts_ticked_spellings() {
  TEST_ASSERT_TRUE(util::portal::checkboxChecked("T"));
  TEST_ASSERT_TRUE(util::portal::checkboxChecked("t"));
  TEST_ASSERT_TRUE(util::portal::checkboxChecked("1"));
  TEST_ASSERT_TRUE(util::portal::checkboxChecked("on"));
  TEST_ASSERT_TRUE(util::portal::checkboxChecked("true"));
}

void test_checkbox_unticked_is_false() {
  TEST_ASSERT_FALSE(util::portal::checkboxChecked(""));
  TEST_ASSERT_FALSE(util::portal::checkboxChecked(nullptr));
}

void test_checkbox_rejects_unexpected_values() {
  // An earlier revision treated "F" as ticked, which silently inverted a
  // setting; anything that is not an affirmative spelling must read as off.
  TEST_ASSERT_FALSE(util::portal::checkboxChecked("F"));
  TEST_ASSERT_FALSE(util::portal::checkboxChecked("f"));
  TEST_ASSERT_FALSE(util::portal::checkboxChecked("0"));
  TEST_ASSERT_FALSE(util::portal::checkboxChecked("off"));
  TEST_ASSERT_FALSE(util::portal::checkboxChecked("TRUE"));
  TEST_ASSERT_FALSE(util::portal::checkboxChecked("<script>"));
}

// --- step number ------------------------------------------------------------

void test_step_index_maps_one_based_to_zero_based() {
  uint8_t step = 99;
  TEST_ASSERT_TRUE(util::portal::stepIndex("1", 3, &step));
  TEST_ASSERT_EQUAL_UINT8(0, step);
  TEST_ASSERT_TRUE(util::portal::stepIndex("3", 3, &step));
  TEST_ASSERT_EQUAL_UINT8(2, step);
}

void test_step_index_rejects_out_of_range() {
  uint8_t step = 7;
  TEST_ASSERT_FALSE(util::portal::stepIndex("0", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex("4", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex("999", 3, &step));
  TEST_ASSERT_EQUAL_UINT8(7, step);  // caller's value survives a rejection
}

void test_step_index_rejects_malformed() {
  uint8_t step = 7;
  TEST_ASSERT_FALSE(util::portal::stepIndex("", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex(nullptr, 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex("2x", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex(" 2", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex("-1", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex("+2", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex("2.0", 3, &step));
  TEST_ASSERT_FALSE(util::portal::stepIndex("99999999999999", 3, &step));
  TEST_ASSERT_EQUAL_UINT8(7, step);
}

// --- coordinates ------------------------------------------------------------

void test_coordinate_parses_plain_decimals() {
  double value = 0.0;
  TEST_ASSERT_TRUE(util::coords::parseCoordinate("52.3676", &value));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 52.3676, value);
  TEST_ASSERT_TRUE(util::coords::parseCoordinate("-4.9041", &value));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, -4.9041, value);
  TEST_ASSERT_TRUE(util::coords::parseCoordinate("0", &value));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, value);
}

void test_coordinate_rejects_trailing_junk() {
  double value = 1.5;
  TEST_ASSERT_FALSE(util::coords::parseCoordinate("52.3676N", &value));
  TEST_ASSERT_FALSE(util::coords::parseCoordinate("52,3676", &value));
  TEST_ASSERT_FALSE(util::coords::parseCoordinate("", &value));
  TEST_ASSERT_FALSE(util::coords::parseCoordinate(nullptr, &value));
  TEST_ASSERT_FALSE(util::coords::parseCoordinate("abc", &value));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.5, value);
}

void test_coordinate_rejects_non_finite() {
  // strtod happily accepts these spellings; the radar's projection would then
  // produce NaN pixel coordinates for every aircraft.
  double value = 1.5;
  TEST_ASSERT_FALSE(util::coords::parseCoordinate("nan", &value));
  TEST_ASSERT_FALSE(util::coords::parseCoordinate("inf", &value));
  TEST_ASSERT_FALSE(util::coords::parseCoordinate("-inf", &value));
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 1.5, value);
}

void test_lat_lon_range() {
  TEST_ASSERT_TRUE(util::coords::validLatLon(52.3676, 4.9041));
  TEST_ASSERT_TRUE(util::coords::validLatLon(-90.0, -180.0));
  TEST_ASSERT_TRUE(util::coords::validLatLon(90.0, 180.0));
  TEST_ASSERT_FALSE(util::coords::validLatLon(90.1, 0.0));
  TEST_ASSERT_FALSE(util::coords::validLatLon(-90.1, 0.0));
  TEST_ASSERT_FALSE(util::coords::validLatLon(0.0, 180.1));
  TEST_ASSERT_FALSE(util::coords::validLatLon(0.0, -180.1));
}

// --- fixed-choice number field ----------------------------------------------

void test_one_of_accepts_listed_values() {
  const uint8_t allowed[] = {0, 3, 5, 7};
  uint8_t value = 99;
  TEST_ASSERT_TRUE(util::portal::oneOf("0", allowed, 4, &value));
  TEST_ASSERT_EQUAL_UINT8(0, value);
  TEST_ASSERT_TRUE(util::portal::oneOf("7", allowed, 4, &value));
  TEST_ASSERT_EQUAL_UINT8(7, value);
}

void test_one_of_rejects_everything_else() {
  const uint8_t allowed[] = {0, 3, 5, 7};
  uint8_t value = 5;
  TEST_ASSERT_FALSE(util::portal::oneOf("4", allowed, 4, &value));
  TEST_ASSERT_FALSE(util::portal::oneOf("70", allowed, 4, &value));
  TEST_ASSERT_FALSE(util::portal::oneOf("", allowed, 4, &value));
  TEST_ASSERT_FALSE(util::portal::oneOf(nullptr, allowed, 4, &value));
  TEST_ASSERT_FALSE(util::portal::oneOf("3s", allowed, 4, &value));
  TEST_ASSERT_FALSE(util::portal::oneOf("-3", allowed, 4, &value));
  TEST_ASSERT_EQUAL_UINT8(5, value);
}

// --- callsign ---------------------------------------------------------------

void test_callsign_extracts_airline_prefix() {
  char code[4] = {};
  TEST_ASSERT_TRUE(util::callsign::icaoPrefix("DLH4AB", code));
  TEST_ASSERT_EQUAL_STRING("DLH", code);
  TEST_ASSERT_TRUE(util::callsign::icaoPrefix("baw12", code));
  TEST_ASSERT_EQUAL_STRING("BAW", code);
}

void test_callsign_rejects_non_airline_shapes() {
  char code[4] = {};
  // Registrations and hex ids must keep showing verbatim rather than being
  // attributed to whichever airline shares their first three characters.
  TEST_ASSERT_FALSE(util::callsign::icaoPrefix("D-EABC", code));
  TEST_ASSERT_FALSE(util::callsign::icaoPrefix("DLH", code));
  TEST_ASSERT_FALSE(util::callsign::icaoPrefix("ABCDEF", code));
  TEST_ASSERT_FALSE(util::callsign::icaoPrefix("4X2ABC", code));
  TEST_ASSERT_FALSE(util::callsign::icaoPrefix("", code));
  TEST_ASSERT_FALSE(util::callsign::icaoPrefix(nullptr, code));
}

// --- aircraft class ---------------------------------------------------------

void test_class_prefers_emitter_category() {
  using util::aircraft::Klass;
  TEST_ASSERT_EQUAL(Klass::kRotor, util::aircraft::classify("B738", "A7"));
  TEST_ASSERT_EQUAL(Klass::kHeavy, util::aircraft::classify("", "A5"));
  TEST_ASSERT_EQUAL(Klass::kLight, util::aircraft::classify("", "A1"));
}

void test_class_falls_back_to_type_code() {
  using util::aircraft::Klass;
  TEST_ASSERT_EQUAL(Klass::kRotor, util::aircraft::classify("EC35", ""));
  TEST_ASSERT_EQUAL(Klass::kRotor, util::aircraft::classify("R44", ""));
  TEST_ASSERT_EQUAL(Klass::kWide, util::aircraft::classify("A388", ""));
  TEST_ASSERT_EQUAL(Klass::kWide, util::aircraft::classify("B744", ""));
  TEST_ASSERT_EQUAL(Klass::kNarrow, util::aircraft::classify("A320", ""));
  TEST_ASSERT_EQUAL(Klass::kJet, util::aircraft::classify("ZZZZ", "A3"));
  TEST_ASSERT_EQUAL(Klass::kJet, util::aircraft::classify("", ""));
  TEST_ASSERT_EQUAL(Klass::kJet, util::aircraft::classify(nullptr, nullptr));
}

void test_class_does_not_confuse_bombers_with_bells() {
  using util::aircraft::Klass;
  // Prefix matching would read the B-29 and the B-47 as Bell helicopters.
  TEST_ASSERT_EQUAL(Klass::kJet, util::aircraft::classify("B29", ""));
  TEST_ASSERT_EQUAL(Klass::kJet, util::aircraft::classify("B47", ""));
  TEST_ASSERT_EQUAL(Klass::kRotor, util::aircraft::classify("B47G", ""));
}

void test_shape_tells_types_apart() {
  using util::aircraft::Klass;
  using util::aircraft::shapeOf;
  const auto crj = shapeOf("CRJ9", "A3");
  TEST_ASSERT_EQUAL(Klass::kRegional, crj.klass);
  TEST_ASSERT_TRUE(crj.rear_engines);
  const auto e175 = shapeOf("E75L", "A3");
  TEST_ASSERT_EQUAL(Klass::kRegional, e175.klass);
  TEST_ASSERT_FALSE(e175.rear_engines);
  TEST_ASSERT_EQUAL(Klass::kNarrow, shapeOf("B38M", "A3").klass);
  const auto b789 = shapeOf("B789", "A5");
  TEST_ASSERT_EQUAL(Klass::kWide, b789.klass);
  TEST_ASSERT_EQUAL_UINT8(2, b789.engines);
  TEST_ASSERT_EQUAL_UINT8(4, shapeOf("B748", "A5").engines);
  TEST_ASSERT_EQUAL_UINT8(3, shapeOf("MD11", "A5").engines);
  TEST_ASSERT_EQUAL(Klass::kTurboprop, shapeOf("DH8D", "A2").klass);
  TEST_ASSERT_EQUAL(Klass::kBizJet, shapeOf("GLF6", "A2").klass);
  TEST_ASSERT_EQUAL(Klass::kHeavy, shapeOf("ZZZZ", "A5").klass);  // unknown big one
  TEST_ASSERT_EQUAL_UINT8(0, shapeOf("ZZZZ", "A5").engines);
  TEST_ASSERT_EQUAL_STRING("WB", util::aircraft::shortLabel(Klass::kWide));
  TEST_ASSERT_NULL(util::aircraft::shortLabel(Klass::kLight));
}

void test_alert_flags() {
  using namespace util::aircraft;
  TEST_ASSERT_EQUAL_UINT8(kFlagNotable, alertFlags("A388", "none", "", 0));
  TEST_ASSERT_EQUAL_UINT8(kFlagMilitary, alertFlags("A320", "none", "1000",
                                                    kDbFlagMilitary));
  TEST_ASSERT_EQUAL_UINT8(kFlagEmergency, alertFlags("A320", "", "7700", 0));
  TEST_ASSERT_EQUAL_UINT8(kFlagEmergency, alertFlags("A320", "general", "", 0));
  TEST_ASSERT_EQUAL_UINT8(0, alertFlags("A320", "none", "1000", 0));
  TEST_ASSERT_EQUAL_UINT8(0, alertFlags(nullptr, nullptr, nullptr, 0));
}

void test_emergency_squawks() {
  TEST_ASSERT_TRUE(util::aircraft::isEmergencySquawk("7500"));
  TEST_ASSERT_TRUE(util::aircraft::isEmergencySquawk("7600"));
  TEST_ASSERT_TRUE(util::aircraft::isEmergencySquawk("7700"));
  TEST_ASSERT_FALSE(util::aircraft::isEmergencySquawk("7000"));
  TEST_ASSERT_FALSE(util::aircraft::isEmergencySquawk("77000"));
  TEST_ASSERT_FALSE(util::aircraft::isEmergencySquawk(nullptr));
}

// --- display formatting ------------------------------------------------------

void test_thousands_groups_digits() {
  char buf[16];
  util::format::thousands(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("0", buf);
  util::format::thousands(950, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("950", buf);
  util::format::thousands(1000, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("1,000", buf);
  util::format::thousands(12400, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("12,400", buf);
  util::format::thousands(1234567, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("1,234,567", buf);
  util::format::thousands(-1500, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("-1,500", buf);
}

void test_thousands_never_overruns() {
  char buf[4];
  util::format::thousands(12400, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("12,", buf);
  util::format::thousands(5, buf, 0);  // must not write at all
}

void test_flight_number_uses_iata_code() {
  char buf[12];
  util::format::flightNumber("SWA1966", "WN", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("WN1966", buf);
  util::format::flightNumber("UAL1531", "", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("UAL1531", buf);
  util::format::flightNumber("N537JB", nullptr, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("N537JB", buf);
  util::format::flightNumber("UAL", "UA", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("UAL", buf);
  util::format::flightNumber(nullptr, "UA", buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

// --- BOOT long press -------------------------------------------------------------

namespace {
// Settings as the firmware uses them: 10 s hold, 400 ms max sampling gap,
// nothing counts in the first 5 s after power-up.
util::HoldDetector makeHold() { return util::HoldDetector(10000, 400, 5000); }

/** Feed `down` every `step` ms from `from` to `to`; true if it fired. */
bool feed(util::HoldDetector& h, bool down, unsigned long from, unsigned long to,
          unsigned long step = 50) {
  bool fired = false;
  for (unsigned long t = from; t <= to; t += step) {
    fired = h.update(down, t) || fired;
  }
  return fired;
}
}  // namespace

void test_hold_fires_after_continuous_hold() {
  auto h = makeHold();
  TEST_ASSERT_FALSE(feed(h, false, 6000, 7000));
  TEST_ASSERT_FALSE(feed(h, true, 7050, 16950));  // 9.9 s
  TEST_ASSERT_TRUE(feed(h, true, 17000, 17100));  // reaches 10 s
}

void test_hold_fires_only_once_per_press() {
  auto h = makeHold();
  feed(h, false, 6000, 6100);
  TEST_ASSERT_TRUE(feed(h, true, 6150, 16200));
  TEST_ASSERT_FALSE(feed(h, true, 16250, 30000));
}

void test_release_restarts_the_hold() {
  auto h = makeHold();
  feed(h, false, 6000, 6100);
  TEST_ASSERT_FALSE(feed(h, true, 6150, 12000));
  TEST_ASSERT_FALSE(feed(h, false, 12050, 12100));
  TEST_ASSERT_FALSE(feed(h, true, 12150, 21000));  // only 8.85 s since re-press
}

void test_unwatched_gap_restarts_the_hold() {
  // The loop was busy for 3 s: nobody saw the button, so the hold starts over.
  auto h = makeHold();
  feed(h, false, 6000, 6100);
  TEST_ASSERT_FALSE(feed(h, true, 6150, 9000));
  TEST_ASSERT_FALSE(h.update(true, 12000));        // gap
  TEST_ASSERT_FALSE(feed(h, true, 12050, 21950));  // 9.95 s since the gap
  TEST_ASSERT_TRUE(feed(h, true, 22000, 22050));
}

void test_presses_during_power_up_do_not_count() {
  // Held from power-on while being plugged in: counts from 5 s, not from 0.
  auto h = makeHold();
  TEST_ASSERT_FALSE(feed(h, true, 0, 14900));
  TEST_ASSERT_TRUE(feed(h, true, 14950, 15100));
}

void test_held_ms_reports_the_watched_hold() {
  auto h = makeHold();
  feed(h, false, 6000, 6100);
  TEST_ASSERT_EQUAL_UINT32(0, h.heldMs(6100));
  feed(h, true, 6150, 8150);
  TEST_ASSERT_EQUAL_UINT32(2000, h.heldMs(8150));
  feed(h, false, 8200, 8200);
  TEST_ASSERT_EQUAL_UINT32(0, h.heldMs(8200));
}

// --- self-repair -------------------------------------------------------------------

namespace {
using util::health::Reason;
constexpr unsigned long kMin = 60000;

/** A healthy radar ten minutes after boot, Wi-Fi up for nine. */
util::health::State healthy() {
  util::health::State s;
  s.now_ms = 10 * kMin;
  s.loop_heartbeat_ms = s.now_ms - 100;
  s.wifi_up_since_ms = 1 * kMin;
  s.last_attempt_ms = s.now_ms - 5000;
  s.last_success_ms = s.now_ms - 5000;
  s.largest_free_block = 20000;
  return s;
}
}  // namespace

void test_health_ok_when_all_is_well() {
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(healthy())));
}

void test_health_restarts_a_stalled_loop() {
  auto s = healthy();
  s.loop_heartbeat_ms = s.now_ms - 61000;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kLoopStalled),
                          static_cast<uint8_t>(util::health::check(s)));
}

void test_health_restarts_when_fetching_stopped() {
  // The bug that left an empty sky: Wi-Fi up, fetch task never started.
  auto s = healthy();
  s.last_attempt_ms = 0;
  s.last_success_ms = 0;
  s.wifi_up_since_ms = s.now_ms - 4 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kFetchStalled),
                          static_cast<uint8_t>(util::health::check(s)));
}

void test_health_waits_for_wifi_to_settle() {
  auto s = healthy();
  s.last_attempt_ms = 0;
  s.last_success_ms = 0;
  s.wifi_up_since_ms = s.now_ms - 30000;  // just connected
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(s)));
  s.wifi_up_since_ms = 0;  // no Wi-Fi at all: nothing to blame the fetch for
  s.now_ms = 100 * kMin;
  s.loop_heartbeat_ms = s.now_ms;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(s)));
}

void test_health_restarts_after_long_no_data() {
  auto s = healthy();
  s.now_ms = 60 * kMin;
  s.loop_heartbeat_ms = s.now_ms;
  s.last_attempt_ms = s.now_ms - 10000;
  s.last_success_ms = s.now_ms - 31 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNoData),
                          static_cast<uint8_t>(util::health::check(s)));
  s.last_success_ms = s.now_ms - 29 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(s)));
}

void test_health_no_data_counts_from_wifi_up_when_never_fetched() {
  auto s = healthy();
  s.now_ms = 60 * kMin;
  s.loop_heartbeat_ms = s.now_ms;
  s.last_attempt_ms = s.now_ms - 10000;  // trying, never succeeding
  s.last_success_ms = 0;
  s.wifi_up_since_ms = s.now_ms - 20 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(s)));
  s.wifi_up_since_ms = s.now_ms - 31 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNoData),
                          static_cast<uint8_t>(util::health::check(s)));
}

void test_health_restarts_on_lasting_low_memory() {
  auto s = healthy();
  s.largest_free_block = 4000;
  s.low_memory_since_ms = s.now_ms - 30000;  // brief dip: tolerated
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(s)));
  s.low_memory_since_ms = s.now_ms - 61000;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kLowMemory),
                          static_cast<uint8_t>(util::health::check(s)));
}

// With mbedTLS's record buffers reserved at boot, the largest free piece sits
// at ~9 KB for days on the radar, and nothing it asks for once running needs
// more than the water mask's 6 KB: a piece that size is not low memory.
void test_health_tolerates_the_reserved_heaps_largest_piece() {
  auto s = healthy();
  s.largest_free_block = 7 * 1024 + 512;
  s.low_memory_since_ms = s.now_ms - 2 * 60 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(s)));
  // Too small for the water mask, for over a minute: restart.
  s.largest_free_block = 6 * 1024;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kLowMemory),
                          static_cast<uint8_t>(util::health::check(s)));
}

// Plenty free in total, but no two pieces big enough for mbedTLS's record
// buffers: every HTTPS attempt fails until a restart puts the heap together.
void test_health_restarts_when_https_finds_no_room() {
  auto s = healthy();
  s.tls_no_room = 2;  // one or two can be bad luck
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                          static_cast<uint8_t>(util::health::check(s)));
  s.tls_no_room = 3;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kLowMemory),
                          static_cast<uint8_t>(util::health::check(s)));
}

// The same heap, met by fetchUpdate's check before a request
// (tls::enoughMemory()): the skips count as failed handshakes do, also after
// a run that a handshake ended. They did not count, and the radar stayed
// dark until the 30-minute no-data restart.
void test_health_counts_requests_skipped_for_no_room() {
  util::health::NoRoomRun run;
  run.handshakeDone();  // the last fetch that went through
  auto s = healthy();
  for (uint8_t skips = 1; skips <= 2; ++skips) {
    run.noRoom();
    s.tls_no_room = run.inARow();
    TEST_ASSERT_EQUAL_UINT8(skips, run.inARow());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNone),
                            static_cast<uint8_t>(util::health::check(s)));
  }
  run.noRoom();
  s.tls_no_room = run.inARow();
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kLowMemory),
                          static_cast<uint8_t>(util::health::check(s)));
}

void test_no_room_run_ends_only_with_an_answer_read() {
  util::health::NoRoomRun run;
  run.noRoom();  // not sent: no room for a handshake
  run.noRoom();  // a handshake that failed for want of it
  run.handshakeDone();  // one goes through...
  TEST_ASSERT_EQUAL_UINT8(0, run.inARow());
  run.noRoomForAnswer();  // ...with no room to read its answer
  TEST_ASSERT_EQUAL_UINT8(3, run.inARow());
  run.handshakeDone();  // the next one's answer is read: the run is over
  TEST_ASSERT_EQUAL_UINT8(0, run.inARow());
  run.noRoom();
  TEST_ASSERT_EQUAL_UINT8(1, run.inARow());
}

// No room for an answer while a web page shares the heap is the page's: it
// neither counts nor wipes the strikes before it (services::tls).
void test_no_room_beside_a_page_leaves_the_run_as_it_was() {
  util::health::NoRoomRun run;
  run.noRoom();
  run.noRoom();
  run.handshakeDone();
  run.answerUncounted();
  TEST_ASSERT_EQUAL_UINT8(2, run.inARow());
  run.noRoom();  // the next real one still makes three
  TEST_ASSERT_EQUAL_UINT8(3, run.inARow());
}

void test_health_daily_restart() {
  auto s = healthy();
  s.now_ms = 24UL * 60 * kMin + 1;
  s.loop_heartbeat_ms = s.now_ms;
  s.last_attempt_ms = s.now_ms;
  s.last_success_ms = s.now_ms;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kDaily),
                          static_cast<uint8_t>(util::health::check(s)));
}

// --- route cache --------------------------------------------------------------------

namespace {
util::Route makeRoute(const char* cs, const char* codes, bool known) {
  util::Route r{};
  snprintf(r.callsign, sizeof(r.callsign), "%s", cs);
  snprintf(r.codes, sizeof(r.codes), "%s", codes);
  r.known = known;
  return r;
}
}  // namespace

void test_route_cache_remembers_and_expires() {
  util::RouteCache c;
  util::Route out{};
  TEST_ASSERT_TRUE(c.needsLookup("AAL3031", 1000));
  c.put(makeRoute("AAL3031", "MCO-ORD", true), 1000);
  TEST_ASSERT_FALSE(c.needsLookup("AAL3031", 2000));
  TEST_ASSERT_TRUE(c.find("AAL3031", 2000, &out));
  TEST_ASSERT_EQUAL_STRING("MCO-ORD", out.codes);
  // Known routes last hours, then are asked about again.
  TEST_ASSERT_TRUE(c.find("AAL3031", 1000 + util::RouteCache::kKnownTtlMs - 1, &out));
  TEST_ASSERT_FALSE(c.find("AAL3031", 1000 + util::RouteCache::kKnownTtlMs + 1, &out));
  TEST_ASSERT_TRUE(c.needsLookup("AAL3031", 1000 + util::RouteCache::kKnownTtlMs + 1));
}

void test_route_cache_unknown_is_retried_sooner() {
  util::RouteCache c;
  c.put(makeRoute("ZZZ9999", "unknown", false), 0 + 5);
  TEST_ASSERT_FALSE(c.needsLookup("ZZZ9999", 10 * 60000UL));
  TEST_ASSERT_TRUE(c.needsLookup("ZZZ9999", 31 * 60000UL));
}

void test_route_cache_replaces_and_recycles_oldest() {
  util::RouteCache c;
  util::Route out{};
  c.put(makeRoute("UAL1", "ORD-SFO", true), 100);
  c.put(makeRoute("UAL1", "ORD-DEN", true), 200);  // same flight: replaced, not duplicated
  TEST_ASSERT_TRUE(c.find("UAL1", 300, &out));
  TEST_ASSERT_EQUAL_STRING("ORD-DEN", out.codes);
  char cs[9];
  for (size_t i = 0; i < util::RouteCache::kCapacity; ++i) {  // fill past capacity
    snprintf(cs, sizeof(cs), "SWA%u", static_cast<unsigned>(i));
    c.put(makeRoute(cs, "MDW-LAS", true), 1000 + i);
  }
  TEST_ASSERT_FALSE(c.find("UAL1", 2000, &out));  // the oldest went first
  TEST_ASSERT_TRUE(c.find("SWA63", 2000, &out));
}

// --- arriving / departing guess ----------------------------------------------------

namespace {
using util::airport_guess::Kind;
// O'Hare and Midway, as on the radar.
const util::airport_guess::Airport kChicago[] = {{41.9786f, -87.9048f}, {41.7860f, -87.7524f}};
constexpr float kKmPerDegLon = 111.0f * 0.7437f;  // cos(42 deg)

util::airport_guess::Plane at(float east_km, float north_km, int32_t alt, int16_t vrate,
                              float track) {
  // Position relative to O'Hare.
  return {kChicago[0].lat + north_km / 111.0f, kChicago[0].lon + east_km / kKmPerDegLon,
          alt, vrate, track, 180.0f};
}

uint8_t kindOf(const util::airport_guess::Guess& g) { return static_cast<uint8_t>(g.kind); }
}  // namespace

void test_guess_arrival_on_final() {
  // 15 km west of O'Hare, 4,000 ft, descending, flying east toward it.
  const auto g = util::airport_guess::guess(at(-15, 0, 4000, -800, 90), kChicago, 2);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Kind::kArriving), kindOf(g));
  TEST_ASSERT_EQUAL_UINT32(0, g.airport);
}

void test_guess_departure_climbing_away() {
  // 10 km north of O'Hare, 5,000 ft, climbing, flying north.
  const auto g = util::airport_guess::guess(at(0, 10, 5000, 2000, 0), kChicago, 2);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Kind::kDeparting), kindOf(g));
  TEST_ASSERT_EQUAL_UINT32(0, g.airport);
}

void test_guess_none_when_level_high_far_or_unaligned() {
  TEST_ASSERT_EQUAL_UINT8(0, kindOf(util::airport_guess::guess(at(-15, 0, 4000, 0, 90), kChicago, 2)));
  TEST_ASSERT_EQUAL_UINT8(0, kindOf(util::airport_guess::guess(at(-15, 0, 25000, -1500, 90), kChicago, 2)));
  TEST_ASSERT_EQUAL_UINT8(0, kindOf(util::airport_guess::guess(at(-60, 0, 6000, -800, 90), kChicago, 2)));
  TEST_ASSERT_EQUAL_UINT8(0, kindOf(util::airport_guess::guess(at(-15, 0, 4000, -800, 0), kChicago, 2)));
  // Too high for how close it is: not on any approach.
  TEST_ASSERT_EQUAL_UINT8(0, kindOf(util::airport_guess::guess(at(-5, 0, 9000, -800, 90), kChicago, 2)));
  // Unknown altitude.
  TEST_ASSERT_EQUAL_UINT8(0, kindOf(util::airport_guess::guess(at(-15, 0, INT32_MIN, -800, 90), kChicago, 2)));
}

void test_guess_none_when_two_airports_fit() {
  // 12 km north of one airport and 23 km north of another straight behind it,
  // flying south and descending: both are plausible destinations.
  const util::airport_guess::Airport in_line[] = {{41.9786f, -87.9048f}, {41.88f, -87.9048f}};
  const auto g = util::airport_guess::guess(at(0, 12, 4000, -900, 180), in_line, 2);
  TEST_ASSERT_EQUAL_UINT8(0, kindOf(g));
}

void test_route_leg_picks_the_current_leg() {
  char buf[16];
  TEST_ASSERT_TRUE(util::format::routeLeg("SNA-ORD-IAD", "ORD", true, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("SNA-ORD", buf);
  TEST_ASSERT_TRUE(util::format::routeLeg("SNA-ORD-IAD", "ORD", false, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("ORD-IAD", buf);
  TEST_ASSERT_TRUE(util::format::routeLeg("ORD-CHS-ORD", "ORD", true, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("CHS-ORD", buf);
  TEST_ASSERT_TRUE(util::format::routeLeg("ORD-CHS-ORD", "ORD", false, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("ORD-CHS", buf);
  TEST_ASSERT_TRUE(util::format::routeLeg("MCO-ORD", "ORD", true, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("MCO-ORD", buf);
}

void test_route_leg_rejects_impossible_legs() {
  char buf[16] = "untouched";
  TEST_ASSERT_FALSE(util::format::routeLeg("MCO-ORD", "ORD", false, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(util::format::routeLeg("MCO-ORD", "MCO", true, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(util::format::routeLeg("MCO-ORD", "MDW", true, buf, sizeof(buf)));
  TEST_ASSERT_FALSE(util::format::routeLeg("unknown", "ORD", true, buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("untouched", buf);
}

// --- feed failover -------------------------------------------------------------------

namespace {
// As the firmware uses it: adsb.fi then adsb.lol, two failures to fail over,
// back to the first after 5 min.
constexpr unsigned long kRetry = 5UL * 60UL * 1000UL;
util::FeedFailover makeFailover() { return util::FeedFailover(2, 2, kRetry); }
}  // namespace

void test_failover_after_two_failures() {
  auto f = makeFailover();
  TEST_ASSERT_TRUE(f.select(0));
  f.failure(10000);
  TEST_ASSERT_EQUAL_UINT32(0, f.current());  // one blip is not enough
  f.failure(20000);
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  f.success(30000);
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
}

void test_failover_success_resets_the_count() {
  auto f = makeFailover();
  f.failure(10000);
  f.success(20000);
  f.failure(30000);
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
}

void test_failover_returns_to_first_and_leaves_after_one_failure() {
  auto f = makeFailover();
  f.failure(10000);
  f.failure(20000);  // on the reserve from 20 s
  f.success(20000 + kRetry - 1);
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  f.success(20000 + kRetry);  // time to see whether the first is back
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
  f.failure(20000 + kRetry + 10000);  // it is not: straight back
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  f.success(20000 + kRetry + 20000);  // and the 5 min start over
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
}

void test_failover_stays_on_first_once_it_answers() {
  auto f = makeFailover();
  f.failure(10000);
  f.failure(20000);
  f.success(20000 + kRetry);
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
  f.success(30000 + kRetry);  // the first is back
  f.failure(40000 + kRetry);  // one blip on it no longer sends us away
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
}

void test_failover_rests_a_rate_limited_feed() {
  auto f = makeFailover();
  f.failure(10000);
  f.failure(20000);                // on the reserve
  f.failure(30000, 3600000);       // 429: rest it an hour, move on at once
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
  TEST_ASSERT_TRUE(f.resting(1, 30000));
  f.failure(40000);
  f.failure(50000);                // the first is down too: nowhere to go
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
  TEST_ASSERT_TRUE(f.select(60000));
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
  f.failure(30000 + 3600000);
  f.failure(30000 + 3600000 + 10000);  // rest over: the reserve is tried again
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
}

void test_failover_select_skips_resting_feeds() {
  auto f = makeFailover();
  f.failure(10000, 900000);  // the first rate-limits
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  f.failure(20000, 900000);  // and then the reserve
  TEST_ASSERT_FALSE(f.select(30000));  // both resting: skip this poll
  TEST_ASSERT_TRUE(f.select(10000 + 900000));  // the first's rest is over
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
}

void test_failover_does_not_return_to_a_resting_first() {
  auto f = makeFailover();
  f.failure(10000, 3600000);  // 429 from the first
  f.success(10000 + kRetry);  // retry time, but it asked for an hour
  TEST_ASSERT_EQUAL_UINT32(1, f.current());
  f.success(10000 + 3600000);
  TEST_ASSERT_EQUAL_UINT32(0, f.current());
}

void test_failover_all_failing_after_each_feed_had_its_turn() {
  auto f = makeFailover();
  for (int i = 0; i < 3; ++i) {
    f.failure(10000UL * (i + 1));
  }
  TEST_ASSERT_FALSE(f.allFailing());
  f.failure(40000);
  TEST_ASSERT_TRUE(f.allFailing());
  f.success(50000);
  TEST_ASSERT_FALSE(f.allFailing());
}

void test_failover_rest_survives_millis_wrap() {
  auto f = makeFailover();
  const unsigned long start = 0xFFFFFFFFUL - 60000UL;  // a minute before wrap
  f.failure(start, 900000);
  TEST_ASSERT_TRUE(f.resting(0, start + 600000UL));   // 10 min on, past the wrap
  TEST_ASSERT_FALSE(f.resting(0, start + 900000UL));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_checkbox_accepts_ticked_spellings);
  RUN_TEST(test_checkbox_unticked_is_false);
  RUN_TEST(test_checkbox_rejects_unexpected_values);
  RUN_TEST(test_step_index_maps_one_based_to_zero_based);
  RUN_TEST(test_step_index_rejects_out_of_range);
  RUN_TEST(test_step_index_rejects_malformed);
  RUN_TEST(test_coordinate_parses_plain_decimals);
  RUN_TEST(test_coordinate_rejects_trailing_junk);
  RUN_TEST(test_coordinate_rejects_non_finite);
  RUN_TEST(test_lat_lon_range);
  RUN_TEST(test_one_of_accepts_listed_values);
  RUN_TEST(test_one_of_rejects_everything_else);
  RUN_TEST(test_callsign_extracts_airline_prefix);
  RUN_TEST(test_callsign_rejects_non_airline_shapes);
  RUN_TEST(test_class_prefers_emitter_category);
  RUN_TEST(test_class_falls_back_to_type_code);
  RUN_TEST(test_class_does_not_confuse_bombers_with_bells);
  RUN_TEST(test_shape_tells_types_apart);
  RUN_TEST(test_alert_flags);
  RUN_TEST(test_emergency_squawks);
  RUN_TEST(test_thousands_groups_digits);
  RUN_TEST(test_thousands_never_overruns);
  RUN_TEST(test_flight_number_uses_iata_code);
  RUN_TEST(test_route_leg_picks_the_current_leg);
  RUN_TEST(test_route_leg_rejects_impossible_legs);
  RUN_TEST(test_hold_fires_after_continuous_hold);
  RUN_TEST(test_hold_fires_only_once_per_press);
  RUN_TEST(test_release_restarts_the_hold);
  RUN_TEST(test_unwatched_gap_restarts_the_hold);
  RUN_TEST(test_presses_during_power_up_do_not_count);
  RUN_TEST(test_held_ms_reports_the_watched_hold);
  RUN_TEST(test_health_ok_when_all_is_well);
  RUN_TEST(test_health_restarts_a_stalled_loop);
  RUN_TEST(test_health_restarts_when_fetching_stopped);
  RUN_TEST(test_health_waits_for_wifi_to_settle);
  RUN_TEST(test_health_restarts_after_long_no_data);
  RUN_TEST(test_health_no_data_counts_from_wifi_up_when_never_fetched);
  RUN_TEST(test_health_restarts_on_lasting_low_memory);
  RUN_TEST(test_health_tolerates_the_reserved_heaps_largest_piece);
  RUN_TEST(test_health_restarts_when_https_finds_no_room);
  RUN_TEST(test_health_counts_requests_skipped_for_no_room);
  RUN_TEST(test_no_room_run_ends_only_with_an_answer_read);
  RUN_TEST(test_no_room_beside_a_page_leaves_the_run_as_it_was);
  RUN_TEST(test_health_daily_restart);
  RUN_TEST(test_route_cache_remembers_and_expires);
  RUN_TEST(test_route_cache_unknown_is_retried_sooner);
  RUN_TEST(test_route_cache_replaces_and_recycles_oldest);
  RUN_TEST(test_guess_arrival_on_final);
  RUN_TEST(test_guess_departure_climbing_away);
  RUN_TEST(test_guess_none_when_level_high_far_or_unaligned);
  RUN_TEST(test_guess_none_when_two_airports_fit);
  RUN_TEST(test_failover_after_two_failures);
  RUN_TEST(test_failover_success_resets_the_count);
  RUN_TEST(test_failover_returns_to_first_and_leaves_after_one_failure);
  RUN_TEST(test_failover_stays_on_first_once_it_answers);
  RUN_TEST(test_failover_rests_a_rate_limited_feed);
  RUN_TEST(test_failover_select_skips_resting_feeds);
  RUN_TEST(test_failover_does_not_return_to_a_resting_first);
  RUN_TEST(test_failover_all_failing_after_each_feed_had_its_turn);
  RUN_TEST(test_failover_rest_survives_millis_wrap);
  return UNITY_END();
}
