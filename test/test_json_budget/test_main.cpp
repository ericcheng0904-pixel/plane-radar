/**
 * Host tests for util::JsonBudget, the memory limit on every JSON document the
 * radar builds from a server's answer (each aircraft of a feed's, a route
 * list, a town search, the update manifest).
 *
 * A hardware fault test fed the radar 3000 aircraft and it ran out of heap
 * inside deserializeJson(). These check that a budget ends such a parse with
 * NoMemory instead, that the elements read before that are intact, and that
 * the budget's books balance through every resize. A feed-style answer is the
 * data. Byte counts here are the host's, whose slots are twice the radar's
 * size and whose slot pools are four times. How a feed's answer is read is
 * test_feed_answer's.
 */
#include <unity.h>

#include <ArduinoJson.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "util/feed_answer.h"
#include "util/json_budget.h"

void setUp() {}
void tearDown() {}

namespace {

/** Veto that remembers each question and refuses blocks over s_max_block. */
size_t s_max_block = SIZE_MAX;
std::vector<size_t> s_asked;
bool vetoLarge(size_t bytes) {
  s_asked.push_back(bytes);
  return bytes <= s_max_block;
}

/** Lets s_blocks_left blocks through, then refuses: a heap running low. */
int s_blocks_left = 0;
bool heapRunsOut(size_t) { return s_blocks_left-- > 0; }

/** A heap that counts what it hands out, and has none left once s_heap_empty. */
bool s_heap_empty = false;
int s_heap_blocks = 0;
void* countedAllocate(size_t bytes) {
  if (s_heap_empty) {
    return nullptr;
  }
  ++s_heap_blocks;
  return malloc(bytes);
}
void* countedReallocate(void* ptr, size_t bytes) {
  return s_heap_empty ? nullptr : realloc(ptr, bytes);
}
void countedDeallocate(void* ptr) {
  --s_heap_blocks;
  free(ptr);
}
const util::JsonBudget::Heap kCountedHeap = {countedAllocate, countedReallocate, countedDeallocate};

/** A whole answer's filter: "now", and the radar's fields of each aircraft. */
void answerFilter(JsonDocument& filter) {
  JsonDocument aircraft;
  util::feed_answer::buildFilter(aircraft);
  filter["now"] = true;
  filter["ac"].add(aircraft.as<JsonVariantConst>());
}

/**
 * A readsb answer of `count` aircraft around O'Hare in adsb.lol's order ("ac"
 * before "now"), with the fields the filter drops between the ones it keeps.
 */
std::string feedAnswer(int count) {
  std::string out = "{\"ac\":[";
  for (int i = 0; i < count; ++i) {
    char alt[16];
    if (i % 4 == 0) {
      snprintf(alt, sizeof(alt), "\"ground\"");
    } else {
      snprintf(alt, sizeof(alt), "%d", 1000 + 25 * i);
    }
    char plane[512];
    snprintf(plane, sizeof(plane),
             "%s{\"hex\":\"a%05x\",\"type\":\"adsb_icao\",\"flight\":\"UAL%-5d\","
             "\"r\":\"N%dUA\",\"t\":\"B738\",\"desc\":\"BOEING 737-800\",\"alt_baro\":%s,"
             "\"gs\":%.1f,\"track\":%.2f,\"baro_rate\":%d,\"squawk\":\"%04d\","
             "\"category\":\"A3\",\"nav_qnh\":1013.6,\"lat\":%.6f,\"lon\":%.6f,\"nic\":8,"
             "\"rc\":186,\"seen_pos\":%.3f,\"version\":2,\"dbFlags\":0,\"messages\":%d,"
             "\"seen\":0.1,\"rssi\":-21.4,\"dst\":%.3f,\"dir\":%.1f}",
             i == 0 ? "" : ",", i, 100 + i, 100 + i, alt, 140.0 + i * 0.7,
             (i * 37) % 360 + 0.25, (i % 7 - 3) * 512, 1000 + i % 7000,
             41.9786 + (i % 50) * 0.003417, -87.9048 - (i / 50) * 0.004129, (i % 30) * 0.1,
             1000 + i, 0.2 + i * 0.01, (i * 53) % 360 * 1.0);
    out += plane;
  }
  out += "],\"msg\":\"No error\",\"now\":1792531860000,\"total\":";
  out += std::to_string(count);
  out += ",\"ctime\":1792531860000,\"ptime\":0}";
  return out;
}

std::string toJson(JsonVariantConst v) {
  std::string out;
  serializeJson(v, out);
  return out;
}

/**
 * A parse that ran out part way: NoMemory, and an "ac" list whose aircraft
 * before the last are exactly those of `whole`, the same answer read without
 * a limit. Returns how many the list holds, the last included.
 */
size_t expectCutShortList(const JsonDocument& doc, DeserializationError err,
                          const JsonDocument& whole) {
  TEST_ASSERT_EQUAL_INT(DeserializationError::NoMemory, err.code());
  TEST_ASSERT_TRUE(doc["ac"].is<JsonArrayConst>());
  JsonArrayConst ac = doc["ac"].as<JsonArrayConst>();
  JsonArrayConst all = whole["ac"].as<JsonArrayConst>();
  const size_t kept = ac.size();
  TEST_ASSERT_GREATER_THAN_size_t(1, kept);
  TEST_ASSERT_LESS_THAN_size_t(all.size(), kept);
  size_t i = 0;
  for (JsonObjectConst plane : ac) {
    if (i + 1 == kept) {
      break;  // the one being read when memory ran out may be incomplete
    }
    TEST_ASSERT_EQUAL_STRING(toJson(all[i]).c_str(), toJson(plane).c_str());
    TEST_ASSERT_TRUE(plane["lat"].is<float>());
    TEST_ASSERT_TRUE(plane["lon"].is<float>());
    ++i;
  }
  // "now" comes after the list and was never reached.
  TEST_ASSERT_TRUE(doc["now"].isNull());
  return kept;
}

}  // namespace

