#pragma once

#include <cstddef>

/**
 * Whether a block that a server's document asks for can be taken now: the
 * rule of services::memory::canSpare(), kept apart from the ESP-IDF calls
 * that measure the heaps so the host tests cover it. Free of Arduino headers.
 */
namespace util::heap_room {

/**
 * The radar's heaps, as the rule asks about them. Each is asked only when
 * the rule needs it: finding a largest piece walks that heap under its lock.
 */
struct Heaps {
  /** Free bytes of the heap Wi-Fi can use (DRAM). */
  size_t (*wifi_free)();
  /** Its largest free piece. */
  size_t (*wifi_largest)();
  /**
   * The largest free piece of the RTC heap (~7.9 KB), which Wi-Fi cannot
   * use and documents' blocks come from first (services::memory::kDocumentHeap).
   */
  size_t (*rtc_largest)();
};

/**
 * Heap Wi-Fi can use that is left free while a document grows. Wi-Fi takes a
 * buffer for every frame it receives until lwIP and the TLS reader are done
 * with it: up to its 6-frame block-ack window, ~10 KB, of which the answer's
 * own 5.7 KB TCP window is part. The 6 KB buffer the main loop borrows while
 * it builds the water mask (only when the range or the place changes) is not
 * reserved for: taken during the parse of a busy sky, it can cost Wi-Fi a few
 * frames, which TCP sends again.
 *
 * ~45 KB is free at rest as ESP.getFreeHeap() counts it, ~7.5 KB of it the RTC
 * heap, beside the 33 KB reserved for mbedTLS's record buffers. The rest of
 * the TLS session with HTTPClient and the filter takes 12-15 KB (fault tests:
 * ~28 KB left during a fetch, 7.5 KB of it the RTC heap), so a feed's answer
 * starts with ~20 KB of DRAM and the RTC heap. It is read one aircraft at a
 * time, ~1.3 KB of document each (1.8 KB of heap, its largest block 1 KB:
 * services::adsb's kAircraftBudget), given back before the next, so this
 * reserve is only reached when the heap was short before the request, and
 * even then the RTC heap can hold an aircraft. A real feed's certificate
 * chain may take a little more than the fault tests' mock: to be measured on
 * the radar.
 */
constexpr size_t kReserve = 10 * 1024;
/**
 * A free piece at least this big must be left beside a large block: room for
 * a full-size Wi-Fi frame (1.6 KB), or the answer being read stops arriving.
 * Either the block comes out of the largest piece and this much of it is
 * left, or it comes from elsewhere and the largest piece is untouched. In the
 * RTC heap, it is left for what malloc() puts there once DRAM is short.
 */
constexpr size_t kPieceLeft = 2 * 1024;
/**
 * Finding the largest piece of DRAM walks the whole heap under its lock: done
 * for slot pools (1 KB) and long strings, not for each short string of an
 * answer.
 */
constexpr size_t kWalkFrom = 512;

/**
 * Whether a block of `bytes` can be taken: from DRAM without cutting into
 * kReserve or its largest piece, or else from the RTC heap, where it goes
 * first anyway, leaving a kPieceLeft piece there.
 */
bool canSpare(size_t bytes, const Heaps& heaps);

}  // namespace util::heap_room
