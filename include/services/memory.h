#pragma once

#include <cstddef>

#include "util/json_budget.h"

/**
 * How much of the heap work that may fail is allowed to take: a JSON document
 * built from a server's answer, above all. The radar runs unattended for
 * years, so such work gives up (and is logged) well before Wi-Fi, lwIP or the
 * main loop could be left without the memory they need.
 */
namespace services::memory {

/**
 * Whether a block of `bytes` can be taken now without cutting into that
 * reserve: util::heap_room's rule, which also lets a block into the RTC heap
 * when the rest is short. The util::JsonBudget veto for every document a
 * server fills.
 */
bool canSpare(size_t bytes);

/**
 * Where those documents' blocks come from: the RTC heap first, which Wi-Fi
 * cannot use and nothing else touches until the rest has run out, then the
 * rest. Pass it to every util::JsonBudget that canSpare() vetoes.
 */
extern const util::JsonBudget::Heap kDocumentHeap;

}  // namespace services::memory
