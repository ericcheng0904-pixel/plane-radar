#include "util/feed_answer.h"

#include <cctype>
#include <cmath>
#include <cstring>

namespace util::feed_answer {

namespace {

constexpr double kKmPerDeg = 111.2;
constexpr double kRadPerDeg = 3.14159265358979323846 / 180.0;

/** Exactly `count` decimal digits at `p`, which moves past them. */
bool digits(const char*& p, int count, int* out) {
  int value = 0;
  for (int i = 0; i < count; ++i, ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    value = value * 10 + (*p - '0');
  }
  *out = value;
  return true;
}

/** `text` at `p`, which moves past it. */
bool literal(const char*& p, const char* text) {
  const size_t n = strlen(text);
  if (strncmp(p, text, n) != 0) {
    return false;
  }
  p += n;
  return true;
}

/** Days from 1970-01-01 to year-month-day (proleptic Gregorian). */
long daysFromCivil(int year, int month, int day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const int yoe = year - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<long>(era) * 146097L + doe - 719468L;
}

int daysInMonth(int year, int month) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : kDays[month - 1];
}

using Code = DeserializationError::Code;
using json_stream::Input;
using json_stream::skipSpace;

bool isOnGround(JsonObjectConst plane) {
  const char* alt = plane["alt_baro"];
  return alt != nullptr && strcmp(alt, "ground") == 0;
}

/**
 * One answer being read. The walk goes through the top-level object a byte
 * at a time: the keys it does not want and their values are passed over
 * (json_stream::skipValue), "now" is read as a number, and "ac" a member at
 * a time. Each aircraft is parsed into a document of its own, with its own
 * budget, both gone before the next one is read. Every step returns false
 * once the walk has ended (answer.end says how).
 */
struct Walk {
  Input& in;
  JsonVariantConst filter;
  const Request& request;
  Sink& sink;
  Answer& answer;

  /**
   * End on `code`. Once the input has ended, that is the text cut short (a
   * number or a word missing its end reads as a mistake), not a mistake in it.
   */
  bool stop(Code code) {
    if (in.ended()) {
      answer.end = End::kCutShort;
    } else {
      answer.end = End::kMalformed;
      answer.error = code;
    }
    return false;
  }

  /** The whole answer: an object, or (not understood, but JSON) anything else. */
  void run() {
    int c = skipSpace(in);
    if (c != '{') {
      in.unread(c);
      const Code err = json_stream::skipValue(in);
      if (err != Code::Ok) {
        stop(err);
        return;
      }
      answer.end = End::kWhole;
      return;
    }
    c = skipSpace(in);
    if (c == '}') {
      answer.end = End::kWhole;
      return;
    }
    for (;;) {
      if (c != '"') {
        stop(Code::InvalidInput);
        return;
      }
      if (!member()) {
        return;
      }
      c = skipSpace(in);
      if (c == '}') {
        answer.end = End::kWhole;  // anything after it is not read
        return;
      }
      if (c != ',') {
        stop(Code::InvalidInput);
        return;
      }
      c = skipSpace(in);
    }
  }

  /** A member of the top-level object, its key's opening quote read. */
  bool member() {
    // Room for "now": a longer key is none the radar reads, and is read past.
    char key[4];
    bool fits = false;
    Code err = json_stream::readString(in, key, sizeof(key), &fits);
    if (err != Code::Ok) {
      return stop(err);
    }
    if (skipSpace(in) != ':') {
      return stop(Code::InvalidInput);
    }
    const int c = skipSpace(in);
    in.unread(c);
    if (fits && strcmp(key, "ac") == 0 && c == '[') {
      in.read();
      return list();
    }
    if (fits && strcmp(key, "now") == 0 && (c == '-' || (c >= '0' && c <= '9'))) {
      err = json_stream::readNumber(in, &answer.now);
    } else {
      err = json_stream::skipValue(in);  // "ac" that is not a list counts as none
    }
    return err == Code::Ok || stop(err);
  }

  /** The aircraft list, its opening bracket read. */
  bool list() {
    answer.list = true;
    int c = skipSpace(in);
    if (c == ']') {
      answer.list_whole = true;
      return true;
    }
    for (;;) {
      in.unread(c);
      if (c == '{') {
        if (!aircraft()) {
          return false;
        }
      } else {
        // Not an aircraft (readsb sends none of these): passed over.
        const Code err = json_stream::skipValue(in);
        if (err != Code::Ok) {
          return stop(err);
        }
        ++answer.records;
      }
      c = skipSpace(in);
      if (c == ']') {
        answer.list_whole = true;
        return true;
      }
      if (c != ',') {
        return stop(Code::InvalidInput);
      }
      c = skipSpace(in);
    }
  }

