#include "services/memory.h"

#include <esp_heap_caps.h>

#include "util/heap_room.h"

namespace services::memory {

namespace {

/**
 * The heap Wi-Fi's buffers come from: esp_adapter asks for 8BIT|DMA|INTERNAL,
 * which is every DRAM region and not the RTC heap (0x500000d8-0x50001ff0, which
 * this build lets malloc() fall back to). Counting all 8-bit memory would
 * count ~7.5 KB that Wi-Fi can never have.
 */
constexpr uint32_t kWifiHeap = MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
/** The heaps util::heap_room's rule asks about. */
const util::heap_room::Heaps kHeaps = {
    [] { return heap_caps_get_free_size(kWifiHeap); },
    [] { return heap_caps_get_largest_free_block(kWifiHeap); },
    [] { return heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM); },
};

void* allocateRtcFirst(size_t bytes) {
  return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_RTCRAM, MALLOC_CAP_DEFAULT);
}

void* reallocateRtcFirst(void* ptr, size_t bytes) {
  return heap_caps_realloc_prefer(ptr, bytes, 2, MALLOC_CAP_RTCRAM, MALLOC_CAP_DEFAULT);
}

void deallocate(void* ptr) { heap_caps_free(ptr); }

}  // namespace

const util::JsonBudget::Heap kDocumentHeap = {allocateRtcFirst, reallocateRtcFirst, deallocate};

bool canSpare(size_t bytes) { return util::heap_room::canSpare(bytes, kHeaps); }

}  // namespace services::memory
