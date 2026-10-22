/**
 * Host tests for util::feed_answer and util::json_stream: how the radar reads
 * a flight feed's answer one aircraft at a time, and what it makes of it.
 *
 * The answer used to be parsed whole, and a normal evening's at the widest
 * range did not fit the memory left beside the TLS session. These check that
 * the walk reads answers of any size in the memory of one aircraft, that an
 * answer cut off at any byte is read as far as it goes and never mistaken for
 * a broken one, which answers count for or against the feed, and that
 * deserializeJson() leaves the walk exactly where it should. Byte counts are
 * the host's, whose slot pool alone is 4 KB (the radar's: 1 KB).
 */
#include <unity.h>

#include <ArduinoJson.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "util/feed_answer.h"
#include "util/heap_room.h"
#include "util/json_budget.h"
#include "util/json_stream.h"

void setUp() {}
void tearDown() {}

namespace {

using util::feed_answer::Answer;
using util::feed_answer::Area;
using util::feed_answer::End;
using util::feed_answer::Reading;
using util::feed_answer::Request;
using util::json_stream::Input;
using Code = DeserializationError::Code;

constexpr double kPi = 3.14159265358979323846;

/** The circle asked about in these tests: 25 km round O'Hare. */
const Area kOHare{41.9786, -87.9048, 25.0f};

/**
 * Memory for each aircraft here: 4 times the radar's 4 KB, as the host's slot
 * pool alone is 4 KB.
 */
constexpr size_t kAircraftBytes = 16 * 1024;

/** A text handed out a byte at a time, cut after `length` bytes. */
class TextSource final : public util::json_stream::ByteSource {
 public:
  explicit TextSource(const std::string& text, size_t length = SIZE_MAX)
      : text_(text), length_(length < text.size() ? length : text.size()) {}

  int read() override {
    ++reads_;
    return pos_ < length_ ? static_cast<unsigned char>(text_[pos_++]) : -1;
  }

  /** Bytes handed out so far. */
  size_t position() const { return pos_; }
  /** Times asked, the end included. */
  size_t reads() const { return reads_; }

 private:
  const std::string& text_;
  size_t length_;
  size_t pos_ = 0;
  size_t reads_ = 0;
};

/** A heap that counts the bytes it holds (JsonBudget's headers included). */
size_t s_live = 0;
size_t s_peak = 0;
std::map<void*, size_t> s_sizes;
void note(void* p, size_t bytes) {
  s_sizes[p] = bytes;
  s_live += bytes;
  s_peak = s_live > s_peak ? s_live : s_peak;
}
void* countedAllocate(size_t bytes) {
  void* p = malloc(bytes);
  note(p, bytes);
  return p;
}
void* countedReallocate(void* ptr, size_t bytes) {
  s_live -= s_sizes[ptr];
  s_sizes.erase(ptr);
  void* p = realloc(ptr, bytes);
  note(p, bytes);
  return p;
}
void countedDeallocate(void* ptr) {
  s_live -= s_sizes[ptr];
  s_sizes.erase(ptr);
  free(ptr);
}
const util::JsonBudget::Heap kCountedHeap = {countedAllocate, countedReallocate,
                                             countedDeallocate};
void resetHeap() {
  s_live = 0;
  s_peak = 0;
}

/** The radar's heap allowing s_blocks_left more blocks, then none. */
int s_blocks_left = INT_MAX;
bool spareSome(size_t) { return s_blocks_left-- > 0; }

/** What the radar would show: the nearest 64 offered, by hex, as read (JSON). */
class Shown final : public util::feed_answer::Sink {
 public:
  void offer(JsonObjectConst plane, float distance_km) override {
    const int slot = nearest_.slotFor(distance_km);
    if (slot < 0) {
      return;
    }
    if (static_cast<size_t>(slot) >= hex_.size()) {
      hex_.resize(slot + 1);
      json_.resize(slot + 1);
    }
    hex_[slot] = plane["hex"] | "";
    json_[slot].clear();
    serializeJson(plane, json_[slot]);
  }

  std::map<std::string, std::string> byHex() const {
    std::map<std::string, std::string> out;
    for (size_t i = 0; i < nearest_.size(); ++i) {
      out[hex_[i]] = json_[i];
    }
    return out;
  }
  size_t size() const { return nearest_.size(); }

