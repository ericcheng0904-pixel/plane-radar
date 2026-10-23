#pragma once

#include <cstddef>

/**
 * Blocks set aside once for allocations of one size, and lent out again and
 * again: mbedTLS's two TLS record buffers, reserved at boot (services::tls).
 *
 * Every HTTPS session asks for two 16.7 KB buffers and gives them back when
 * it ends. Taken from the heap each time, they need two free pieces that big,
 * and the heap's pieces shrink as the radar runs: a small block left in one
 * between two sessions was enough to make the next handshake fail. Set aside
 * while the heap is still whole, they are always there.
 *
 * Bookkeeping only: which blocks are lent. The caller locks around every call
 * but owns() (more than one task allocates through mbedTLS), and zeroes what
 * it hands out as calloc() would. Free of Arduino headers so the host suite
 * covers it.
 */
namespace util {

class RecordSlots {
 public:
  static constexpr size_t kMaxSlots = 2;

  /** Slots of `slot_bytes` each, none yet: add() them. */
  explicit RecordSlots(size_t slot_bytes) : bytes_(slot_bytes) {}
  RecordSlots(const RecordSlots&) = delete;
  RecordSlots& operator=(const RecordSlots&) = delete;

  /**
   * One more slot: `block`, at least slot_bytes() long. Before the first
   * take(). False (and nothing kept) for a null block or once full.
   */
  bool add(void* block);

  /**
   * A free slot for calloc(n, size), lent until give(): only for exactly
   * slot_bytes(), so every other allocation goes to the heap as before.
   * nullptr when the size differs or every slot is lent. Not zeroed.
   */
  void* take(size_t n, size_t size);

  /**
   * `ptr` comes back: true if it is one of the slots, which is then free
   * again; false for anything else, which the caller frees as before.
   */
  bool give(void* ptr);

  /**
   * Whether `ptr` is one of the slots, lent or not. The slots never move once
   * added, so this needs no lock.
   */
  bool owns(const void* ptr) const;

  size_t slotBytes() const { return bytes_; }
  /** Slots added. */
  size_t count() const { return count_; }
  /** Slots not lent right now. */
  size_t freeCount() const;

 private:
  size_t bytes_;
  size_t count_ = 0;
  void* slot_[kMaxSlots] = {};
  bool lent_[kMaxSlots] = {};
};

}  // namespace util
