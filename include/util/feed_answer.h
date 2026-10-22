#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

#include "util/json_budget.h"
#include "util/json_stream.h"

/**
 * How the radar reads a flight feed's answer (readsb JSON: an "ac" list and
 * the feed's clock "now", in either order) and what it makes of it: which
 * aircraft are shown, and whether the answer counts for or against the feed.
 *
 * The list is read one aircraft at a time, each in a document of its own
 * with a memory budget of its own that is given back before the next, so
 * the memory an answer takes no longer grows with it: a busy evening and a
 * fault test's 3000 aircraft take the same. Kept apart from services::adsb
 * so the host tests read answers by the same rules. Free of Arduino headers.
 */
namespace util::feed_answer {

/**
 * Fill `filter` with the fields of one aircraft that are drawn or classified
 * on (an object of keys); the rest never reaches RAM.
 */
void buildFilter(JsonDocument& filter);

/**
 * How far from the point asked about an aircraft can be and still be part of
 * the answer: the feeds only send what lies within the radius (adsb.lol
 * rounds it up to whole nautical miles), so anything well beyond it is from
 * a feed that ignores the radius.
 */
constexpr float kReachFactor = 1.5f;

/** The circle a request asked for. */
struct Area {
  double lat;
  double lon;
  float radius_km;

  /** Ground distance from the centre (flat earth: exact enough at a few radii); NaN in, NaN out. */
  double distanceKm(double lat_deg, double lon_deg) const;
  /** Whether lat/lon lies within kReachFactor radii of the centre. */
  bool reaches(double lat_deg, double lon_deg) const;
};

/**
 * Which of the aircraft offered so far are the `N` nearest, each kept in a
 * slot (0 to size() - 1) of the caller's own list: an aircraft takes a free
 * slot, or once all are taken the farthest one's, or none when it is no
 * nearer than that one.
 */
template <size_t N>
class Nearest {
 public:
  void clear() { count_ = 0; }

  /** The slot to write an aircraft `distance_km` away into, or -1 to drop it. */
  int slotFor(float distance_km) {
    if (count_ < N) {
      distance_[count_] = distance_km;
      return static_cast<int>(count_++);
    }
    size_t farthest = 0;
    for (size_t i = 1; i < N; ++i) {
      if (distance_[i] > distance_[farthest]) {
        farthest = i;
      }
    }
    if (!(distance_km < distance_[farthest])) {
      return -1;
    }
    distance_[farthest] = distance_km;
    return static_cast<int>(farthest);
  }

  size_t size() const { return count_; }

 private:
  float distance_[N] = {};
  size_t count_ = 0;
};

/** Where the aircraft of an answer go as they are read. */
class Sink {
 public:
  /**
   * `plane`: an aircraft read whole (the filter's fields), with a position
   * the area reaches, `distance_km` from its centre, and airborne unless
   * Request::with_ground. It is gone once this returns.
   */
  virtual void offer(JsonObjectConst plane, float distance_km) = 0;

 protected:
  ~Sink() = default;
};

/** What to read an answer for, and with how much memory. */
struct Request {
  Area area;
  /** Offer aircraft on the ground too (alt_baro "ground"). */
  bool with_ground;
  /** util::JsonBudget for each aircraft: its limit, veto and heap. */
  size_t aircraft_bytes;
  JsonBudget::CanSpare can_spare;
  const JsonBudget::Heap* heap;
};

/**
 * ArduinoJson's nesting limit for one aircraft: what the whole answer's
 * default of 10 left it, the answer and its list being two of those levels.
 * Real aircraft go two deep ("mlat": []).
 */
constexpr uint8_t kAircraftNesting = 8;

/** How reading an answer ended. */
enum class End : uint8_t {
  /** Its top-level value was read to its end. */
  kWhole,
  /** The text ended first: the body, the byte cap or the time ran out. */
  kCutShort,
  /** An aircraft did not fit Request::aircraft_bytes: reading stopped there. */
  kRefused,
  /** The heap (Request::can_spare, or the heap itself) refused an aircraft its memory. */
  kNoRoom,
  /** Not JSON, or nested deeper than allowed: Answer::error says how. */
  kMalformed,
};

/** What reading an answer found. */
struct Answer {
  End end = End::kCutShort;
  DeserializationError error = DeserializationError::Ok;
  /** An "ac" array began, and was read to its end. */
  bool list = false;
  bool list_whole = false;
  /** Members of the list read whole, aircraft or not. */
  size_t records = 0;
  /** Of those, the aircraft with a position (on the ground or not). */
  size_t positioned = 0;
  /** Of those, the ones the area reaches. */
  size_t in_area = 0;
  /** Of those, the ones given to the Sink (airborne unless Request::with_ground). */
  size_t offered = 0;
  /** The feed's clock as sent ("now"; Unix time, ms or s), 0 when absent. */
  double now = 0.0;
  /** The most memory one aircraft took (util::JsonBudget::peak()). */
  size_t peak_bytes = 0;
};

/**
 * Read an answer from `body`, offering its aircraft to `sink` as they come.
 * `filter` is buildFilter()'s, made before the answer was asked for (it
 * takes heap, which is shortest while the answer arrives).
 */
Answer read(json_stream::ByteSource& body, JsonVariantConst filter, const Request& request,
            Sink& sink);

/** What to make of an answer. */
enum class Reading : uint8_t {
  /** Read whole: show what was offered. */
  kAircraft,
  /**
   * Ended early, but after an aircraft with a position in the area, or after
   * the end of the list: show what was offered, as the feed answered.
   */
  kCutShort,
  /** The radar's own heap ended it before anything usable: skip, and do not blame the feed. */
  kNoRoom,
  /** Ended early before anything usable: a failure. */
  kUnusable,
  /** Most aircraft with a position lie outside the area: not the answer asked for. */
  kElsewhere,
  /** Whole, but without an "ac" list: an error page, a changed API. */
  kNoList,
  /** Not JSON (Answer::error). */
  kMalformed,
};

Reading judge(const Answer& answer);

/**
 * Unix time from an HTTP Date header ("Tue, 20 Oct 2026 21:15:07 GMT", the
 * only form a server may send). The radar's clock when an answer was cut
 * short before "now", which the feeds send after the list. False, leaving
 * `unix_s` alone, for anything else or a time before 2001.
 */
bool unixFromHttpDate(const char* text, uint32_t* unix_s);

/**
 * Unix time in seconds from a feed's clock (Answer::now: ms as readsb sends
 * it, or s). False, leaving `unix_s` alone, for none, a time before 2001,
 * or one past what uint32_t holds (infinite, or in microseconds after an
 * API change).
 */
bool unixFromFeedClock(double now, uint32_t* unix_s);

}  // namespace util::feed_answer