  /** One aircraft, its opening brace next. */
  bool aircraft() {
    JsonBudget budget(request.aircraft_bytes, request.can_spare, request.heap);
    JsonDocument doc(&budget);
    // deserializeJson() reads an object up to its closing brace and no
    // further (proved by the host tests), so the walk goes on from there.
    const DeserializationError err =
        deserializeJson(doc, in, DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(kAircraftNesting));
    if (budget.peak() > answer.peak_bytes) {
      answer.peak_bytes = budget.peak();
    }
    // A refusal ArduinoJson got past would leave a value out: not read whole.
    if (err == DeserializationError::NoMemory || budget.refused()) {
      answer.end = budget.vetoed() ? End::kNoRoom : End::kRefused;
      return false;
    }
    if (err) {
      return stop(err.code());
    }
    ++answer.records;
    JsonObjectConst plane = doc.as<JsonObjectConst>();
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      return true;
    }
    ++answer.positioned;
    // Area::reaches(), keeping the distance for the sink.
    const double distance_km =
        request.area.distanceKm(plane["lat"].as<double>(), plane["lon"].as<double>());
    if (!(distance_km <= static_cast<double>(kReachFactor) * request.area.radius_km)) {
      return true;  // not part of the answer asked for (see kReachFactor)
    }
    ++answer.in_area;
    if (!request.with_ground && isOnGround(plane)) {
      return true;
    }
    ++answer.offered;
    sink.offer(plane, static_cast<float>(distance_km));
    return true;
  }
};

}  // namespace

void buildFilter(JsonDocument& filter) {
  for (const char* key :
       {"lat", "lon", "true_heading", "mag_heading", "track", "gs", "tas", "ias", "alt_baro",
        "alt_geom", "seen_pos", "flight", "hex", "t", "category", "emergency", "squawk",
        "dbFlags", "baro_rate", "geom_rate"}) {
    filter[key] = true;
  }
}

double Area::distanceKm(double lat_deg, double lon_deg) const {
  // Anything far away stays far: the longitude difference wraps at the
  // antimeridian.
  const double dx =
      std::remainder(lon_deg - lon, 360.0) * std::cos(lat * kRadPerDeg) * kKmPerDeg;
  const double dy = (lat_deg - lat) * kKmPerDeg;
  return std::sqrt(dx * dx + dy * dy);
}

bool Area::reaches(double lat_deg, double lon_deg) const {
  // False for NaN, as every comparison with it is.
  return distanceKm(lat_deg, lon_deg) <= static_cast<double>(kReachFactor) * radius_km;
}

Answer read(json_stream::ByteSource& body, JsonVariantConst filter, const Request& request,
            Sink& sink) {
  json_stream::Input in(body);
  Answer answer;
  Walk{in, filter, request, sink, answer}.run();
  return answer;
}

Reading judge(const Answer& answer) {
  if (answer.end == End::kMalformed) {
    return Reading::kMalformed;
  }
  // Most, not all: one odd position must not cost a good answer. An empty
  // sky (none positioned) is an answer too.
  if (answer.in_area * 2 < answer.positioned) {
    return Reading::kElsewhere;
  }
  if (answer.end == End::kWhole) {
    return answer.list ? Reading::kAircraft : Reading::kNoList;
  }
  // The feed answered for here: the other one would send the same, while
  // spending adsb.lol's daily allowance.
  if (answer.in_area > 0 || answer.list_whole) {
    return Reading::kCutShort;
  }
  return answer.end == End::kNoRoom ? Reading::kNoRoom : Reading::kUnusable;
}

bool unixFromHttpDate(const char* text, uint32_t* unix_s) {
  if (text == nullptr) {
    return false;
  }
  // IMF-fixdate (RFC 9110): "Sun, 06 Nov 1994 08:49:37 GMT". The day's name
  // is not checked against the date.
  const char* p = text;
  for (int i = 0; i < 3; ++i, ++p) {
    if (!isalpha(static_cast<unsigned char>(*p))) {
      return false;
    }
  }
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!literal(p, ", ") || !digits(p, 2, &day) || !literal(p, " ")) {
    return false;
  }
  static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int month = 0;
  for (int m = 0; m < 12 && month == 0; ++m) {
    if (strncmp(p, kMonths + 3 * m, 3) == 0) {
      month = m + 1;
    }
  }
  if (month == 0) {
    return false;
  }
  p += 3;
  if (!literal(p, " ") || !digits(p, 4, &year) || !literal(p, " ") || !digits(p, 2, &hour) ||
      !literal(p, ":") || !digits(p, 2, &minute) || !literal(p, ":") ||
      !digits(p, 2, &second) || !literal(p, " GMT") || *p != '\0') {
    return false;
  }
  if (day < 1 || day > daysInMonth(year, month) || hour > 23 || minute > 59 || second > 60) {
    return false;
  }
  const double t = static_cast<double>(daysFromCivil(year, month, day)) * 86400.0 +
                   hour * 3600.0 + minute * 60.0 + second;
  if (t < 1.0e9 || t > 4294967295.0) {
    return false;  // before 2001 (as for "now"), or past what uint32_t holds
  }
  *unix_s = static_cast<uint32_t>(t);
  return true;
}

bool unixFromFeedClock(double now, uint32_t* unix_s) {
  const double t = now > 1.0e12 ? now / 1000.0 : now;
  // Written so NaN fails too.
  if (!(t >= 1.0e9 && t <= 4294967295.0)) {
    return false;
  }
  *unix_s = static_cast<uint32_t>(t);
  return true;
}

}  // namespace util::feed_answer