// --- the budget's books ---------------------------------------------------------

void test_budget_counts_through_resizes() {
  util::JsonBudget budget(1000);
  auto* p = static_cast<char*>(budget.allocate(100));
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_size_t(100, budget.used());
  memset(p, 'a', 100);
  p = static_cast<char*>(budget.reallocate(p, 400));
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EACH_EQUAL_CHAR('a', p, 100);  // contents move with the block
  TEST_ASSERT_EQUAL_size_t(400, budget.used());
  p = static_cast<char*>(budget.reallocate(p, 40));
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_size_t(40, budget.used());
  // What ArduinoJson does with a slot pool it was refused, at the end of a parse.
  void* q = budget.reallocate(nullptr, 60);
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQUAL_size_t(100, budget.used());
  budget.deallocate(p);
  budget.deallocate(q);
  budget.deallocate(nullptr);
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
  TEST_ASSERT_EQUAL_size_t(400, budget.peak());
  TEST_ASSERT_FALSE(budget.refused());
}

void test_budget_refuses_past_its_limit() {
  util::JsonBudget budget(1000);
  void* p = budget.allocate(1000);  // the whole budget is usable
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_FALSE(budget.refused());
  TEST_ASSERT_NULL(budget.allocate(1));
  TEST_ASSERT_TRUE(budget.refused());
  TEST_ASSERT_FALSE(budget.vetoed());  // too big for the budget, not short of heap
  TEST_ASSERT_NULL(budget.allocate(SIZE_MAX));  // must not wrap around and fit
  TEST_ASSERT_EQUAL_size_t(1000, budget.used());
  budget.deallocate(p);
  p = budget.allocate(600);  // room again once it is given back
  TEST_ASSERT_NOT_NULL(p);
  budget.deallocate(p);
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

void test_refused_growth_leaves_the_block_alone() {
  util::JsonBudget budget(1000);
  auto* p = static_cast<char*>(budget.allocate(600));
  TEST_ASSERT_NOT_NULL(p);
  memset(p, 'x', 600);
  TEST_ASSERT_NULL(budget.reallocate(p, 1001));
  TEST_ASSERT_TRUE(budget.refused());
  TEST_ASSERT_EQUAL_size_t(600, budget.used());
  TEST_ASSERT_EACH_EQUAL_CHAR('x', p, 600);  // still the caller's, still whole
  budget.deallocate(p);
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

// ArduinoJson shrinks blocks at the end of every parse, one that ran out of
// memory included, and uses two of the results without checking them.
void test_shrinking_is_never_refused() {
  s_max_block = SIZE_MAX;
  util::JsonBudget budget(1000, vetoLarge);
  void* p = budget.allocate(800);
  TEST_ASSERT_NOT_NULL(p);
  s_max_block = 0;  // the heap can spare nothing now
  p = budget.reallocate(p, 800);
  TEST_ASSERT_NOT_NULL(p);
  p = budget.reallocate(p, 200);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_size_t(200, budget.used());
  TEST_ASSERT_FALSE(budget.refused());
  TEST_ASSERT_NULL(budget.reallocate(p, 201));  // growing is another matter
  TEST_ASSERT_TRUE(budget.refused());
  budget.deallocate(p);
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

void test_veto_is_asked_about_every_block() {
  s_asked.clear();
  s_max_block = 512;
  util::JsonBudget budget(100000, vetoLarge);
  void* p = budget.allocate(100);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_size_t(1, s_asked.size());
  // The block, header and all.
  TEST_ASSERT_EQUAL_size_t(100 + util::JsonBudget::kOverhead, s_asked[0]);
  TEST_ASSERT_FALSE(budget.vetoed());
  TEST_ASSERT_NULL(budget.allocate(1000));  // within budget, but vetoed
  TEST_ASSERT_TRUE(budget.refused());
  TEST_ASSERT_TRUE(budget.vetoed());
  // A grown block may have to be copied: the veto hears about all of it, not
  // only the 500 bytes it grows by.
  TEST_ASSERT_NULL(budget.reallocate(p, 600));
  TEST_ASSERT_EQUAL_size_t(600 + util::JsonBudget::kOverhead, s_asked.back());
  TEST_ASSERT_EQUAL_size_t(100, budget.used());
  p = budget.reallocate(p, 400);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_size_t(400, budget.used());
  budget.deallocate(p);
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

// --- a feed's answer ------------------------------------------------------------

void test_answer_within_budget_reads_whole() {
  const std::string body = feedAnswer(10);
  JsonDocument filter;
  answerFilter(filter);
  util::JsonBudget budget(64 * 1024);
  JsonDocument doc(&budget);
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  TEST_ASSERT_EQUAL_INT(DeserializationError::Ok, err.code());
  TEST_ASSERT_FALSE(budget.refused());
  TEST_ASSERT_EQUAL_size_t(10, doc["ac"].size());
  TEST_ASSERT_EQUAL_STRING("a00003", doc["ac"][3]["hex"].as<const char*>());
  TEST_ASSERT_TRUE(doc["ac"][3]["dir"].isNull());  // dropped by the filter
  TEST_ASSERT_EQUAL_DOUBLE(1792531860000.0, doc["now"].as<double>());
}

void test_answer_over_budget_keeps_the_aircraft_before_the_cut() {
  const std::string body = feedAnswer(400);
  JsonDocument filter;
  answerFilter(filter);
  JsonDocument whole;
  TEST_ASSERT_EQUAL_INT(DeserializationError::Ok,
                        deserializeJson(whole, body, DeserializationOption::Filter(filter)).code());

  constexpr size_t kLimit = 24 * 1024;
  util::JsonBudget budget(kLimit);
  {
    JsonDocument doc(&budget);
    const DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    TEST_ASSERT_TRUE(budget.refused());
    TEST_ASSERT_LESS_OR_EQUAL_size_t(kLimit, budget.peak());
    const size_t kept = expectCutShortList(doc, err, whole);
    // Most of the budget went into aircraft, not lost to the cut.
    TEST_ASSERT_GREATER_THAN_size_t(kLimit / 2, budget.peak());
    TEST_ASSERT_GREATER_THAN_size_t(10, kept);
  }
  TEST_ASSERT_EQUAL_size_t(0, budget.used());  // the document gave every byte back
}

void test_heap_running_low_keeps_the_aircraft_before_the_cut() {
  const std::string body = feedAnswer(400);
  JsonDocument filter;
  answerFilter(filter);
  JsonDocument whole;
  deserializeJson(whole, body, DeserializationOption::Filter(filter));

  s_blocks_left = 60;
  util::JsonBudget budget(1024 * 1024, heapRunsOut);
  {
    JsonDocument doc(&budget);
    const DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    TEST_ASSERT_TRUE(budget.refused());
    expectCutShortList(doc, err, whole);
  }
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

void test_long_strings_cost_only_when_kept() {
  JsonDocument filter;
  answerFilter(filter);
  const std::string junk(100000, 'x');
  util::JsonBudget budget(16 * 1024);
  {
    // A dropped value is skipped, however long.
    JsonDocument doc(&budget);
    const std::string body = "{\"ac\":[{\"hex\":\"abc123\",\"desc\":\"" + junk + "\"}]}";
    TEST_ASSERT_EQUAL_INT(DeserializationError::Ok,
                          deserializeJson(doc, body, DeserializationOption::Filter(filter)).code());
    TEST_ASSERT_EQUAL_STRING("abc123", doc["ac"][0]["hex"].as<const char*>());
  }
  TEST_ASSERT_FALSE(budget.refused());
  {
    // A kept one grows until the budget says no, and the parse ends there.
    JsonDocument doc(&budget);
    const std::string body = "{\"ac\":[{\"hex\":\"abc123\",\"flight\":\"" + junk + "\"}]}";
    TEST_ASSERT_EQUAL_INT(DeserializationError::NoMemory,
                          deserializeJson(doc, body, DeserializationOption::Filter(filter)).code());
    TEST_ASSERT_TRUE(budget.refused());
    TEST_ASSERT_LESS_OR_EQUAL_size_t(16 * 1024, budget.peak());
  }
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

// A key is read whole before the filter is asked about it, dropped or not:
// a long one costs memory however it ends, but no more than the budget.
void test_long_keys_cost_even_when_dropped() {
  JsonDocument filter;
  answerFilter(filter);
  util::JsonBudget budget(16 * 1024);
  {
    JsonDocument doc(&budget);
    const std::string body = "{\"ac\":[{\"hex\":\"abc123\"},{\"" + std::string(100000, 'k') +
                             "\":1}]}";
    TEST_ASSERT_EQUAL_INT(DeserializationError::NoMemory,
                          deserializeJson(doc, body, DeserializationOption::Filter(filter)).code());
    TEST_ASSERT_TRUE(budget.refused());
    TEST_ASSERT_LESS_OR_EQUAL_size_t(16 * 1024, budget.peak());
  }
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

// --- where the blocks come from -------------------------------------------------

void test_blocks_come_from_the_heap_given() {
  s_heap_empty = false;
  s_heap_blocks = 0;
  util::JsonBudget budget(64 * 1024, nullptr, &kCountedHeap);
  {
    JsonDocument doc(&budget);
    const std::string body = feedAnswer(10);
    TEST_ASSERT_EQUAL_INT(DeserializationError::Ok, deserializeJson(doc, body).code());
    TEST_ASSERT_GREATER_THAN_INT(0, s_heap_blocks);
  }
  TEST_ASSERT_EQUAL_INT(0, s_heap_blocks);  // and go back to it
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

// The heap failing is the radar short of memory, as a veto is.
void test_heap_failure_counts_as_a_veto() {
  s_heap_empty = false;
  util::JsonBudget budget(64 * 1024, nullptr, &kCountedHeap);
  void* p = budget.allocate(100);
  TEST_ASSERT_NOT_NULL(p);
  s_heap_empty = true;
  TEST_ASSERT_NULL(budget.reallocate(p, 200));
  TEST_ASSERT_TRUE(budget.refused());
  TEST_ASSERT_TRUE(budget.vetoed());
  TEST_ASSERT_EQUAL_size_t(100, budget.used());
  TEST_ASSERT_NOT_NULL(budget.reallocate(p, 50));  // shrinking still works
  s_heap_empty = false;
  budget.deallocate(p);
  TEST_ASSERT_EQUAL_size_t(0, budget.used());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_budget_counts_through_resizes);
  RUN_TEST(test_budget_refuses_past_its_limit);
  RUN_TEST(test_refused_growth_leaves_the_block_alone);
  RUN_TEST(test_shrinking_is_never_refused);
  RUN_TEST(test_veto_is_asked_about_every_block);
  RUN_TEST(test_answer_within_budget_reads_whole);
  RUN_TEST(test_answer_over_budget_keeps_the_aircraft_before_the_cut);
  RUN_TEST(test_heap_running_low_keeps_the_aircraft_before_the_cut);
  RUN_TEST(test_long_strings_cost_only_when_kept);
  RUN_TEST(test_long_keys_cost_even_when_dropped);
  RUN_TEST(test_blocks_come_from_the_heap_given);
  RUN_TEST(test_heap_failure_counts_as_a_veto);
  return UNITY_END();
}