 private:
  util::feed_answer::Nearest<64> nearest_;
  std::vector<std::string> hex_;
  std::vector<std::string> json_;
};

Request request(bool with_ground = false, util::JsonBudget::CanSpare can_spare = nullptr,
                const Area& area = kOHare) {
  return Request{area, with_ground, kAircraftBytes, can_spare, &kCountedHeap};
}

/** `text` read as the radar reads an answer, the first `length` bytes of it. */
Answer readText(const std::string& text, Shown& shown, const Request& req = request(),
                size_t length = SIZE_MAX) {
  JsonDocument filter;
  util::feed_answer::buildFilter(filter);
  TextSource source(text, length);
  return util::feed_answer::read(source, filter, req, shown);
}

uint8_t readingOf(const Answer& answer) {
  return static_cast<uint8_t>(util::feed_answer::judge(answer));
}
uint8_t code(Reading r) { return static_cast<uint8_t>(r); }
uint8_t code(End e) { return static_cast<uint8_t>(e); }

/** A point `km` from O'Hare on bearing `deg`. */
void pointAt(double km, double deg, double* lat, double* lon) {
  const double rad = deg * kPi / 180.0;
  *lat = kOHare.lat + km * std::cos(rad) / 111.2;
  *lon = kOHare.lon + km * std::sin(rad) / (111.2 * std::cos(kOHare.lat * kPi / 180.0));
}

/**
 * One aircraft as readsb writes it, `km` from O'Hare, with fields the filter
 * drops (nested ones too) between the ones it keeps. `alt`: its alt_baro.
 */
std::string aircraft(int id, double km, double deg = 90.0, const char* alt = nullptr) {
  double lat = 0.0;
  double lon = 0.0;
  pointAt(km, deg, &lat, &lon);
  char altitude[16];
  snprintf(altitude, sizeof(altitude), "%d", 3000 + id % 9000);
  char out[640];
  snprintf(out, sizeof(out),
           "{\"hex\":\"a%05x\",\"type\":\"adsb_icao\",\"flight\":\"UAL%-5d\",\"r\":\"N%dUA\","
           "\"t\":\"B738\",\"desc\":\"BOEING 737-800\",\"alt_baro\":%s,\"gs\":%.1f,"
           "\"track\":%.2f,\"baro_rate\":%d,\"squawk\":\"%04d\",\"category\":\"A3\","
           "\"nav_qnh\":1013.6,\"lat\":%.6f,\"lon\":%.6f,\"nic\":8,\"mlat\":[],"
           "\"tisb\":[\"lat\",\"lon\"],\"lastPosition\":{\"lat\":1.5,\"lon\":2.5},"
           "\"seen_pos\":%.3f,\"dbFlags\":0,\"messages\":%d,\"rssi\":-21.4,\"dst\":%.3f,"
           "\"dir\":%.1f}",
           id, id % 100000, id, alt != nullptr ? alt : altitude, 140.0 + id % 300,
           (id * 37) % 360 + 0.25, (id % 7 - 3) * 512, id % 7000, lat, lon, (id % 30) * 0.1,
           1000 + id, km / 1.852, deg);
  return out;
}

/** An answer of `planes`: "ac" first as both feeds send it, or "now" first. */
std::string answerOf(const std::vector<std::string>& planes, bool now_first = false) {
  std::string list = "\"ac\":[\n";
  for (size_t i = 0; i < planes.size(); ++i) {
    list += i == 0 ? "" : ",\n";
    list += planes[i];
  }
  list += "\n]";
  const std::string now = "\"now\": 1792531860000";
  if (now_first) {
    return "{" + now + ", " + list + ", \"msg\": \"No error\"}";
  }
  return "{" + list + "\n,\"msg\": \"No error\"\n," + now + "\n,\"total\": " +
         std::to_string(planes.size()) + "\n}\n";
}

/** `count` aircraft 1-24 km out, every fourth on the ground. */
std::vector<std::string> nearbySky(int count) {
  std::vector<std::string> planes;
  for (int i = 0; i < count; ++i) {
    const char* alt = i % 4 == 0 ? "\"ground\"" : nullptr;
    planes.push_back(aircraft(i, 1.0 + (i * 7) % 24, i * 31.0, alt));
  }
  return planes;
}

/**
 * Six aircraft of adsb.lol's answer for 20 nm round O'Hare on a weekday
 * evening, as received but for the other 80: four airborne (one TIS-B), two
 * on the ground at the airport.
 */
const std::string kRealAnswer = R"({"ac":[
{"hex":"abb089","type":"adsb_icao","flight":"UAL2144 ","r":"N852UA","t":"A319","alt_baro":11500,"alt_geom":11900,"gs":300.1,"track":251.75,"baro_rate":3328,"squawk":"6216","emergency":"none","category":"A3","nav_qnh":1023.2,"nav_altitude_mcp":15008,"lat":41.921826,"lon":-88.268631,"nic":8,"rc":186,"seen_pos":0.136,"version":2,"nic_baro":1,"nac_p":9,"nac_v":1,"sil":3,"sil_type":"perhour","gva":2,"sda":2,"alert":0,"spi":0,"mlat":[],"tisb":[],"messages":5845,"seen":0.1,"rssi":-7.2,"dst":16.565,"dir":258.3},
{"hex":"a25e58","type":"adsb_icao","flight":"CXK216  ","r":"N25153","t":"C172","dbFlags":8,"alt_baro":550,"alt_geom":700,"gs":75.2,"track":193.85,"true_heading":194.06,"baro_rate":512,"geom_rate":576,"squawk":"1200","emergency":"none","category":"A1","lat":41.903366,"lon":-88.246557,"nic":9,"rc":75,"seen_pos":0.437,"version":2,"nic_baro":1,"nac_p":10,"nac_v":2,"sil":3,"sil_type":"perhour","gva":2,"sda":2,"alert":0,"spi":0,"mlat":[],"tisb":["geom_rate"],"messages":17127,"seen":0.2,"rssi":-12.8,"dst":15.886,"dir":253.6},
{"hex":"ab8836","type":"adsb_icao","flight":"N842KP  ","r":"N842KP","t":"C172","alt_baro":750,"alt_geom":900,"gs":59.5,"track":196.61,"baro_rate":-448,"geom_rate":-448,"squawk":"1200","emergency":"none","category":"A1","nav_qnh":1023.2,"nav_altitude_mcp":1792,"lat":41.917078,"lon":-88.241932,"nic":9,"rc":75,"seen_pos":0.167,"version":2,"nic_baro":1,"nac_p":10,"nac_v":2,"sil":3,"sil_type":"perhour","gva":2,"sda":2,"alert":0,"spi":0,"mlat":[],"tisb":["geom_rate"],"messages":13896,"seen":0.1,"rssi":-2.4,"dst":15.470,"dir":256.3},
{"hex":"~273e1b","type":"tisb_other","alt_baro":4800,"alt_geom":7175,"gs":274.3,"track":146.60,"geom_rate":64,"lat":41.764945,"lon":-88.171223,"nic":6,"rc":926,"seen_pos":0.313,"nac_p":6,"nac_v":0,"sil":2,"sil_type":"unknown","alert":0,"spi":0,"mlat":[],"tisb":["altitude","alt_baro","alt_geom","gs","track","geom_rate","lat","lon","nic","rc","nac_p","nac_v","sil","sil_type"],"messages":209,"seen":0.3,"rssi":-26.6,"dst":17.498,"dir":223.0},
{"hex":"a00ce0","type":"adsb_icao","flight":"RPA4510 ","r":"N102HQ","t":"E75L","alt_baro":"ground","gs":10.2,"true_heading":180.00,"squawk":"3670","emergency":"none","category":"A3","lat":41.988338,"lon":-87.931440,"nic":8,"rc":186,"seen_pos":1.000,"version":2,"nac_p":10,"nac_v":2,"sil":3,"sil_type":"perhour","sda":2,"alert":1,"spi":1,"mlat":[],"tisb":[],"messages":47329,"seen":0.7,"rssi":-31.0,"dst":1.323,"dir":296.2},
{"hex":"a38682","type":"adsb_icao","flight":"AAL3163 ","r":"N326VN","t":"B38M","alt_baro":"ground","gs":13.2,"true_heading":180.00,"squawk":"1704","emergency":"none","category":"A3","lat":41.983112,"lon":-87.931425,"nic":8,"rc":186,"seen_pos":0.601,"version":2,"nac_p":10,"nac_v":2,"sil":3,"sil_type":"perhour","sda":2,"mlat":[],"tisb":[],"messages":91706,"seen":0.6,"rssi":-6.2,"dst":1.216,"dir":282.9}
]
,"msg": "No error"
,"now": 1792632711500
,"total": 86
,"ctime": 1792632711500
,"ptime": 0
}
)";
/** The 20 nm it was asked for. */
const Area kRealArea{41.9786, -87.9048, 20.0f * 1.852f};

}  // namespace

