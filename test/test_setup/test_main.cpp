/**
 * Host tests for the guided first-time setup: the pasted-coordinates parser,
 * the town / ZIP code lookup's query and ranking, the setup texts, and the
 * self-repair rules that setup changes.
 *
 * Everything here is reachable from the setup pages by whoever is on the
 * network, so the hostile and malformed inputs matter as much as the happy
 * paths. Coordinates are public landmarks, never anybody's home.
 */
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "util/coords.h"
#include "util/geocode.h"
#include "util/health.h"
#include "util/setup_text.h"

void setUp() {}
void tearDown() {}

// --- pasted coordinates -------------------------------------------------------

namespace {

/** Parses, and checks both values to six decimals. */
void expectPair(const char* text, double lat, double lon) {
  double got_lat = 0.0;
  double got_lon = 0.0;
  TEST_ASSERT_TRUE_MESSAGE(util::coords::parseLatLonPair(text, &got_lat, &got_lon), text);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6, lat, got_lat, text);
  TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-6, lon, got_lon, text);
}

/** Refuses, and leaves the outputs alone. */
void expectNoPair(const char* text) {
  double lat = 7.0;
  double lon = 8.0;
  TEST_ASSERT_FALSE_MESSAGE(util::coords::parseLatLonPair(text, &lat, &lon), text);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 7.0, lat);
  TEST_ASSERT_DOUBLE_WITHIN(1e-12, 8.0, lon);
}

}  // namespace

void test_pair_plain_forms() {
  expectPair("51.5074, -0.1278", 51.5074, -0.1278);
  expectPair("51.5074 -0.1278", 51.5074, -0.1278);
  expectPair("51.5074,-0.1278", 51.5074, -0.1278);
  expectPair("  51.5074 ,  -0.1278  ", 51.5074, -0.1278);
  expectPair("(51.5074, -0.1278)", 51.5074, -0.1278);
  expectPair("51.5074\t-0.1278", 51.5074, -0.1278);
  expectPair("+47.3769, +8.5417", 47.3769, 8.5417);
  expectPair("-33.8688, 151.2093", -33.8688, 151.2093);
  expectPair("0, 0", 0.0, 0.0);
  expectPair("90, -180", 90.0, -180.0);
}

void test_pair_hemisphere_letters() {
  // Apple Maps copies coordinates this way.
  expectPair("51.5074\xC2\xB0 N, 0.1278\xC2\xB0 W", 51.5074, -0.1278);
  expectPair("33.8688\xC2\xB0S 151.2093\xC2\xB0" "E", -33.8688, 151.2093);
  expectPair("51.5074 n, 0.1278 w", 51.5074, -0.1278);
  // Longitude first is unambiguous once the letters say so.
  expectPair("0.1278 W, 51.5074 N", 51.5074, -0.1278);
}

void test_pair_map_links() {
  expectPair("https://www.google.com/maps/place/Eiffel+Tower/@48.8583701,2.2944813,17z/"
             "data=!3m1!4b1",
             48.8583701, 2.2944813);
  expectPair("https://www.google.com/maps/@-33.8567844,151.213108,15z", -33.8567844,
             151.213108);
  expectPair("https://maps.google.com/?q=48.8584,2.2945", 48.8584, 2.2945);
  expectPair("https://www.google.com/maps/search/?api=1&query=48.8584%2C2.2945", 48.8584,
             2.2945);
  expectPair("https://maps.apple.com/?ll=48.8584,2.2945&q=Dropped%20Pin", 48.8584, 2.2945);
  expectPair("https://maps.google.com/?q=48.8584+2.2945&z=15", 48.8584, 2.2945);
}

