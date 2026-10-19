#include "services/routes.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/tls.h"
#include "data/large_airports.h"
#include "util/aircraft_class.h"
#include "util/airport_guess.h"
#include "util/callsign.h"
#include "util/format.h"

namespace services::routes {

namespace {

/**
 * Plain HTTP while adsb.im still serves it: it costs no TLS handshake. The
 * position feeds went HTTPS-only without warning; when this one follows suit
 * (a redirect), the radar switches to HTTPS by itself.
 */
constexpr char kUrl[] = "http://adsb.im/api/0/routeset";
constexpr char kUrlHttps[] = "https://adsb.im/api/0/routeset";
/** Flights per request, and the pause between requests. */
constexpr size_t kBatch = 8;
constexpr unsigned long kMinIntervalMs = 20000;
/** After a failed request, wait this long before trying again. */
constexpr unsigned long kFailBackoffMs = 120000;
constexpr int kTimeoutMs = 6000;
/** Eight answers are ~3 KB; anything far bigger is not an answer. */
constexpr int kMaxBodyBytes = 32 * 1024;

util::RouteCache s_cache;
SemaphoreHandle_t s_mutex = nullptr;
unsigned long s_next_ms = 0;
bool s_https = false;

void lock() {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
}
void unlock() {
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void copyString(char* out, size_t len, const char* in) {
  snprintf(out, len, "%s", in != nullptr ? in : "");
}

struct Candidate {
  char callsign[9];
  float lat;
  float lon;
  float dist2;
};

/** Up to kBatch airline flights without a fresh answer, nearest first. */
size_t pickCandidates(Candidate* out) {
  static services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  const size_t n =
      services::adsb::snapshotAircraft(planes, services::adsb::kMaxAircraft, nullptr);
  const float clat = static_cast<float>(services::location::lat());
  const float clon = static_cast<float>(services::location::lon());
  const float coslat = cosf(clat * 0.01745329252f);
  const unsigned long now = millis();

  size_t count = 0;
  for (size_t i = 0; i < n; ++i) {
    char prefix[4];
    if (!util::callsign::icaoPrefix(planes[i].callsign, prefix)) {
      continue;  // registrations and hex ids have no scheduled route
    }
    lock();
    const bool needed = s_cache.needsLookup(planes[i].callsign, now);
    unlock();
    if (!needed) {
      continue;
    }
    const float dy = planes[i].lat - clat;
    const float dx = (planes[i].lon - clon) * coslat;
    Candidate c{};
    copyString(c.callsign, sizeof(c.callsign), planes[i].callsign);
    c.lat = planes[i].lat;
    c.lon = planes[i].lon;
    c.dist2 = dx * dx + dy * dy;
    // Insertion into a nearest-first list of at most kBatch.
    size_t at = count;
    while (at > 0 && out[at - 1].dist2 > c.dist2) {
      if (at < kBatch) {
        out[at] = out[at - 1];
      }
      --at;
    }
    if (at < kBatch) {
      out[at] = c;
      if (count < kBatch) {
        ++count;
      }
    }
  }
  return count;
}

/**
 * POST `body` to `url` and stream-parse the answer through `filter` into
 * `doc`. Returns the HTTP status, a negative HTTPClient error, or 0 for a
 * response that is too big or not JSON.
 */
int post(WiFiClient& client, const char* url, const String& body,
         const JsonDocument& filter, JsonDocument& doc) {
  HTTPClient http;
  if (!http.begin(client, url)) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  http.useHTTP10(true);  // plain body, no chunk framing to undo
  http.setReuse(false);
  http.setUserAgent(config::kUserAgent);
  http.setConnectTimeout(kTimeoutMs);
  http.setTimeout(kTimeoutMs);
  http.addHeader("Content-Type", "application/json");
  const int code = http.POST(body);
  if (code != HTTP_CODE_OK || http.getSize() > kMaxBodyBytes) {
    Serial.printf("routes: HTTP %d\n", code);
    http.end();
    return code == HTTP_CODE_OK ? 0 : code;
  }
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("routes: JSON %s\n", err.c_str());
    return 0;
  }
  return code;
}

}  // namespace

void init() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

void refresh() {
  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED || static_cast<long>(now - s_next_ms) < 0) {
    return;
  }
  Candidate batch[kBatch];
  const size_t count = pickCandidates(batch);
  if (count == 0) {
    return;
  }