// --- where deserializeJson() leaves the input --------------------------------------

// The walk hands each aircraft to deserializeJson() and goes on from where
// it stopped: it must stop at the object's closing brace, not a byte later.
void test_arduinojson_reads_an_object_to_its_closing_brace() {
  JsonDocument filter;
  util::feed_answer::buildFilter(filter);
  for (const std::string text :
       {"{\"hex\":\"a1\",\"lat\":41.9}X", "{\"hex\":\"a1\" , \"x\":[1,{\"y\":\"}]\"}] }X", "{}X",
        "{ \"lat\" : 41.9 , \"dir\" : 12 }X"}) {
    TextSource source(text);
    Input in(source);
    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT(DeserializationError::Ok,
                          deserializeJson(doc, in, DeserializationOption::Filter(filter)).code());
    // Not one byte past the brace was taken from the source...
    TEST_ASSERT_EQUAL_size_t(text.size() - 1, source.position());
    // ...so the next is the walk's.
    TEST_ASSERT_EQUAL_INT('X', in.read());
  }
  // A number ends only at the byte after it, which ArduinoJson keeps in its
  // latch and drops: the comma is lost. Only objects are handed to it.
  for (const std::string number : {"125,X", "12.5,X"}) {
    TextSource source(number);
    Input in(source);
    JsonDocument doc;
    TEST_ASSERT_EQUAL_INT(DeserializationError::Ok,
                          deserializeJson(doc, in, DeserializationOption::Filter(filter)).code());
    TEST_ASSERT_EQUAL_size_t(number.size() - 1, source.position());
    TEST_ASSERT_EQUAL_INT('X', in.read());
  }
}

// --- json_stream ---------------------------------------------------------------------

void test_strings_and_their_escapes() {
  const std::string text = R"(a\"b\\c\/d\b\f\n\r\t\u0041\u00e9")" "X";
  TextSource source(text);
  Input in(source);
  char out[32];
  bool fits = true;
  TEST_ASSERT_EQUAL_INT(Code::Ok, util::json_stream::readString(in, out, sizeof(out), &fits));
  // é is not ASCII: no key the walk looks for, though read to the end.
  TEST_ASSERT_FALSE(fits);
  TEST_ASSERT_EQUAL_STRING("a\"b\\c/d\b\f\n\r\tA", out);
  TEST_ASSERT_EQUAL_INT('X', in.read());

  const std::string long_key = std::string(100000, 'k') + "\":1";
  TextSource long_source(long_key);
  Input long_in(long_source);
  char key[4];
  TEST_ASSERT_EQUAL_INT(Code::Ok, util::json_stream::readString(long_in, key, sizeof(key), &fits));
  TEST_ASSERT_FALSE(fits);
  TEST_ASSERT_EQUAL_STRING("kkk", key);
  TEST_ASSERT_EQUAL_INT(':', long_in.read());

  for (const std::string& bad : {std::string("\\x\""), std::string("\\u00g0\"")}) {
    TextSource s(bad);
    Input i(s);
    TEST_ASSERT_EQUAL_INT(Code::InvalidInput,
                          util::json_stream::readString(i, key, sizeof(key), &fits));
  }
  for (const std::string& cut : {std::string("abc"), std::string("ab\\"), std::string("\\u00")}) {
    TextSource s(cut);
    Input i(s);
    TEST_ASSERT_EQUAL_INT(Code::IncompleteInput,
                          util::json_stream::readString(i, key, sizeof(key), &fits));
  }
}

void test_numbers() {
  struct Case {
    const char* text;
    double value;
  };
  for (const Case c :
       {Case{"1792531860000,", 1792531860000.0}, Case{"-12.5e3}", -12500.0},
        Case{"1.5E-2 ", 0.015}, Case{"0]", 0.0}, Case{"1792531860.25,", 1792531860.25}}) {
    const std::string text = c.text;
    TextSource source(text);
    Input in(source);
    double v = -1.0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(Code::Ok, util::json_stream::readNumber(in, &v), c.text);
    TEST_ASSERT_EQUAL_DOUBLE(c.value, v);
    TEST_ASSERT_EQUAL_INT(text.back(), in.read());  // the byte after it is still there
  }
  for (const std::string bad : {"x", "-x", "1.x", "1e+x", "12ab", "\"12\""}) {
    TextSource source(bad);
    Input in(source);
    double v = -1.0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(Code::InvalidInput, util::json_stream::readNumber(in, &v),
                                  bad.c_str());
    TEST_ASSERT_EQUAL_DOUBLE(-1.0, v);
  }
  // A number the text ends right after may be missing digits.
  for (const std::string cut : {"", "-", "17.", "1e", "1e+", "179253186000"}) {
    TextSource source(cut);
    Input in(source);
    double v = -1.0;
    TEST_ASSERT_EQUAL_INT(Code::IncompleteInput, util::json_stream::readNumber(in, &v));
  }
}

// However many digits, a number stays finite unless it is beyond a double.
void test_long_numbers() {
  struct Case {
    std::string text;
    double value;
  };
  const std::string zeros(400, '0');
  for (const Case& c :
       {Case{"1761000000000." + std::string(400, '7') + ",", 1761000000000.7778},
        Case{"1761000000000." + std::string(128 * 1024, '7') + ",", 1761000000000.7778},
        Case{"1234567890123456789012345678901234567890,", 1.2345678901234568e39},
        Case{"1" + zeros + "e-395,", 1.0e5}, Case{"0." + zeros + "5,", 0.0},
        Case{"-0.000000000000000000001234,", -1.234e-21},
        Case{"1e999999999999,", static_cast<double>(INFINITY)}}) {
    TextSource source(c.text);
    Input in(source);
    double v = -1.0;
    TEST_ASSERT_EQUAL_INT(Code::Ok, util::json_stream::readNumber(in, &v));
    if (std::isinf(c.value)) {
      TEST_ASSERT_TRUE(std::isinf(v));
    } else {
      TEST_ASSERT_DOUBLE_WITHIN(std::fabs(c.value) * 1e-13, c.value, v);
    }
    TEST_ASSERT_EQUAL_INT(',', in.read());
  }
  // Beyond a double: infinite, as ArduinoJson reads "1e400" too.
  const std::string huge = "1" + std::string(320, '1') + ",";
  TextSource source(huge);
  Input in(source);
  double v = 0.0;
  TEST_ASSERT_EQUAL_INT(Code::Ok, util::json_stream::readNumber(in, &v));
  TEST_ASSERT_TRUE(std::isinf(v));
}

