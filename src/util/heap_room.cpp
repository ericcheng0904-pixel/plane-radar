#include "util/heap_room.h"

namespace util::heap_room {

bool canSpare(size_t bytes, const Heaps& heaps) {
  if (heaps.wifi_free() >= bytes + kReserve &&
      (bytes < kWalkFrom || heaps.wifi_largest() >= bytes + kPieceLeft)) {
    return true;
  }
  // DRAM is short, but the block would not come from it: the RTC heap
  // (~7.9 KB) holds an aircraft, ~1.8 KB, given back before the next.
  return heaps.rtc_largest() >= bytes + kPieceLeft;
}

}  // namespace util::heap_room