void test_pair_rejects_malformed() {
  expectNoPair("");
  expectNoPair("   ");
  expectNoPair("51.5074");
  expectNoPair("51.5074,");
  expectNoPair(", -0.1278");
  expectNoPair("51.5074, -0.1278, 12");     // three numbers in plain text
  expectNoPair("51.5074,,-0.1278");
  expectNoPair("51.5074-0.1278");           // the numbers touch
  expectNoPair("51.5074, -0.1278abc");
  expectNoPair("51.5074 North, 0.1278 West");
  expectNoPair("51., -0.");
  expectNoPair(".5, .5");
  expectNoPair("(51.5074, -0.1278");
  expectNoPair("Eiffel Tower");
  expectNoPair("https://maps.app.goo.gl/AbCdEf123");  // a short link needs a lookup
  expectNoPair("https://www.google.com/maps/place/Eiffel+Tower/");
  expectNoPair("https://maps.google.com/?q=Eiffel+Tower");
  expectNoPair("https://example.com/faq=48.8,2.29");  // "q=" inside another word
  double lat = 0.0;
  double lon = 0.0;
  TEST_ASSERT_FALSE(util::coords::parseLatLonPair(nullptr, &lat, &lon));
  TEST_ASSERT_FALSE(util::coords::parseLatLonPair("1, 2", nullptr, &lon));
  TEST_ASSERT_FALSE(util::coords::parseLatLonPair("1, 2", &lat, nullptr));
}

void test_pair_rejects_hostile_numbers() {
  // strtod would accept all of these; none is a coordinate.
  expectNoPair("nan, nan");
  expectNoPair("inf, 0");
  expectNoPair("1e1, 2");
  expectNoPair("0x10, 0x20");
  expectNoPair("--51.5, 0");
  // Out of range, or more digits than any coordinate has.
  expectNoPair("90.000001, 0");
  expectNoPair("0, -180.5");
  expectNoPair("1234, 5");
  expectNoPair("51.1234567890123456, 0");
  expectNoPair("99999999999999999999999999, 1");
  expectNoPair("https://www.google.com/maps/@91.0,0.0,15z");
}

void test_pair_rejects_contradicting_hemispheres() {
  expectNoPair("-51.5 S, 0.12 W");   // sign and letter
  expectNoPair("51.5 N, 47.3 N");    // two latitudes
  expectNoPair("51.5 N, -0.12");     // half lettered
  expectNoPair("0.12 E, 0.5 W");     // two longitudes
}

void test_pair_rejects_overlong_input() {
  std::string text = "https://www.google.com/maps/@48.8583701,2.2944813,17z/";
  text.append(util::coords::kMaxPairText, 'x');
  expectNoPair(text.c_str());
  std::string digits(4000, '1');
  expectNoPair(digits.c_str());
}

// --- town or ZIP code ------------------------------------------------------------

namespace {

util::geocode::Query query(const char* text) {
  util::geocode::Query q{};
  TEST_ASSERT_TRUE_MESSAGE(util::geocode::parseQuery(text, &q), text);
  return q;
}

}  // namespace

void test_query_splits_name_and_region() {
  auto q = query("Evanston");
  TEST_ASSERT_EQUAL_STRING("Evanston", q.name);
  TEST_ASSERT_EQUAL_STRING("", q.region);
  TEST_ASSERT_FALSE(q.us_zip);

  q = query("  Evanston, IL  ");
  TEST_ASSERT_EQUAL_STRING("Evanston", q.name);
  TEST_ASSERT_EQUAL_STRING("IL", q.region);
  TEST_ASSERT_FALSE(q.region_guessed);

  q = query("Evanston, IL, USA");
  TEST_ASSERT_EQUAL_STRING("IL", q.region);

  q = query("Naperville IL");
  TEST_ASSERT_EQUAL_STRING("Naperville", q.name);
  TEST_ASSERT_EQUAL_STRING("IL", q.region);
  TEST_ASSERT_TRUE(q.region_guessed);

  q = query("naperville il");
  TEST_ASSERT_EQUAL_STRING("naperville", q.name);
  TEST_ASSERT_EQUAL_STRING("il", q.region);

  q = query("Kansas City Missouri");
  TEST_ASSERT_EQUAL_STRING("Kansas City", q.name);
  TEST_ASSERT_EQUAL_STRING("Missouri", q.region);

  q = query("Charleston West Virginia");  // not "Charleston West" in Virginia
  TEST_ASSERT_EQUAL_STRING("Charleston", q.name);
  TEST_ASSERT_EQUAL_STRING("West Virginia", q.region);

  q = query("New York");  // a state's name alone is still a place to look for
  TEST_ASSERT_EQUAL_STRING("New York", q.name);
  TEST_ASSERT_EQUAL_STRING("", q.region);

  q = query("Oak Park");  // "Park" is no state
  TEST_ASSERT_EQUAL_STRING("Oak Park", q.name);
  TEST_ASSERT_EQUAL_STRING("", q.region);
}