void test_skipping_values() {
  // Brackets and escaped quotes inside strings are not structure.
  for (const std::string value :
       {"\"a \\\"]}\\\" [{\"", "{\"a\":[1,{\"b\":\"}],[\"},[[]],{}],\"c\":null}", "-1.5e3",
        "true", "null", "[]", "{}"}) {
    const std::string text = " " + value + ",X";
    TextSource source(text);
    Input in(source);
    TEST_ASSERT_EQUAL_INT_MESSAGE(Code::Ok, util::json_stream::skipValue(in), value.c_str());
    TEST_ASSERT_EQUAL_INT(',', in.read());
  }
  for (const std::string bad : {"{\"a\":[}", "[1}", ",", "}"}) {
    TextSource source(bad);
    Input in(source);
    TEST_ASSERT_EQUAL_INT_MESSAGE(Code::InvalidInput, util::json_stream::skipValue(in),
                                  bad.c_str());
  }
  for (const std::string cut : {"", " ", "[1,", "{\"a\":\"]"}) {
    TextSource source(cut);
    Input in(source);
    TEST_ASSERT_EQUAL_INT(Code::IncompleteInput, util::json_stream::skipValue(in));
  }
  // As deep as allowed, and one deeper.
  const size_t deepest = util::json_stream::kMaxDepth;
  const std::string ok = std::string(deepest, '[') + std::string(deepest, ']');
  TextSource ok_source(ok);
  Input ok_in(ok_source);
  TEST_ASSERT_EQUAL_INT(Code::Ok, util::json_stream::skipValue(ok_in));
  const std::string deep = std::string(deepest + 1, '[') + std::string(deepest + 1, ']');
  TextSource deep_source(deep);
  Input deep_in(deep_source);
  TEST_ASSERT_EQUAL_INT(Code::TooDeep, util::json_stream::skipValue(deep_in));
}

// --- reading an answer -----------------------------------------------------------------

void test_reads_either_order_of_keys() {
  const std::vector<std::string> planes = nearbySky(10);
  Shown ac_first;
  Shown now_first;
  const Answer a = readText(answerOf(planes), ac_first);
  const Answer b = readText(answerOf(planes, true), now_first);
  for (const Answer& answer : {a, b}) {
    TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(answer.end));
    TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(answer));
    TEST_ASSERT_TRUE(answer.list_whole);
    TEST_ASSERT_EQUAL_size_t(10, answer.records);
    TEST_ASSERT_EQUAL_size_t(10, answer.in_area);
    TEST_ASSERT_EQUAL_size_t(7, answer.offered);  // three on the ground
    TEST_ASSERT_EQUAL_DOUBLE(1792531860000.0, answer.now);
    TEST_ASSERT_FALSE(answer.error);  // set (to Ok) though nothing went wrong
  }
  TEST_ASSERT_TRUE(ac_first.byHex() == now_first.byHex());
  // Only the filter's fields are kept.
  const std::string one = ac_first.byHex().at("a00001");
  TEST_ASSERT_NOT_NULL(strstr(one.c_str(), "\"flight\":\"UAL1    \""));
  TEST_ASSERT_NULL(strstr(one.c_str(), "desc"));
  TEST_ASSERT_NULL(strstr(one.c_str(), "lastPosition"));
}

void test_other_members_are_passed_over() {
  const std::string text =
      "{\"msg\":\"a \\\"]}\\\" [{\",\"nested\":{\"a\":[1,{\"b\":\"}],[\"},[[]],{}],"
      "\"now\":5,\"c\":null,\"d\":true},\"" +
      std::string(1000, 'k') + "\":[1],\"acx\":[" + aircraft(9, 3.0) +
      "],\"ac\\u0000\":[],\"ac\":[" +
      aircraft(1, 2.0) + "," + aircraft(2, 3.0) +
      "],\"x\":-1.5e3,\"\\u006eow\":1792531860000,\"tail\":[false,\"ac\",{\"now\":7}]}";
  Shown shown;
  const Answer answer = readText(text, shown);
  TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(answer.end));
  TEST_ASSERT_EQUAL_size_t(2, answer.records);  // "acx" is not the list
  TEST_ASSERT_EQUAL_size_t(2, shown.size());
  // The escaped key is "now"; those inside other values are not the clock.
  TEST_ASSERT_EQUAL_DOUBLE(1792531860000.0, answer.now);
}

void test_a_value_nested_too_deep_fails_cleanly() {
  Shown shown;
  // Iterative: a hundred thousand brackets cost no stack, only a TooDeep.
  const std::string deep = "{\"x\":" + std::string(100000, '[') + "}";
  Answer answer = readText(deep, shown);
  TEST_ASSERT_EQUAL_UINT8(code(End::kMalformed), code(answer.end));
  TEST_ASSERT_EQUAL_INT(DeserializationError::TooDeep, answer.error.code());
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kMalformed), readingOf(answer));

  // Inside an aircraft ArduinoJson counts, and allows what the whole answer
  // parsed at once did (nesting limit 10, two of it the answer and its list).
  JsonDocument filter;
  util::feed_answer::buildFilter(filter);
  JsonDocument whole_filter;
  whole_filter["ac"].add(filter.as<JsonVariantConst>());
  for (int depth = 1; depth <= 12; ++depth) {
    const std::string plane = "{\"hex\":\"a1\",\"x\":" + std::string(depth, '[') +
                              std::string(depth, ']') + ",\"lat\":41.98,\"lon\":-87.9}";
    const std::string text = "{\"ac\":[" + plane + "]}";
    JsonDocument doc;
    const bool whole_ok =
        !deserializeJson(doc, text, DeserializationOption::Filter(whole_filter));
    Shown s;
    answer = readText(text, s);
    TEST_ASSERT_EQUAL_MESSAGE(whole_ok, answer.end == End::kWhole, std::to_string(depth).c_str());
    if (!whole_ok) {
      TEST_ASSERT_EQUAL_INT(DeserializationError::TooDeep, answer.error.code());
    }
  }
}

