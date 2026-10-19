#include "services/geocode.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "services/radar_location.h"
#include "services/tls.h"
#include "util/coords.h"
#include "util/geocode.h"

namespace services::geocode {

namespace {

constexpr char kHost[] = "geocoding-api.open-meteo.com";
/** Answers to rank: the Evanston meant was the second of five. */
constexpr unsigned kCount = 10;
constexpr int kTimeoutMs = 8000;
/** Ten answers are about 4 KB; anything far bigger is not an answer. */
constexpr int kMaxBodyBytes = 32 * 1024;
/**
 * Reported as failed after this long, so the page stops waiting. Covers the
 * slowest real case: an HTTPS handshake plus two searches that time out.
 */
constexpr unsigned long kGiveUpMs = 90000;

/** One answer, ready for services::location. */
struct Place {
  char label[location::kMaxLabel + 1];
  double lat;
  double lon;
};

enum class Outcome : uint8_t { kFound, kNotFound, kFailed, kMoved };

// Shared between the main loop (request, applyResult, the page) and the fetch
// task (runPending); every access goes through s_mux.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/** Given to wake the fetch task early; see idle(). */
SemaphoreHandle_t s_wake = nullptr;
State s_state = State::kIdle;
bool s_running = false;  // the worker has taken the request
bool s_applied = false;
unsigned long s_asked_ms = 0;
util::geocode::Query s_query{};
Place s_place{};

/**
 * Plain HTTP while the service answers it: no handshake, a fraction of the
 * memory. If it starts redirecting, HTTPS from then on, as routes.cpp does.
 */
bool s_https = false;

bool isRedirect(int code) {
  return code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_FOUND ||
         code == HTTP_CODE_TEMPORARY_REDIRECT || code == HTTP_CODE_PERMANENT_REDIRECT;
}

/**
 * GET `url` and stream-parse the answer through `filter` into `doc`. The HTTP
 * status, a negative HTTPClient error, or 0 for a body too big or not JSON.
 */
int get(WiFiClient& client, const char* url, const JsonDocument& filter, JsonDocument& doc) {
  HTTPClient http;
  if (!http.begin(client, url)) {
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  http.useHTTP10(true);  // plain body, no chunk framing to undo
  http.setReuse(false);
  http.setUserAgent(config::kUserAgent);
  http.setConnectTimeout(kTimeoutMs);
  http.setTimeout(kTimeoutMs);
  const int code = http.GET();
  if (code != HTTP_CODE_OK || http.getSize() > kMaxBodyBytes) {
    http.end();
    return code == HTTP_CODE_OK ? 0 : code;
  }
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("geocode: JSON %s\n", err.c_str());
    return 0;
  }
  return code;
}

/** One request for `q`, ranked; the winner in `out`. */
Outcome search(const util::geocode::Query& q, Place* out) {
  char path[256];
  if (!util::geocode::searchPath(q, kCount, path, sizeof(path))) {
    return Outcome::kNotFound;
  }
  char url[320];
  snprintf(url, sizeof(url), "%s://%s%s", s_https ? "https" : "http", kHost, path);

  JsonDocument filter;
  JsonObject f = filter["results"].add<JsonObject>();
  for (const char* key : {"name", "latitude", "longitude", "admin1", "country_code", "country",
                          "feature_code", "population"}) {
    f[key] = true;
  }
  JsonDocument doc;
  int code = 0;
  if (s_https) {
    if (!services::tls::enoughMemory()) {
      return Outcome::kFailed;
    }
    WiFiClientSecure secure;
    services::tls::configure(secure, services::tls::Host::kOpenMeteo, kTimeoutMs);
    code = get(secure, url, filter, doc);
    services::tls::noteResult(secure, services::tls::Host::kOpenMeteo, code);
  } else {
    WiFiClient plain;
    code = get(plain, url, filter, doc);
  }
  if (!s_https && isRedirect(code)) {
    Serial.println("geocode: moved to HTTPS - following");
    s_https = true;
    return Outcome::kMoved;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("geocode: HTTP %d\n", code);
    return Outcome::kFailed;
  }

  util::geocode::Candidate candidates[kCount];
  size_t n = 0;
  for (JsonObjectConst r : doc["results"].as<JsonArrayConst>()) {
    if (n == kCount) {
      break;
    }
    util::geocode::Candidate& c = candidates[n];
    c.name = r["name"] | "";
    c.admin1 = r["admin1"] | "";
    c.country_code = r["country_code"] | "";
    c.country = r["country"] | "";
    c.feature_code = r["feature_code"] | "";
    c.lat = r["latitude"] | 1000.0;
    c.lon = r["longitude"] | 1000.0;
    c.population = r["population"] | 0L;
    if (c.name[0] != '\0' && util::coords::validLatLon(c.lat, c.lon)) {
      ++n;
    }
  }
  const int best = util::geocode::pickBest(candidates, n, q);
  if (best < 0) {
    return Outcome::kNotFound;
  }
  util::geocode::label(candidates[best], out->label, sizeof(out->label));
  out->lat = candidates[best].lat;
  out->lon = candidates[best].lon;
  return Outcome::kFound;
}

/** search(), once more over HTTPS if the service has just moved there. */
Outcome searchFollowing(const util::geocode::Query& q, Place* out) {
  const Outcome o = search(q, out);
  return o == Outcome::kMoved ? search(q, out) : o;
}

}  // namespace

void init() {
  if (s_wake == nullptr) {
    s_wake = xSemaphoreCreateBinary();
  }
}

void idle(unsigned long ms) {
  if (s_wake == nullptr) {
    vTaskDelay(pdMS_TO_TICKS(ms));
    return;
  }
  xSemaphoreTake(s_wake, pdMS_TO_TICKS(ms));
}

void poke() {
  if (s_wake != nullptr) {
    xSemaphoreGive(s_wake);
  }
}

Request request(const char* text) {
  util::geocode::Query q{};
  if (!util::geocode::parseQuery(text, &q)) {
    return Request::kBadText;
  }
  portENTER_CRITICAL(&s_mux);
  const bool busy = s_state == State::kWaiting &&
                    (s_running || millis() - s_asked_ms < kGiveUpMs);
  if (!busy) {
    s_query = q;
    s_state = State::kWaiting;
    s_running = false;
    s_applied = false;
    s_asked_ms = millis();
    s_place = Place{};
  }
  portEXIT_CRITICAL(&s_mux);
  if (busy) {
    return Request::kBusy;
  }
  poke();  // the fetch task may be sleeping out a backoff
  return Request::kQueued;
}

bool runPending() {
  util::geocode::Query q{};
  portENTER_CRITICAL(&s_mux);
  const bool take = s_state == State::kWaiting && !s_running;
  if (take) {
    s_running = true;
    q = s_query;
  }
  portEXIT_CRITICAL(&s_mux);
  if (!take) {
    return false;
  }

  Place place{};
  Outcome o = Outcome::kFailed;
  if (WiFi.status() == WL_CONNECTED) {
    o = searchFollowing(q, &place);
    // "Port Washington" is a town, not Port in Washington state.
    if (o == Outcome::kNotFound && q.region_guessed) {
      o = searchFollowing(util::geocode::asWhole(q), &place);
    }
  }
  const State result = o == Outcome::kFound      ? State::kFound
                       : o == Outcome::kNotFound ? State::kNotFound
                                                 : State::kFailed;
  portENTER_CRITICAL(&s_mux);
  s_place = place;
  s_state = result;
  s_running = false;
  s_applied = false;
  portEXIT_CRITICAL(&s_mux);
  Serial.printf("geocode: '%s' -> %s\n", q.whole,
                result == State::kFound      ? place.label
                : result == State::kNotFound ? "not found"
                                             : "no answer");
  return true;
}

void applyResult() {
  Place place{};
  portENTER_CRITICAL(&s_mux);
  const bool apply = s_state == State::kFound && !s_applied;
  if (apply) {
    s_applied = true;
    place = s_place;
  }
  portEXIT_CRITICAL(&s_mux);
  if (apply) {
    services::location::save(place.lat, place.lon, place.label);
  }
}

State state() {
  portENTER_CRITICAL(&s_mux);
  const State s = s_state;
  const bool stalled = s == State::kWaiting && millis() - s_asked_ms > kGiveUpMs;
  portEXIT_CRITICAL(&s_mux);
  return stalled ? State::kFailed : s;
}

void asked(char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  const size_t n = strnlen(s_query.whole, len - 1);
  std::memcpy(out, s_query.whole, n);
  out[n] = '\0';
  portEXIT_CRITICAL(&s_mux);
}

void found(char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return;
  }
  portENTER_CRITICAL(&s_mux);
  const size_t n = strnlen(s_place.label, len - 1);
  std::memcpy(out, s_place.label, n);
  out[n] = '\0';
  portEXIT_CRITICAL(&s_mux);
}

}  // namespace services::geocode