void test_query_guessed_region_can_be_searched_whole() {
  auto q = query("Port Washington");
  TEST_ASSERT_EQUAL_STRING("Port", q.name);
  TEST_ASSERT_TRUE(q.region_guessed);
  const auto whole = util::geocode::asWhole(q);
  TEST_ASSERT_EQUAL_STRING("Port Washington", whole.name);
  TEST_ASSERT_EQUAL_STRING("", whole.region);
  TEST_ASSERT_FALSE(whole.region_guessed);
}

void test_query_recognises_zip_codes() {
  auto q = query("60201");
  TEST_ASSERT_EQUAL_STRING("60201", q.name);
  TEST_ASSERT_TRUE(q.us_zip);
  q = query(" 60614-1234 ");
  TEST_ASSERT_EQUAL_STRING("60614", q.name);
  TEST_ASSERT_TRUE(q.us_zip);
  q = query("6020");
  TEST_ASSERT_FALSE(q.us_zip);
  q = query("602011");
  TEST_ASSERT_FALSE(q.us_zip);
}

void test_query_rejects_hostile_text() {
  util::geocode::Query q{};
  std::strcpy(q.name, "kept");
  TEST_ASSERT_FALSE(util::geocode::parseQuery("", &q));
  TEST_ASSERT_FALSE(util::geocode::parseQuery("    ", &q));
  TEST_ASSERT_FALSE(util::geocode::parseQuery("x", &q));
  TEST_ASSERT_FALSE(util::geocode::parseQuery(", IL", &q));
  TEST_ASSERT_FALSE(util::geocode::parseQuery("Evanston\r\nHost: evil", &q));
  TEST_ASSERT_FALSE(util::geocode::parseQuery("Evan\x7Fston", &q));
  TEST_ASSERT_FALSE(util::geocode::parseQuery(nullptr, &q));
  const std::string longest(util::geocode::kMaxQuery, 'a');
  TEST_ASSERT_TRUE(util::geocode::parseQuery(longest.c_str(), &q));
  std::strcpy(q.name, "kept");
  const std::string too_long(util::geocode::kMaxQuery + 1, 'a');
  TEST_ASSERT_FALSE(util::geocode::parseQuery(too_long.c_str(), &q));
  TEST_ASSERT_EQUAL_STRING("kept", q.name);
}