// Cut off at every byte: never a crash, a hang or a "malformed", and what
// is shown is always part of what the whole answer shows.
void test_cut_short_at_every_byte() {
  const std::vector<std::string> sky = nearbySky(6);
  struct Body {
    std::string text;
    Area area;
  };
  for (const Body& body : {Body{answerOf(sky), kOHare}, Body{answerOf(sky, true), kOHare},
                           Body{kRealAnswer, kRealArea}}) {
    const std::string& text = body.text;
    Shown whole_shown;
    const Answer whole = readText(text, whole_shown, request(false, nullptr, body.area));
    TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(whole.end));
    const std::map<std::string, std::string> all = whole_shown.byHex();
    const size_t closing = text.rfind('}') + 1;  // the walk reads no further
    size_t offered = 0;
    for (size_t cut = 0; cut <= text.size(); ++cut) {
      JsonDocument filter;
      util::feed_answer::buildFilter(filter);
      TextSource source(text, cut);
      Shown shown;
      const Answer answer =
          util::feed_answer::read(source, filter, request(false, nullptr, body.area), shown);
      const std::string at = std::to_string(cut);
      // The source is not asked again once it has ended.
      TEST_ASSERT_LESS_OR_EQUAL_size_t_MESSAGE(cut + 1, source.reads(), at.c_str());
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(code(cut >= closing ? End::kWhole : End::kCutShort),
                                      code(answer.end), at.c_str());
      TEST_ASSERT_NOT_EQUAL_UINT8(code(Reading::kMalformed), readingOf(answer));
      TEST_ASSERT_GREATER_OR_EQUAL_size_t_MESSAGE(offered, answer.offered, at.c_str());
      offered = answer.offered;
      for (const auto& plane : shown.byHex()) {
        TEST_ASSERT_TRUE_MESSAGE(all.count(plane.first) == 1, at.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(all.at(plane.first).c_str(), plane.second.c_str(),
                                         at.c_str());
      }
      if (answer.offered > 0 && answer.end != End::kWhole) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(code(Reading::kCutShort), readingOf(answer), at.c_str());
      }
    }
    TEST_ASSERT_EQUAL_size_t(whole.offered, offered);
  }
}

void test_an_answer_without_a_list() {
  for (const std::string text :
       {"{\"ac\":{\"hex\":\"a1\"},\"now\":1792531860000}", "{\"ac\":null}", "{\"ac\":\"none\"}",
        "{}", "[{\"hex\":\"a1\"}]", "\"Service unavailable\"", "42"}) {
    Shown shown;
    const Answer answer = readText(text, shown);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(code(End::kWhole), code(answer.end), text.c_str());
    TEST_ASSERT_FALSE(answer.list);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(code(Reading::kNoList), readingOf(answer), text.c_str());
  }
  for (const std::string text : {"<html><body>Bad gateway</body></html>", "{\"ac\":[}",
                                 "{\"ac\":[] \"now\":1}", "{\"ac\" []}", "{ac:[]}"}) {
    Shown shown;
    const Answer answer = readText(text, shown);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(code(Reading::kMalformed), readingOf(answer), text.c_str());
    TEST_ASSERT_EQUAL_INT(DeserializationError::InvalidInput, answer.error.code());
  }
  // Nothing at all is not malformed, but no answer either.
  Shown shown;
  const Answer empty = readText("", shown);
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kUnusable), readingOf(empty));
}

void test_members_that_are_not_aircraft_and_null_fields() {
  const std::string text =
      "{\"ac\":[null,1,\"x\",[{\"lat\":41.98}],{},"
      "{\"hex\":\"a1\",\"lat\":null,\"lon\":-87.9},"
      "{\"hex\":\"a2\",\"flight\":null,\"t\":null,\"alt_baro\":null,\"lat\":41.98,\"lon\":-87.9}]}";
  Shown shown;
  const Answer answer = readText(text, shown);
  TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(answer.end));
  TEST_ASSERT_EQUAL_size_t(7, answer.records);
  TEST_ASSERT_EQUAL_size_t(1, answer.positioned);
  TEST_ASSERT_EQUAL_size_t(1, shown.size());
  TEST_ASSERT_EQUAL_STRING(
      "{\"hex\":\"a2\",\"flight\":null,\"t\":null,\"alt_baro\":null,\"lat\":41.98,\"lon\":-87.9}",
      shown.byHex().at("a2").c_str());
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(answer));
}

// An aircraft bigger than its budget ends the reading; those before it stay.
void test_an_aircraft_too_big_ends_the_reading() {
  const std::string flight = "\"flight\":\"" + std::string(100000, 'F') + "\"";
  const std::string big = "{\"hex\":\"bad\",\"lat\":41.98,\"lon\":-87.9," + flight + "}";
  std::vector<std::string> planes = {aircraft(1, 2.0), aircraft(2, 3.0), aircraft(3, 4.0)};
  planes.push_back(big);
  planes.push_back(aircraft(4, 5.0));
  Shown shown;
  Answer answer = readText(answerOf(planes), shown);
  TEST_ASSERT_EQUAL_UINT8(code(End::kRefused), code(answer.end));
  TEST_ASSERT_EQUAL_size_t(3, shown.size());
  TEST_ASSERT_LESS_OR_EQUAL_size_t(kAircraftBytes, answer.peak_bytes);
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kCutShort), readingOf(answer));
  TEST_ASSERT_EQUAL_DOUBLE(0.0, answer.now);  // after the list: the Date header stands in

  // First in the list: nothing usable, and the feed is to blame.
  Shown first;
  answer = readText(answerOf({big, aircraft(1, 2.0)}), first);
  TEST_ASSERT_EQUAL_UINT8(code(End::kRefused), code(answer.end));
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kUnusable), readingOf(answer));

  // A key is read whole before the filter is asked about it: a long one
  // costs memory too, up to the budget.
  Shown keyed;
  const std::string long_key = "{\"" + std::string(100000, 'k') + "\":1}";
  answer = readText(answerOf({aircraft(1, 2.0), long_key}), keyed);
  TEST_ASSERT_EQUAL_UINT8(code(End::kRefused), code(answer.end));
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kCutShort), readingOf(answer));

  // A long value the filter drops is passed over and costs nothing.
  Shown dropped;
  const std::string desc = "{\"hex\":\"a7\",\"desc\":\"" + std::string(100000, 'd') +
                           "\",\"lat\":41.98,\"lon\":-87.9}";
  answer = readText(answerOf({desc}), dropped);
  TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(answer.end));
  TEST_ASSERT_EQUAL_size_t(1, dropped.size());
  TEST_ASSERT_LESS_THAN_size_t(8 * 1024, answer.peak_bytes);
}

// The radar's heap saying no is its own problem, unless aircraft were read.
void test_the_heap_running_short() {
  const std::string text = answerOf(nearbySky(10));
  s_blocks_left = INT_MAX;
  Shown all;
  readText(text, all, request(false, spareSome));
  const int asked = INT_MAX - s_blocks_left;  // blocks the whole answer took

  s_blocks_left = 0;
  Shown none;
  Answer answer = readText(text, none, request(false, spareSome));
  TEST_ASSERT_EQUAL_UINT8(code(End::kNoRoom), code(answer.end));
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kNoRoom), readingOf(answer));

  s_blocks_left = asked / 2;  // enough for a few aircraft
  Shown some;
  answer = readText(text, some, request(false, spareSome));
  TEST_ASSERT_EQUAL_UINT8(code(End::kNoRoom), code(answer.end));
  TEST_ASSERT_GREATER_THAN_size_t(0, answer.in_area);
  TEST_ASSERT_LESS_THAN_size_t(10, answer.records);
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kCutShort), readingOf(answer));
  s_blocks_left = INT_MAX;
}