  String body = "{\"planes\":[";
  for (size_t i = 0; i < count; ++i) {
    char item[80];
    snprintf(item, sizeof(item), "%s{\"callsign\":\"%s\",\"lat\":%.4f,\"lng\":%.4f}",
             i == 0 ? "" : ",", batch[i].callsign, batch[i].lat, batch[i].lon);
    body += item;
  }
  body += "]}";

  JsonDocument filter;
  filter[0]["callsign"] = true;
  filter[0]["_airport_codes_iata"] = true;
  filter[0]["plausible"] = true;
  JsonDocument doc;
  int code = 0;
  if (s_https) {
    if (!services::tls::enoughMemory()) {
      s_next_ms = now + kMinIntervalMs;
      return;
    }
    WiFiClientSecure secure;
    services::tls::configure(secure, services::tls::Host::kAdsbIm, kTimeoutMs);
    code = post(secure, kUrlHttps, body, filter, doc);
    services::tls::noteResult(secure, services::tls::Host::kAdsbIm, code);
  } else {
    WiFiClient plain;
    code = post(plain, kUrl, body, filter, doc);
  }
  if (!s_https && (code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
                   code == HTTP_CODE_TEMPORARY_REDIRECT ||
                   code == HTTP_CODE_PERMANENT_REDIRECT)) {
    Serial.println("routes: adsb.im moved to HTTPS - following");
    s_https = true;
    s_next_ms = now;
    return;
  }
  if (code != HTTP_CODE_OK) {
    s_next_ms = now + kFailBackoffMs;
    return;
  }
  s_next_ms = now + kMinIntervalMs;

  size_t known = 0;
  const unsigned long stored = millis();
  for (JsonObject entry : doc.as<JsonArray>()) {
    util::Route r{};
    copyString(r.callsign, sizeof(r.callsign), entry["callsign"] | "");
    const char* codes = entry["_airport_codes_iata"] | "unknown";
    copyString(r.codes, sizeof(r.codes), codes);
    r.known = strcmp(codes, "unknown") != 0 && (entry["plausible"] | false);
    if (r.callsign[0] == '\0') {
      continue;
    }
    known += r.known ? 1 : 0;
    Serial.printf("routes: %s %s%s\n", r.callsign, codes,
                  r.known || strcmp(codes, "unknown") == 0 ? "" : " (implausible)");
    lock();
    s_cache.put(r, stored);
    unlock();
  }
  // Anything the service skipped counts as unknown, so it is not asked again
  // on every sweep.
  for (size_t i = 0; i < count; ++i) {
    lock();
    const bool answered = !s_cache.needsLookup(batch[i].callsign, stored);
    if (!answered) {
      util::Route r{};
      copyString(r.callsign, sizeof(r.callsign), batch[i].callsign);
      copyString(r.codes, sizeof(r.codes), "unknown");
      s_cache.put(r, stored);
    }
    unlock();
  }
  Serial.printf("routes: %u asked, %u known\n", static_cast<unsigned>(count),
                static_cast<unsigned>(known));
}

