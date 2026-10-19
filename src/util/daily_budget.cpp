#include "util/daily_budget.h"

namespace util {

void DailyBudget::advance(unsigned long now_ms) {
  if (!started_) {
    started_ = true;
    hour_start_ms_ = now_ms;
    return;
  }
  const unsigned long elapsed = now_ms - hour_start_ms_;
  if (elapsed >= static_cast<unsigned long>(kHours) * kHourMs) {
    for (uint16_t& c : counts_) {
      c = 0;
    }
    hour_start_ms_ = now_ms;
    return;
  }
  for (unsigned long hours = elapsed / kHourMs; hours > 0; --hours) {
    head_ = (head_ + 1) % kHours;
    counts_[head_] = 0;
    hour_start_ms_ += kHourMs;
  }
}

uint16_t DailyBudget::used(unsigned long now_ms) {
  advance(now_ms);
  unsigned int sum = 0;
  for (uint16_t c : counts_) {
    sum += c;
  }
  return sum > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(sum);
}

bool DailyBudget::take(unsigned long now_ms) {
  if (used(now_ms) >= limit_) {
    return false;
  }
  if (counts_[head_] < 0xFFFFu) {
    ++counts_[head_];
  }
  return true;
}

unsigned long DailyBudget::msUntilFree(unsigned long now_ms) {
  if (used(now_ms) < limit_) {
    return 0;
  }
  // The oldest hour with requests in it leaves the window first. Bucket
  // head_+k is 24-k hours old and goes when the window has moved k more hours.
  for (int k = 1; k <= kHours; ++k) {
    if (counts_[(head_ + k) % kHours] > 0) {
      return hour_start_ms_ + static_cast<unsigned long>(k) * kHourMs - now_ms;
    }
  }
  return kHourMs;  // unreachable while used() >= limit_ > 0
}

DailyBudget::Snapshot DailyBudget::save(unsigned long now_ms) {
  advance(now_ms);
  Snapshot s{};
  for (int i = 0; i < kHours; ++i) {
    s.counts[i] = counts_[i];
  }
  s.head = static_cast<uint8_t>(head_);
  s.into_hour_ms = static_cast<uint32_t>(now_ms - hour_start_ms_);
  return s;
}

void DailyBudget::restore(const Snapshot& s, unsigned long now_ms) {
  for (int i = 0; i < kHours; ++i) {
    counts_[i] = s.counts[i];
  }
  head_ = s.head < kHours ? s.head : 0;
  const unsigned long into = s.into_hour_ms < kHourMs ? s.into_hour_ms : 0;
  // The hour began `into` ms before now; on a fresh clock that can be before
  // zero, which unsigned arithmetic carries through correctly.
  hour_start_ms_ = now_ms - into;
  started_ = true;
}

}  // namespace util