/**
 * The radar's heaps while a feed answers, for util::heap_room: DRAM short,
 * and an RTC heap of s_rtc_free bytes in one piece, which a document's
 * blocks take first (services::memory::kDocumentHeap). Host blocks are
 * about twice the radar's, and so is this RTC heap (the radar's: ~7.9 KB).
 */
size_t s_dram_free = 0;
size_t s_dram_largest = 0;
size_t s_rtc_free = 0;
size_t s_dram_taken = 0;
int s_rtc_asked = 0;
const util::heap_room::Heaps kModelHeaps = {
    [] { return s_dram_free; },
    [] { return s_dram_largest; },
    [] {
      ++s_rtc_asked;
      return s_rtc_free;
    },
};
bool spareModel(size_t bytes) { return util::heap_room::canSpare(bytes, kModelHeaps); }
struct ModelBlock {
  size_t size;
  bool rtc;
  double align;
};
void* modelAllocate(size_t bytes) {
  auto* block = static_cast<ModelBlock*>(malloc(sizeof(ModelBlock) + bytes));
  block->size = bytes;
  block->rtc = s_rtc_free >= bytes;
  (block->rtc ? s_rtc_free : s_dram_free) -= bytes;
  s_dram_taken += block->rtc ? 0 : bytes;
  return block + 1;
}
void modelDeallocate(void* ptr) {
  ModelBlock* block = static_cast<ModelBlock*>(ptr) - 1;
  (block->rtc ? s_rtc_free : s_dram_free) += block->size;
  free(block);
}
void* modelReallocate(void* ptr, size_t bytes) {
  void* moved = modelAllocate(bytes);
  const size_t old = (static_cast<ModelBlock*>(ptr) - 1)->size;
  memcpy(moved, ptr, old < bytes ? old : bytes);
  modelDeallocate(ptr);
  return moved;
}
const util::JsonBudget::Heap kModelHeap = {modelAllocate, modelReallocate, modelDeallocate};

// DRAM down to its reserve stops a document only when the RTC heap, where
// its blocks go first, has no room either.
void test_the_rtc_heap_holds_an_aircraft() {
  using util::heap_room::kPieceLeft;
  using util::heap_room::kReserve;
  using util::heap_room::kWalkFrom;
  s_rtc_free = 0;
  s_rtc_asked = 0;
  // DRAM has room: the RTC heap is not walked.
  s_dram_free = 4096 + kReserve;
  s_dram_largest = 4096 + kPieceLeft;
  TEST_ASSERT_TRUE(spareModel(4096));
  s_dram_largest = 0;
  TEST_ASSERT_TRUE(spareModel(kWalkFrom - 1));  // nor DRAM, for a small block
  TEST_ASSERT_EQUAL_INT(0, s_rtc_asked);
  // Below the reserve, or no piece left beside a large block: only with room in RTC.
  struct Dram {
    size_t free;
    size_t largest;
  };
  for (const Dram dram : {Dram{4096 + kReserve - 1, 8192}, Dram{4096 + kReserve, 6143}}) {
    s_dram_free = dram.free;
    s_dram_largest = dram.largest;
    s_rtc_free = 4096 + kPieceLeft - 1;
    TEST_ASSERT_FALSE(spareModel(4096));
    s_rtc_free = 4096 + kPieceLeft;
    TEST_ASSERT_TRUE(spareModel(4096));
  }

  const std::string text = answerOf(nearbySky(40));
  JsonDocument filter;
  util::feed_answer::buildFilter(filter);
  const Request model{kOHare, false, kAircraftBytes, spareModel, &kModelHeap};
  // DRAM 1 KB under the reserve, the RTC heap free: read whole, no DRAM taken.
  s_dram_free = s_dram_largest = kReserve - 1024;
  s_rtc_free = 16 * 1024;
  s_dram_taken = 0;
  Shown all;
  TextSource source(text);
  Answer answer = util::feed_answer::read(source, filter, model, all);
  TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(answer.end));
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(answer));
  TEST_ASSERT_EQUAL_size_t(30, all.size());  // ten on the ground
  TEST_ASSERT_EQUAL_size_t(0, s_dram_taken);
  TEST_ASSERT_EQUAL_size_t(16 * 1024, s_rtc_free);  // all given back
  // The RTC heap taken by something else: nothing read, the radar's problem.
  s_rtc_free = 2 * 1024;
  Shown none;
  TextSource again(text);
  answer = util::feed_answer::read(again, filter, model, none);
  TEST_ASSERT_EQUAL_UINT8(code(End::kNoRoom), code(answer.end));
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kNoRoom), readingOf(answer));
  TEST_ASSERT_EQUAL_size_t(0, s_dram_taken);
}

void test_the_nearest_are_kept() {
  util::feed_answer::Nearest<3> nearest;
  TEST_ASSERT_EQUAL_INT(0, nearest.slotFor(5.0f));
  TEST_ASSERT_EQUAL_INT(1, nearest.slotFor(1.0f));
  TEST_ASSERT_EQUAL_INT(2, nearest.slotFor(4.0f));
  TEST_ASSERT_EQUAL_INT(0, nearest.slotFor(2.0f));   // takes the 5 km one's slot
  TEST_ASSERT_EQUAL_INT(-1, nearest.slotFor(4.0f));  // no nearer than the farthest
  TEST_ASSERT_EQUAL_INT(2, nearest.slotFor(3.0f));
  TEST_ASSERT_EQUAL_size_t(3, nearest.size());
  nearest.clear();
  TEST_ASSERT_EQUAL_size_t(0, nearest.size());

  // 200 airborne in no order of distance, and 20 on the ground nearer still.
  std::vector<std::string> planes;
  for (int i = 0; i < 200; ++i) {
    planes.push_back(aircraft(i, 2.0 + ((i * 73) % 200) * 0.1, i * 13.0));
  }
  for (int i = 200; i < 220; ++i) {
    planes.push_back(aircraft(i, 0.5, i * 13.0, "\"ground\""));
  }
  const std::string text = answerOf(planes);
  Shown shown;
  Answer answer = readText(text, shown);
  TEST_ASSERT_EQUAL_size_t(220, answer.in_area);
  TEST_ASSERT_EQUAL_size_t(200, answer.offered);
  TEST_ASSERT_EQUAL_size_t(64, shown.size());
  for (int i = 0; i < 200; ++i) {
    char hex[8];
    snprintf(hex, sizeof(hex), "a%05x", i);
    const bool near = (i * 73) % 200 < 64;  // 2.0 to 8.3 km
    TEST_ASSERT_EQUAL_MESSAGE(near, shown.byHex().count(hex) == 1, hex);
  }
  Shown with_ground;
  answer = readText(text, with_ground, request(true));
  TEST_ASSERT_EQUAL_size_t(220, answer.offered);
  TEST_ASSERT_EQUAL_size_t(1, with_ground.byHex().count("a000c8"));  // on the ground, 0.5 km
}

