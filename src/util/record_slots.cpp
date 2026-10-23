#include "util/record_slots.h"

namespace util {

bool RecordSlots::add(void* block) {
  if (block == nullptr || count_ == kMaxSlots) {
    return false;
  }
  slot_[count_] = block;
  lent_[count_] = false;
  ++count_;
  return true;
}

void* RecordSlots::take(size_t n, size_t size) {
  // calloc(1, size) from mbedTLS; n * size is checked without overflowing,
  // so a huge request cannot wrap round to the slot size.
  if (n == 0 || size > bytes_ / n || n * size != bytes_) {
    return nullptr;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (!lent_[i]) {
      lent_[i] = true;
      return slot_[i];
    }
  }
  return nullptr;
}

bool RecordSlots::give(void* ptr) {
  for (size_t i = 0; i < count_; ++i) {
    if (slot_[i] == ptr) {
      lent_[i] = false;
      return true;
    }
  }
  return false;
}

bool RecordSlots::owns(const void* ptr) const {
  for (size_t i = 0; i < count_; ++i) {
    if (slot_[i] == ptr) {
      return true;
    }
  }
  return false;
}

size_t RecordSlots::freeCount() const {
  size_t n = 0;
  for (size_t i = 0; i < count_; ++i) {
    n += lent_[i] ? 0 : 1;
  }
  return n;
}

}  // namespace util