namespace {

/** Major airports near the radar centre, for guessing; rebuilt when it moves. */
constexpr size_t kMaxNearby = 24;
/** Aircraft are drawn up to ~30 km out and guesses reach 35 km past them. */
constexpr float kNearbyKm = 70.0f;
util::airport_guess::Airport s_nearby[kMaxNearby];
const char* s_nearby_iata[kMaxNearby];
size_t s_nearby_count = 0;
double s_nearby_lat = 1000.0;
double s_nearby_lon = 1000.0;

void refreshNearbyAirports() {
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  if (lat == s_nearby_lat && lon == s_nearby_lon) {
    return;
  }
  s_nearby_lat = lat;
  s_nearby_lon = lon;
  s_nearby_count = 0;
  const float coslat = cosf(static_cast<float>(lat) * 0.01745329252f);
  for (size_t i = 0; i < data::large_airports::kAirportCount && s_nearby_count < kMaxNearby;
       ++i) {
    const auto& ap = data::large_airports::kAirports[i];
    if (ap.iata[0] == '\0') {
      continue;
    }
    const float alat = ap.lat_e7 * 1e-7f;
    const float alon = ap.lon_e7 * 1e-7f;
    const float dn = (alat - static_cast<float>(lat)) * 111.0f;
    const float de = (alon - static_cast<float>(lon)) * 111.0f * coslat;
    if (dn * dn + de * de <= kNearbyKm * kNearbyKm) {
      s_nearby[s_nearby_count] = {alat, alon};
      s_nearby_iata[s_nearby_count] = ap.iata;
      ++s_nearby_count;
    }
  }
}

}  // namespace

namespace {

/**
 * Is this airline flight clearly landing at or leaving a nearby major airport?
 * Never for helicopters or light aircraft: they circle at low level and
 * rarely use the big airports ("-> MDW" for a sightseeing R44 was exactly the
 * wrong answer a blank avoids).
 */
bool guessAirport(const services::adsb::Aircraft& plane, float lat, float lon,
                  util::airport_guess::Guess* out) {
  char prefix[4];
  const auto klass = static_cast<util::aircraft::Klass>(plane.klass);
  if (!util::callsign::icaoPrefix(plane.callsign, prefix) || std::isnan(plane.track_deg) ||
      klass == util::aircraft::Klass::kRotor || klass == util::aircraft::Klass::kLight) {
    return false;
  }
  refreshNearbyAirports();
  const util::airport_guess::Plane p{lat, lon, plane.alt_ft, plane.vrate_fpm,
                                     plane.track_deg, plane.gs_knots};
  *out = util::airport_guess::guess(p, s_nearby, s_nearby_count);
  return out->kind != util::airport_guess::Kind::kNone;
}

}  // namespace

Shown describe(const services::adsb::Aircraft& plane, float lat, float lon) {
  Shown shown;
  util::airport_guess::Guess g;
  const bool guessed = guessAirport(plane, lat, lon, &g);
  const bool arriving = guessed && g.kind == util::airport_guess::Kind::kArriving;

  util::Route route{};
  if (lookup(plane.callsign, &route)) {
    shown.kind = Shown::Kind::kRoute;
    copyString(shown.codes, sizeof(shown.codes), route.codes);
    // A flight number that covers a day's trip ("ORD-CHS-ORD", "SNA-ORD-IAD")
    // is cut to the leg being flown when the aircraft is clearly landing at or
    // leaving one of its stops; otherwise the whole chain is shown rather
    // than a misleading "ORD -> ORD".
    char leg[16];
    const char* first_dash = strchr(route.codes, '-');
    const bool multi_stop = first_dash != nullptr && strchr(first_dash + 1, '-') != nullptr;
    if (guessed && multi_stop &&
        util::format::routeLeg(route.codes, s_nearby_iata[g.airport], arriving, leg,
                               sizeof(leg))) {
      copyString(shown.codes, sizeof(shown.codes), leg);
    }
    return shown;
  }
  if (!guessed) {
    return shown;
  }
  shown.kind = arriving ? Shown::Kind::kArriving : Shown::Kind::kDeparting;
  copyString(shown.codes, sizeof(shown.codes), s_nearby_iata[g.airport]);
  return shown;
}

bool lookup(const char* callsign, util::Route* out) {
  lock();
  const bool found = s_cache.find(callsign, millis(), out);
  unlock();
  return found && out->known;
}

}  // namespace services::routes