// A feed that ignores the radius: most of what it sends is elsewhere.
void test_an_answer_for_elsewhere() {
  std::vector<std::string> planes;
  for (int i = 0; i < 10; ++i) {
    planes.push_back(aircraft(i, i < 5 ? 3.0 : 300.0));
  }
  Shown half;
  Answer answer = readText(answerOf(planes), half);
  TEST_ASSERT_EQUAL_size_t(10, answer.positioned);
  TEST_ASSERT_EQUAL_size_t(5, answer.in_area);
  TEST_ASSERT_EQUAL_size_t(5, half.size());  // those far out are not drawn
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(answer));
  planes[4] = aircraft(4, 300.0);
  Shown most;
  answer = readText(answerOf(planes), most);
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kElsewhere), readingOf(answer));
  // Cut short too: judged on what was read.
  Shown cut;
  const std::string text = answerOf(planes);
  answer = readText(text, cut, request(), text.rfind("{\"hex\"") + 10);
  TEST_ASSERT_EQUAL_UINT8(code(End::kCutShort), code(answer.end));
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kElsewhere), readingOf(answer));
}

// The fault test's answer: 3000 aircraft round the world. Read through to the
// end in the memory of one aircraft, and not the answer asked for.
void test_the_whole_world_takes_the_memory_of_one_aircraft() {
  auto world = [](int count) {
    std::vector<std::string> planes;
    for (int i = 0; i < count; ++i) {
      char plane[256];
      snprintf(plane, sizeof(plane),
               "{\"hex\":\"%06x\",\"flight\":\"TST%-5d\",\"t\":\"A320\",\"alt_baro\":%d,"
               "\"gs\":420.5,\"track\":%d.5,\"squawk\":\"%04d\",\"lat\":%.5f,\"lon\":%.5f,"
               "\"seen_pos\":0.4,\"dst\":%d.1}",
               0x100000 + i * 97, i % 100000, 30000 + i % 9000, i % 360, i % 7000,
               -55.0 + (i * 37) % 120 + 0.5, -179.0 + (i * 73) % 358 + 0.5, 800 + i);
      planes.push_back(plane);
    }
    return answerOf(planes);
  };
  resetHeap();
  Shown one;
  readText(world(1), one);
  const size_t one_aircraft = s_peak;
  TEST_ASSERT_GREATER_THAN_size_t(0, one_aircraft);

  resetHeap();
  Shown shown;
  const std::string text = world(3000);
  const Answer answer = readText(text, shown);
  TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(answer.end));
  TEST_ASSERT_EQUAL_size_t(3000, answer.positioned);
  TEST_ASSERT_LESS_THAN_size_t(10, answer.in_area);
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kElsewhere), readingOf(answer));
  // The allocator never held more than one aircraft's worth (headers and
  // all), and has it all back.
  TEST_ASSERT_LESS_OR_EQUAL_size_t(one_aircraft, s_peak);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(kAircraftBytes, s_peak);
  TEST_ASSERT_EQUAL_size_t(0, s_live);
}

void test_a_real_answer() {
  Shown shown;
  resetHeap();
  const Answer answer = readText(kRealAnswer, shown, request(false, nullptr, kRealArea));
  TEST_ASSERT_EQUAL_UINT8(code(End::kWhole), code(answer.end));
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(answer));
  TEST_ASSERT_EQUAL_size_t(6, answer.records);
  TEST_ASSERT_EQUAL_size_t(6, answer.in_area);
  TEST_ASSERT_EQUAL_size_t(4, answer.offered);
  TEST_ASSERT_EQUAL_DOUBLE(1792632711500.0, answer.now);
  const std::map<std::string, std::string> planes = shown.byHex();
  TEST_ASSERT_EQUAL_size_t(1, planes.count("~273e1b"));
  TEST_ASSERT_EQUAL_STRING(
      "{\"hex\":\"a25e58\",\"flight\":\"CXK216  \",\"t\":\"C172\",\"dbFlags\":8,"
      "\"alt_baro\":550,\"alt_geom\":700,\"gs\":75.2,\"track\":193.85,\"true_heading\":194.06,"
      "\"baro_rate\":512,\"geom_rate\":576,\"squawk\":\"1200\",\"emergency\":\"none\","
      "\"category\":\"A1\",\"lat\":41.903366,\"lon\":-88.246557,\"seen_pos\":0.437}",
      planes.at("a25e58").c_str());
  TEST_ASSERT_EQUAL_size_t(0, s_live);

  // The two at the airport, with ground aircraft asked for.
  Shown all;
  readText(kRealAnswer, all, request(true, nullptr, kRealArea));
  TEST_ASSERT_EQUAL_size_t(6, all.size());
  TEST_ASSERT_EQUAL_size_t(1, all.byHex().count("a38682"));
}

// --- what to make of an answer -------------------------------------------------------

void test_judging_an_answer() {
  Answer a;
  a.end = End::kWhole;
  a.list = a.list_whole = true;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(a));  // an empty sky
  a.positioned = 10;
  a.in_area = 5;  // most, not all: one odd position must not cost a good answer
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(a));
  a.in_area = 4;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kElsewhere), readingOf(a));

  Answer cut;
  cut.end = End::kCutShort;
  cut.list = true;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kUnusable), readingOf(cut));
  cut.positioned = 1;  // on the ground or elsewhere is not enough
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kElsewhere), readingOf(cut));
  cut.in_area = 1;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kCutShort), readingOf(cut));
  Answer listed;  // the list was all there; only what follows it is missing
  listed.end = End::kCutShort;
  listed.list = listed.list_whole = true;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kCutShort), readingOf(listed));

  Answer refused;
  refused.end = End::kRefused;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kUnusable), readingOf(refused));
  refused.positioned = refused.in_area = 2;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kCutShort), readingOf(refused));

  Answer no_room;
  no_room.end = End::kNoRoom;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kNoRoom), readingOf(no_room));
  no_room.positioned = no_room.in_area = 1;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kCutShort), readingOf(no_room));

  Answer malformed;
  malformed.end = End::kMalformed;
  malformed.positioned = malformed.in_area = 3;
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kMalformed), readingOf(malformed));
}

