#pragma once

#include <ArduinoJson.h>

#include <cstddef>

/**
 * A memory allowance for one JsonDocument, given to it as its allocator:
 * `JsonDocument doc(&budget)`.
 *
 * deserializeJson() grows a document for as long as the input goes on, and a
 * filter only keeps out the fields it drops. An answer far bigger than the one
 * asked for (a feed that ignores the radius, a captive portal, a broken proxy)
 * would take the heap Wi-Fi, lwIP and drawing live on, and the radar would
 * crash; a hardware fault test with 3000 aircraft left 1.2 KB free. With a
 * budget the parse ends with NoMemory instead, and what it read before that
 * stays in the document: every element of an array but the last is whole and
 * correct, while the last may lack members or hold a null where a value was
 * refused. The document must still be read with that in mind.
 *
 * `limit` counts the bytes the document asks for, the measure the feeds'
 * answers were sized in; each block costs kOverhead bytes more here and the
 * heap's own overhead on top. `can_spare`, when given, is asked before every
 * block that grows the document, with the size of the block, and can refuse
 * it for reasons the budget cannot see: on the radar, how much the heap has
 * left. vetoed() tells such a refusal from one of the limit's, so the radar's
 * own shortage is not blamed on the server. `heap`, when given, is where the
 * blocks come from instead of malloc(), realloc() and free().
 *
 * A reallocation to the same or a smaller size is never refused: ArduinoJson
 * shrinks blocks at the end of every parse, a failed one included, and two of
 * those shrinks use the result without checking it.
 *
 * One budget per document, on one task, and it must outlive the document.
 * Free of Arduino headers so the host test suite can cover it.
 */
namespace util {

class JsonBudget final : public ArduinoJson::Allocator {
 public:
  /** Whether a block of `bytes` can be taken from the heap right now. */
  using CanSpare = bool (*)(size_t bytes);
  /** Where the blocks come from, as malloc(), realloc() and free() would. */
  struct Heap {
    void* (*allocate)(size_t bytes);
    void* (*reallocate)(void* ptr, size_t bytes);
    void (*deallocate)(void* ptr);
  };
  /**
   * Bytes each block costs here beyond those asked for: its size, kept in
   * front of it as wide as a double so the numbers behind it stay aligned.
   */
  static constexpr size_t kOverhead =
      sizeof(double) > sizeof(size_t) ? sizeof(double) : sizeof(size_t);

  explicit JsonBudget(size_t limit, CanSpare can_spare = nullptr, const Heap* heap = nullptr)
      : limit_(limit), can_spare_(can_spare), heap_(heap) {}
  JsonBudget(const JsonBudget&) = delete;
  JsonBudget& operator=(const JsonBudget&) = delete;

  void* allocate(size_t size) override;
  void deallocate(void* ptr) override;
  void* reallocate(void* ptr, size_t new_size) override;

  /** Bytes the document holds now. */
  size_t used() const { return used_; }
  /** The most it has held at once. */
  size_t peak() const { return peak_; }
  /** A request was turned down: the parse that made it ended with NoMemory. */
  bool refused() const { return refused_; }
  /**
   * It was turned down by the heap (`can_spare`, or an allocation that
   * failed), not by `limit`: the document was not too big, memory was short.
   */
  bool vetoed() const { return vetoed_; }

 private:
  /** May the document grow by `extra` bytes, in a block of `block` bytes? */
  bool admit(size_t extra, size_t block);
  void grow(size_t bytes);
  const Heap& heap() const;

  size_t limit_;
  CanSpare can_spare_;
  const Heap* heap_;
  size_t used_ = 0;
  size_t peak_ = 0;
  bool refused_ = false;
  bool vetoed_ = false;
};

}  // namespace util