void test_search_path_is_encoded() {
  char path[256];
  TEST_ASSERT_TRUE(util::geocode::searchPath(query("Evanston, IL"), 10, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING("/v1/search?name=Evanston&count=10&language=en&format=json", path);
  TEST_ASSERT_TRUE(util::geocode::searchPath(query("Saint Paul"), 10, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING("/v1/search?name=Saint%20Paul&count=10&language=en&format=json",
                           path);
  TEST_ASSERT_TRUE(util::geocode::searchPath(query("60201"), 10, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING(
      "/v1/search?name=60201&count=10&language=en&format=json&countryCode=US", path);
  // Nothing typed can add a parameter or break out of the value.
  TEST_ASSERT_TRUE(util::geocode::searchPath(query("a&count=100#x"), 10, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING(
      "/v1/search?name=a%26count%3D100%23x&count=10&language=en&format=json", path);
  TEST_ASSERT_TRUE(util::geocode::searchPath(query("Z\xC3\xBCrich"), 10, path, sizeof(path)));
  TEST_ASSERT_EQUAL_STRING("/v1/search?name=Z%C3%BCrich&count=10&language=en&format=json",
                           path);
  char tiny[20];
  TEST_ASSERT_FALSE(util::geocode::searchPath(query("Evanston"), 10, tiny, sizeof(tiny)));
}

namespace {

using util::geocode::Candidate;

/** What the API answered for "Evanston" (October 2026), in its order. */
const Candidate kEvanston[] = {
    {"Evanston", "Wyoming", "US", "United States", "PPLA2", 41.26828, -110.96324, 12133},
    {"Evanston", "Illinois", "US", "United States", "PPL", 42.04114, -87.69006, 75527},
    {"Evinston", "Florida", "US", "United States", "PPL", 29.48691, -82.23121, 0},
    {"Evanston", "Indiana", "US", "United States", "PPL", 38.0395, -86.84166, 0},
};

/** "Evanston, IL": the town, and heliports named after it. */
const Candidate kEvanstonHeliports[] = {
    {"Evanston Hospital Heliport", "Illinois", "US", "United States", "AIRH", 42.06, -87.68, 0},
    {"Evanston", "Illinois", "US", "United States", "PPL", 42.04114, -87.69006, 75527},
};

/** "60201" without a country: a French postal code as well. */
const Candidate kZip[] = {
    {"Compi\xC3\xA8gne", "Hauts-de-France", "FR", "France", "PPLA3", 49.41794, 2.82606, 44243},
    {"Evanston", "Illinois", "US", "United States", "PPL", 42.04114, -87.69006, 75527},
};

}  // namespace

void test_pick_prefers_the_bigger_town() {
  const auto q = query("Evanston");
  TEST_ASSERT_EQUAL_INT(1, util::geocode::pickBest(kEvanston, 4, q));
}

void test_pick_respects_the_region() {
  TEST_ASSERT_EQUAL_INT(1, util::geocode::pickBest(kEvanston, 4, query("Evanston IL")));
  TEST_ASSERT_EQUAL_INT(1, util::geocode::pickBest(kEvanston, 4, query("Evanston, Illinois")));
  TEST_ASSERT_EQUAL_INT(3, util::geocode::pickBest(kEvanston, 4, query("Evanston, IN")));
  TEST_ASSERT_EQUAL_INT(0, util::geocode::pickBest(kEvanston, 4, query("evanston wyoming")));
  TEST_ASSERT_EQUAL_INT(1, util::geocode::pickBest(kEvanston, 4, query("Evanston, USA")));
  // A region nothing matches is no reason to pick something else.
  TEST_ASSERT_EQUAL_INT(-1, util::geocode::pickBest(kEvanston, 4, query("Evanston, Ohio")));
  TEST_ASSERT_EQUAL_INT(-1, util::geocode::pickBest(kEvanston, 4, query("Evanston, France")));
}

void test_pick_prefers_towns_and_exact_names() {
  TEST_ASSERT_EQUAL_INT(1, util::geocode::pickBest(kEvanstonHeliports, 2, query("Evanston, IL")));
  // "Evinston" is the API being forgiving about spelling.
  const Candidate near_miss[] = {
      {"Evinston", "Florida", "US", "United States", "PPL", 29.5, -82.2, 900000},
      {"Evanston", "Indiana", "US", "United States", "PPL", 38.0, -86.8, 0},
  };
  TEST_ASSERT_EQUAL_INT(1, util::geocode::pickBest(near_miss, 2, query("Evanston")));
}

void test_pick_zip_by_size_and_handles_nothing() {
  TEST_ASSERT_EQUAL_INT(1, util::geocode::pickBest(kZip, 2, query("60201")));
  TEST_ASSERT_EQUAL_INT(-1, util::geocode::pickBest(kZip, 0, query("60201")));
}

void test_label_is_short_ascii() {
  char out[48];
  util::geocode::label(kEvanston[1], out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Evanston, IL", out);
  util::geocode::label(kZip[0], out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Compiegne, FR", out);
  const Candidate no_state = {"Somewhere", "", "US", "United States", "PPL", 1, 2, 0};
  util::geocode::label(no_state, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Somewhere, US", out);
  const Candidate bare = {"Atlantis", "", "", "", "PPL", 1, 2, 0};
  util::geocode::label(bare, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Atlantis", out);
  char tiny[6];
  util::geocode::label(kEvanston[1], tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("Evans", tiny);
}

void test_ascii_fold() {
  char out[32];
  util::geocode::asciiFold("Z\xC3\xBCrich", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Zurich", out);
  util::geocode::asciiFold("S\xC3\xA3o Paulo", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Sao Paulo", out);
  util::geocode::asciiFold("\xC5\x81\xC3\xB3" "d\xC5\xBA", out, sizeof(out));  // Łódź
  TEST_ASSERT_EQUAL_STRING("od", out);
  util::geocode::asciiFold("A\xF0\x9F\x9B\xA9" "B", out, sizeof(out));  // emoji dropped
  TEST_ASSERT_EQUAL_STRING("AB", out);
  util::geocode::asciiFold("cut\xC3", out, sizeof(out));  // truncated sequence
  TEST_ASSERT_EQUAL_STRING("cut", out);
  util::geocode::asciiFold("\x80\xBF" "ok", out, sizeof(out));  // stray continuation bytes
  TEST_ASSERT_EQUAL_STRING("ok", out);
  char tiny[4];
  util::geocode::asciiFold("Chicago", tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("Chi", tiny);
}

void test_state_codes() {
  TEST_ASSERT_EQUAL_STRING("IL", util::geocode::usStateCode("Illinois"));
  TEST_ASSERT_EQUAL_STRING("IL", util::geocode::usStateCode("illinois"));
  TEST_ASSERT_EQUAL_STRING("DC", util::geocode::usStateCode("Washington, D.C."));
  TEST_ASSERT_NULL(util::geocode::usStateCode("Ontario"));
  TEST_ASSERT_NULL(util::geocode::usStateCode(""));
  TEST_ASSERT_NULL(util::geocode::usStateCode(nullptr));
}

void test_url_encode() {
  char out[16];
  TEST_ASSERT_TRUE(util::geocode::urlEncode("a b/c", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a%20b%2Fc", out);
  TEST_ASSERT_TRUE(util::geocode::urlEncode("A-z_0.9~", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("A-z_0.9~", out);
  TEST_ASSERT_FALSE(util::geocode::urlEncode("&&&&&&", out, sizeof(out)));  // 18 bytes
  TEST_ASSERT_EQUAL_STRING("", out);
}

// --- setup texts ---------------------------------------------------------------

void test_disconnect_reasons() {
  using util::setup::JoinFailure;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(JoinFailure::kNone),
                          static_cast<uint8_t>(util::setup::classifyDisconnect(0)));
  for (uint8_t reason : {2, 15, 202, 204}) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(JoinFailure::kWrongPassword),
                            static_cast<uint8_t>(util::setup::classifyDisconnect(reason)));
  }
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(JoinFailure::kNotFound),
                          static_cast<uint8_t>(util::setup::classifyDisconnect(201)));
  for (uint8_t reason : {1, 8, 200, 203, 205}) {
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(JoinFailure::kOther),
                            static_cast<uint8_t>(util::setup::classifyDisconnect(reason)));
  }
}

void test_wifi_qr_text() {
  char out[96];
  TEST_ASSERT_TRUE(util::setup::wifiQrText("PlaneRadar-Setup", "ABCD2345", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:PlaneRadar-Setup;P:ABCD2345;;", out);
  TEST_ASSERT_TRUE(util::setup::wifiQrText("a;b,c", "p:q\\\"", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("WIFI:T:WPA;S:a\\;b\\,c;P:p\\:q\\\\\\\";;", out);
  // Exactly enough room, then one byte short.
  const char* expected = "WIFI:T:WPA;S:ab;P:12345678;;";
  char exact[29];
  TEST_ASSERT_EQUAL_size_t(28, std::strlen(expected));
  TEST_ASSERT_TRUE(util::setup::wifiQrText("ab", "12345678", exact, sizeof(exact)));
  TEST_ASSERT_EQUAL_STRING(expected, exact);
  char short_by_one[28];
  TEST_ASSERT_FALSE(util::setup::wifiQrText("ab", "12345678", short_by_one, sizeof(short_by_one)));
  TEST_ASSERT_EQUAL_STRING("", short_by_one);
}

void test_plausible_credentials() {
  TEST_ASSERT_TRUE(util::setup::plausibleSsid("Home"));
  TEST_ASSERT_TRUE(util::setup::plausibleSsid(std::string(32, 'x').c_str()));
  TEST_ASSERT_FALSE(util::setup::plausibleSsid(""));
  TEST_ASSERT_FALSE(util::setup::plausibleSsid(std::string(33, 'x').c_str()));
  TEST_ASSERT_FALSE(util::setup::plausibleSsid(nullptr));

  TEST_ASSERT_TRUE(util::setup::plausiblePassword(""));  // open network
  TEST_ASSERT_TRUE(util::setup::plausiblePassword("12345678"));
  TEST_ASSERT_TRUE(util::setup::plausiblePassword(std::string(63, 'p').c_str()));
  TEST_ASSERT_TRUE(util::setup::plausiblePassword(std::string(64, 'a').c_str()));  // hex key
  TEST_ASSERT_FALSE(util::setup::plausiblePassword("1234567"));
  TEST_ASSERT_FALSE(util::setup::plausiblePassword(std::string(64, 'p').c_str()));
  TEST_ASSERT_FALSE(util::setup::plausiblePassword(std::string(65, 'a').c_str()));
  TEST_ASSERT_FALSE(util::setup::plausiblePassword(nullptr));
}

void test_html_escape() {
  char out[64];
  util::setup::htmlEscape("<script>alert('x')</script>", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt;", out);
  util::setup::htmlEscape("Tom & \"Jerry\"", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("Tom &amp; &quot;Jerry&quot;", out);
  char tiny[7];
  util::setup::htmlEscape("ab&cd", tiny, sizeof(tiny));  // "ab&amp;" needs 8 with the NUL
  TEST_ASSERT_EQUAL_STRING("ab", tiny);  // never half an entity
  util::setup::htmlEscape(nullptr, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("", tiny);
}

// --- self-repair during setup ------------------------------------------------------

namespace {
using util::health::Reason;
constexpr unsigned long kMin = 60000;

/** A radar on setup step 2: Wi-Fi up for an hour, nothing ever fetched. */
util::health::State waitingForLocation() {
  util::health::State s;
  s.now_ms = 61 * kMin;
  s.loop_heartbeat_ms = s.now_ms - 100;
  s.wifi_up_since_ms = 1 * kMin;
  s.largest_free_block = 30000;
  s.setup_pending = true;
  return s;
}

uint8_t reasonOf(const util::health::State& s) {
  return static_cast<uint8_t>(util::health::check(s));
}

constexpr uint8_t kNoneReason = static_cast<uint8_t>(Reason::kNone);
}  // namespace

void test_health_setup_pending_is_not_a_stalled_feed() {
  auto s = waitingForLocation();
  TEST_ASSERT_EQUAL_UINT8(kNoneReason, reasonOf(s));
  s.setup_pending = false;  // the same silence once a location is set is a fault
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kFetchStalled), reasonOf(s));
  s.last_attempt_ms = s.now_ms - 5000;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNoData), reasonOf(s));
}

void test_health_fetch_clocks_start_when_setup_ends() {
  // An hour on step 2, then a location: the first fetch has not happened yet,
  // and that is no reason to restart.
  auto s = waitingForLocation();
  s.setup_pending = false;
  s.setup_done_ms = s.now_ms - 1000;
  TEST_ASSERT_EQUAL_UINT8(kNoneReason, reasonOf(s));
  // Four minutes on with still no attempt, the fetch task really is stuck.
  s.now_ms = s.setup_done_ms + 4 * kMin;
  s.loop_heartbeat_ms = s.now_ms - 100;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kFetchStalled), reasonOf(s));
  // Attempts but no data for 30 min after setup: the feed is the problem.
  s.now_ms = s.setup_done_ms + 31 * kMin;
  s.loop_heartbeat_ms = s.now_ms - 100;
  s.last_attempt_ms = s.now_ms - 5000;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kNoData), reasonOf(s));
}

void test_health_setup_pending_keeps_the_other_rules() {
  auto s = waitingForLocation();
  s.loop_heartbeat_ms = s.now_ms - 2 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kLoopStalled), reasonOf(s));
  s = waitingForLocation();
  s.largest_free_block = 1000;
  s.low_memory_since_ms = s.now_ms - 2 * kMin;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kLowMemory), reasonOf(s));
  s = waitingForLocation();
  s.now_ms = 25UL * 60UL * kMin;
  s.loop_heartbeat_ms = s.now_ms - 100;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kDaily), reasonOf(s));
}

void test_health_restarts_after_ten_minutes_without_wifi() {
  util::health::State s;
  s.now_ms = 60 * kMin;
  s.loop_heartbeat_ms = s.now_ms - 100;
  s.wifi_down_since_ms = s.now_ms - 9 * kMin;
  TEST_ASSERT_EQUAL_UINT8(kNoneReason, reasonOf(s));
  s.wifi_down_since_ms = s.now_ms - 10 * kMin - 1;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Reason::kWifiLost), reasonOf(s));
  s.wifi_down_since_ms = 0;
  TEST_ASSERT_EQUAL_UINT8(kNoneReason, reasonOf(s));
  TEST_ASSERT_TRUE(std::strcmp("none", util::health::reasonText(Reason::kWifiLost)) != 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pair_plain_forms);
  RUN_TEST(test_pair_hemisphere_letters);
  RUN_TEST(test_pair_map_links);
  RUN_TEST(test_pair_rejects_malformed);
  RUN_TEST(test_pair_rejects_hostile_numbers);
  RUN_TEST(test_pair_rejects_contradicting_hemispheres);
  RUN_TEST(test_pair_rejects_overlong_input);
  RUN_TEST(test_query_splits_name_and_region);
  RUN_TEST(test_query_guessed_region_can_be_searched_whole);
  RUN_TEST(test_query_recognises_zip_codes);
  RUN_TEST(test_query_rejects_hostile_text);
  RUN_TEST(test_search_path_is_encoded);
  RUN_TEST(test_pick_prefers_the_bigger_town);
  RUN_TEST(test_pick_respects_the_region);
  RUN_TEST(test_pick_prefers_towns_and_exact_names);
  RUN_TEST(test_pick_zip_by_size_and_handles_nothing);
  RUN_TEST(test_label_is_short_ascii);
  RUN_TEST(test_ascii_fold);
  RUN_TEST(test_state_codes);
  RUN_TEST(test_url_encode);
  RUN_TEST(test_disconnect_reasons);
  RUN_TEST(test_wifi_qr_text);
  RUN_TEST(test_plausible_credentials);
  RUN_TEST(test_html_escape);
  RUN_TEST(test_health_setup_pending_is_not_a_stalled_feed);
  RUN_TEST(test_health_fetch_clocks_start_when_setup_ends);
  RUN_TEST(test_health_setup_pending_keeps_the_other_rules);
  RUN_TEST(test_health_restarts_after_ten_minutes_without_wifi);
  return UNITY_END();
}