void test_area_reach() {
  // 25 km asked for: 37.5 km still counts.
  TEST_ASSERT_TRUE(kOHare.reaches(41.9786 + 0.33, -87.9048));    // ~36.7 km north
  TEST_ASSERT_FALSE(kOHare.reaches(41.9786 + 0.35, -87.9048));   // ~38.9 km north
  TEST_ASSERT_TRUE(kOHare.reaches(41.9786, -87.9048 - 0.44));    // ~36.4 km west
  TEST_ASSERT_FALSE(kOHare.reaches(41.9786, -87.9048 + 180.0));  // the other side
  TEST_ASSERT_FALSE(kOHare.reaches(0.0, 0.0));
  TEST_ASSERT_FALSE(kOHare.reaches(NAN, -87.9048));
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 36.70, kOHare.distanceKm(41.9786 + 0.33, -87.9048));
  // Across the antimeridian, near is near.
  const Area fiji{-17.75, 179.95, 25.0f};
  TEST_ASSERT_TRUE(fiji.reaches(-17.75, -179.95));
}

// --- the feed's clock when "now" was cut off -------------------------------------------

void test_http_date() {
  uint32_t t = 7;
  // Before 2001, like a "now" the radar would not take either.
  TEST_ASSERT_FALSE(util::feed_answer::unixFromHttpDate("Sun, 06 Nov 1994 08:49:37 GMT", &t));
  TEST_ASSERT_EQUAL_UINT32(7, t);
  TEST_ASSERT_TRUE(util::feed_answer::unixFromHttpDate("Tue, 20 Oct 2026 21:15:07 GMT", &t));
  TEST_ASSERT_EQUAL_UINT32(1792530907u, t);
  TEST_ASSERT_TRUE(util::feed_answer::unixFromHttpDate("Thu, 29 Feb 2024 00:00:00 GMT", &t));
  TEST_ASSERT_EQUAL_UINT32(1709164800u, t);
  TEST_ASSERT_TRUE(util::feed_answer::unixFromHttpDate("Thu, 31 Dec 2099 23:59:59 GMT", &t));
  TEST_ASSERT_EQUAL_UINT32(4102444799u, t);
  t = 7;
  for (const char* bad : {"", "Tue, 20 Oct 2026 21:15:07", "Tue, 20 Oct 2026 21:15:07 UTC",
                          "Tue, 20 Oct 2026 21:15:07 GMT ", "Tue, 30 Feb 2026 21:15:07 GMT",
                          "Tue, 20 Okt 2026 21:15:07 GMT", "Tue, 20 Oct 2026 24:15:07 GMT",
                          "Tuesday, 20-Oct-26 21:15:07 GMT", "Tue Oct 20 21:15:07 2026",
                          "Tue, 2 Oct 2026 21:15:07 GMT", "Tue, 20 Oct 2200 21:15:07 GMT"}) {
    TEST_ASSERT_FALSE_MESSAGE(util::feed_answer::unixFromHttpDate(bad, &t), bad);
  }
  TEST_ASSERT_FALSE(util::feed_answer::unixFromHttpDate(nullptr, &t));
  TEST_ASSERT_EQUAL_UINT32(7, t);
}

void test_feed_clock() {
  uint32_t t = 7;
  TEST_ASSERT_TRUE(util::feed_answer::unixFromFeedClock(1792632711500.0, &t));  // ms
  TEST_ASSERT_EQUAL_UINT32(1792632711u, t);
  TEST_ASSERT_TRUE(util::feed_answer::unixFromFeedClock(1792632711.25, &t));  // s
  TEST_ASSERT_EQUAL_UINT32(1792632711u, t);
  TEST_ASSERT_TRUE(util::feed_answer::unixFromFeedClock(4294967295.0, &t));
  TEST_ASSERT_EQUAL_UINT32(4294967295u, t);
  t = 7;
  // None, before 2001, past what uint32_t holds (in microseconds, 9e12 ms),
  // infinite or not a number: left alone, and the Date header stands in.
  for (const double bad : {0.0, -1792632711500.0, 999999999.0, 5.0e11, 1792632711500000.0,
                           9.0e12, 4294967296.0, static_cast<double>(INFINITY),
                           static_cast<double>(NAN)}) {
    TEST_ASSERT_FALSE(util::feed_answer::unixFromFeedClock(bad, &t));
  }
  TEST_ASSERT_EQUAL_UINT32(7, t);
  // A clock with a 400-digit fraction, as read, is still the clock.
  const std::string text =
      "{\"ac\":[" + aircraft(1, 2.0) + "],\"now\":1792632711500." + std::string(400, '7') + "}";
  Shown shown;
  const Answer answer = readText(text, shown);
  TEST_ASSERT_EQUAL_UINT8(code(Reading::kAircraft), readingOf(answer));
  TEST_ASSERT_TRUE(util::feed_answer::unixFromFeedClock(answer.now, &t));
  TEST_ASSERT_EQUAL_UINT32(1792632711u, t);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_arduinojson_reads_an_object_to_its_closing_brace);
  RUN_TEST(test_strings_and_their_escapes);
  RUN_TEST(test_numbers);
  RUN_TEST(test_long_numbers);
  RUN_TEST(test_skipping_values);
  RUN_TEST(test_reads_either_order_of_keys);
  RUN_TEST(test_other_members_are_passed_over);
  RUN_TEST(test_a_value_nested_too_deep_fails_cleanly);
  RUN_TEST(test_cut_short_at_every_byte);
  RUN_TEST(test_an_answer_without_a_list);
  RUN_TEST(test_members_that_are_not_aircraft_and_null_fields);
  RUN_TEST(test_an_aircraft_too_big_ends_the_reading);
  RUN_TEST(test_the_heap_running_short);
  RUN_TEST(test_the_rtc_heap_holds_an_aircraft);
  RUN_TEST(test_the_nearest_are_kept);
  RUN_TEST(test_an_answer_for_elsewhere);
  RUN_TEST(test_the_whole_world_takes_the_memory_of_one_aircraft);
  RUN_TEST(test_a_real_answer);
  RUN_TEST(test_judging_an_answer);
  RUN_TEST(test_area_reach);
  RUN_TEST(test_http_date);
  RUN_TEST(test_feed_clock);
  return UNITY_END();
}
