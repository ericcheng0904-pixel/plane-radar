/**
 * Host tests for util::RecordSlots, the two blocks mbedTLS's record buffers
 * are lent from (services::tls::reserveRecordBuffers()).
 *
 * The radar's allocator hook asks it about every block mbedTLS allocates and
 * frees, Wi-Fi's own included: only the record buffers' exact size may get a
 * slot, a slot must never be lent twice, and every pointer that is not a slot
 * must be left for the heap to free.
 */
#include <unity.h>

#include <cstddef>
#include <cstdint>

#include "util/record_slots.h"

void setUp() {}
void tearDown() {}

namespace {

/** mbedTLS's record buffer on the radar: 13 + 16 + 48 + 256 + 16384 bytes. */
constexpr size_t kBytes = 16717;
alignas(8) unsigned char s_a[kBytes];
alignas(8) unsigned char s_b[kBytes];

/** Slots with both blocks added. */
struct Filled {
  util::RecordSlots slots{kBytes};
  Filled() {
    slots.add(s_a);
    slots.add(s_b);
  }
};

}  // namespace

void test_lends_each_slot_once() {
  Filled f;
  void* first = f.slots.take(1, kBytes);
  void* second = f.slots.take(1, kBytes);
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_NOT_NULL(second);
  TEST_ASSERT_TRUE(first != second);
  TEST_ASSERT_TRUE(first == s_a || first == s_b);
  TEST_ASSERT_TRUE(second == s_a || second == s_b);
  TEST_ASSERT_EQUAL_UINT32(0, f.slots.freeCount());
}

void test_third_record_buffer_goes_to_the_heap() {
  // A second session at once (not meant to happen) takes its buffers from
  // the heap, as every session did before the reserve.
  Filled f;
  f.slots.take(1, kBytes);
  f.slots.take(1, kBytes);
  TEST_ASSERT_NULL(f.slots.take(1, kBytes));
}

void test_other_sizes_are_not_lent() {
  Filled f;
  TEST_ASSERT_NULL(f.slots.take(1, kBytes - 1));
  TEST_ASSERT_NULL(f.slots.take(1, kBytes + 1));
  TEST_ASSERT_NULL(f.slots.take(1, 32));
  TEST_ASSERT_NULL(f.slots.take(1, 0));
  TEST_ASSERT_NULL(f.slots.take(0, kBytes));
  TEST_ASSERT_EQUAL_UINT32(2, f.slots.freeCount());
}

void test_count_times_size_is_what_counts() {
  // calloc(n, size) asks for n * size bytes, however they are split.
  util::RecordSlots slots(1024);
  unsigned char block[1024];
  slots.add(block);
  TEST_ASSERT_TRUE(slots.take(4, 256) == block);
}

void test_a_wrapping_product_is_not_the_slot_size() {
  // 2 * (SIZE_MAX / 2 + 1 + 512) wraps round to 1024: a slot would be far too
  // small for what that calloc() asks.
  util::RecordSlots slots(1024);
  unsigned char block[1024];
  slots.add(block);
  const size_t wraps = static_cast<size_t>(SIZE_MAX) / 2 + 1 + 512;
  TEST_ASSERT_EQUAL_UINT64(1024, static_cast<size_t>(2 * wraps));
  TEST_ASSERT_NULL(slots.take(2, wraps));
  TEST_ASSERT_NULL(slots.take(wraps, 2));
  TEST_ASSERT_EQUAL_UINT32(1, slots.freeCount());
}

void test_given_back_slot_is_lent_again() {
  Filled f;
  void* first = f.slots.take(1, kBytes);
  f.slots.take(1, kBytes);
  TEST_ASSERT_TRUE(f.slots.give(first));
  TEST_ASSERT_EQUAL_UINT32(1, f.slots.freeCount());
  TEST_ASSERT_TRUE(f.slots.take(1, kBytes) == first);
}

void test_sessions_one_after_another_never_run_out() {
  // The radar opens tens of thousands of sessions between restarts.
  Filled f;
  for (int i = 0; i < 100000; ++i) {
    void* in = f.slots.take(1, kBytes);
    void* out = f.slots.take(1, kBytes);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_NOT_NULL(out);
    // mbedtls_ssl_free() gives back out_buf first.
    TEST_ASSERT_TRUE(f.slots.give(out));
    TEST_ASSERT_TRUE(f.slots.give(in));
  }
  TEST_ASSERT_EQUAL_UINT32(2, f.slots.freeCount());
}

void test_other_pointers_are_left_for_the_heap() {
  Filled f;
  int elsewhere = 0;
  TEST_ASSERT_FALSE(f.slots.give(&elsewhere));
  TEST_ASSERT_FALSE(f.slots.give(nullptr));
  // Inside a slot but not its start: not a block mbedTLS was given.
  TEST_ASSERT_FALSE(f.slots.give(s_a + 1));
  TEST_ASSERT_FALSE(f.slots.owns(&elsewhere));
  TEST_ASSERT_FALSE(f.slots.owns(nullptr));
  TEST_ASSERT_TRUE(f.slots.owns(s_a));
  TEST_ASSERT_TRUE(f.slots.owns(s_b));
  TEST_ASSERT_EQUAL_UINT32(2, f.slots.freeCount());
}

void test_free_slot_given_back_stays_free() {
  // A slot freed twice must not count twice, or a third buffer could share it.
  Filled f;
  void* first = f.slots.take(1, kBytes);
  TEST_ASSERT_TRUE(f.slots.give(first));
  TEST_ASSERT_TRUE(f.slots.give(first));
  TEST_ASSERT_EQUAL_UINT32(2, f.slots.freeCount());
  void* a = f.slots.take(1, kBytes);
  void* b = f.slots.take(1, kBytes);
  TEST_ASSERT_TRUE(a != b);
  TEST_ASSERT_NULL(f.slots.take(1, kBytes));
}

void test_without_slots_nothing_is_lent() {
  // The reserve did not fit at boot: every allocation goes to the heap.
  util::RecordSlots slots(kBytes);
  TEST_ASSERT_EQUAL_UINT32(0, slots.count());
  TEST_ASSERT_EQUAL_UINT32(0, slots.freeCount());
  TEST_ASSERT_NULL(slots.take(1, kBytes));
  TEST_ASSERT_FALSE(slots.give(s_a));
  TEST_ASSERT_FALSE(slots.owns(s_a));
}

void test_add_refuses_null_and_a_third_block() {
  util::RecordSlots slots(kBytes);
  unsigned char third[8];
  TEST_ASSERT_FALSE(slots.add(nullptr));
  TEST_ASSERT_TRUE(slots.add(s_a));
  TEST_ASSERT_TRUE(slots.add(s_b));
  TEST_ASSERT_FALSE(slots.add(third));
  TEST_ASSERT_EQUAL_UINT32(2, slots.count());
  TEST_ASSERT_FALSE(slots.owns(third));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_lends_each_slot_once);
  RUN_TEST(test_third_record_buffer_goes_to_the_heap);
  RUN_TEST(test_other_sizes_are_not_lent);
  RUN_TEST(test_count_times_size_is_what_counts);
  RUN_TEST(test_a_wrapping_product_is_not_the_slot_size);
  RUN_TEST(test_given_back_slot_is_lent_again);
  RUN_TEST(test_sessions_one_after_another_never_run_out);
  RUN_TEST(test_other_pointers_are_left_for_the_heap);
  RUN_TEST(test_free_slot_given_back_stays_free);
  RUN_TEST(test_without_slots_nothing_is_lent);
  RUN_TEST(test_add_refuses_null_and_a_third_block);
  return UNITY_END();
}
