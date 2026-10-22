#include "util/json_budget.h"

#include <cstdlib>

namespace util {

namespace {

/**
 * In front of every block: ArduinoJson never says how big a block is when it
 * frees or resizes it, and the budget needs that back. As wide as a double,
 * so the numbers the document keeps behind it stay aligned.
 */
union Header {
  size_t size;
  double align;
};
static_assert(sizeof(Header) == JsonBudget::kOverhead, "kOverhead is the header");

Header* headerOf(void* ptr) { return static_cast<Header*>(ptr) - 1; }

const JsonBudget::Heap kCLibrary = {
    [](size_t bytes) { return malloc(bytes); },
    [](void* ptr, size_t bytes) { return realloc(ptr, bytes); },
    [](void* ptr) { free(ptr); },
};

}  // namespace

const JsonBudget::Heap& JsonBudget::heap() const {
  return heap_ != nullptr ? *heap_ : kCLibrary;
}

bool JsonBudget::admit(size_t extra, size_t block) {
  // Subtracted, not added: an absurd request must not wrap around and fit.
  if (extra > limit_ || used_ > limit_ - extra) {
    refused_ = true;
    return false;
  }
  if (can_spare_ != nullptr && !can_spare_(block + sizeof(Header))) {
    refused_ = true;
    vetoed_ = true;
    return false;
  }
  return true;
}

void JsonBudget::grow(size_t bytes) {
  used_ += bytes;
  if (used_ > peak_) {
    peak_ = used_;
  }
}

void* JsonBudget::allocate(size_t size) {
  if (!admit(size, size)) {
    return nullptr;
  }
  auto* block = static_cast<Header*>(heap().allocate(sizeof(Header) + size));
  if (block == nullptr) {
    refused_ = true;
    vetoed_ = true;
    return nullptr;
  }
  block->size = size;
  grow(size);
  return block + 1;
}

void JsonBudget::deallocate(void* ptr) {
  if (ptr == nullptr) {
    return;
  }
  Header* block = headerOf(ptr);
  used_ -= block->size;
  heap().deallocate(block);
}

void* JsonBudget::reallocate(void* ptr, size_t new_size) {
  if (ptr == nullptr) {
    // A slot pool whose allocation was refused, shrunk to fit at the end.
    return allocate(new_size);
  }
  Header* block = headerOf(ptr);
  const size_t old_size = block->size;
  // A grown block may have to be copied, so the heap is asked for all of it.
  if (new_size > old_size && !admit(new_size - old_size, new_size)) {
    return nullptr;
  }
  auto* resized = static_cast<Header*>(heap().reallocate(block, sizeof(Header) + new_size));
  if (resized == nullptr) {
    if (new_size <= old_size) {
      return ptr;  // realloc() left the block as it was, and it still fits
    }
    refused_ = true;
    vetoed_ = true;
    return nullptr;
  }
  resized->size = new_size;
  used_ -= old_size;
  grow(new_size);
  return resized + 1;
}

}  // namespace util
