#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <ArduinoJson.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "config.h"
#include "services/http_body_framing.h"
#include "services/tls.h"
#include "util/aircraft_class.h"
#include "util/daily_budget.h"
#include "util/feed_failover.h"

namespace services::adsb {

namespace {

/**
 * Where aircraft come from: community ADS-B aggregators with free, readsb-style
 * JSON ("ac" array) for a circle around a point, radius in nautical miles.
 * Both went HTTPS-only in October 2026 (plain HTTP now answers 301).
 * adsb.fi first: its open data API allows one request a second. adsb.lol is
 * the reserve: its headers announce 500 requests a day per address, which a
 * poll every 10 s uses up in under an hour and a half -- so the radar goes
 * back to adsb.fi as soon as it answers again, and leaves a feed alone for a
 * while after it says "too many requests" (429).
 */
struct Feed {
  const char* name;
  /** printf format: latitude, longitude, radius (nm; whole when whole_nm). */
  const char* url;
  services::tls::Host host;
  bool whole_nm;
  /** Requests allowed per 24 h by this device (0: no limit we know of). */
  uint16_t daily_limit;
  /** Once on this feed this long, poll it only every slow_interval_ms. */
  unsigned long slow_after_ms;
  unsigned long slow_interval_ms;
};
// adsb.lol: 500 a day per address, so 400 keeps a margin for anything else
// on the same connection; 10 s for the first 10 min of an adsb.fi outage
// (a blip), then every 150 s, which a whole day stays within.
constexpr Feed kFeeds[] = {
    {"adsb.fi", "https://opendata.adsb.fi/api/v3/lat/%.5f/lon/%.5f/dist/%.1f",
     services::tls::Host::kAdsbFi, false, 0, 0, 0},
    {"adsb.lol", "https://api.adsb.lol/v2/point/%.5f/%.5f/%.0f",
     services::tls::Host::kAdsbLol, true, 400, 10UL * 60UL * 1000UL, 150UL * 1000UL},
};
constexpr size_t kFeedCount = sizeof(kFeeds) / sizeof(kFeeds[0]);
/** Failures in a row before handing over to the next feed. */
constexpr int kFailoverAfter = 2;
/**
 * How long to stay on a reserve feed before trying the first one again. Each
 * minute on adsb.lol spends 6 of its 500 daily requests; a failed try costs
 * one missed poll, as the first failure of a retry hands straight back.
 */
constexpr unsigned long kPrimaryRetryMs = 5UL * 60UL * 1000UL;
/** Rest after a 429 when the feed does not say how long (Retry-After). */
constexpr unsigned long kRateLimitRestMs = 15UL * 60UL * 1000UL;
constexpr unsigned long kRateLimitRestMinMs = 60UL * 1000UL;
constexpr unsigned long kRateLimitRestMaxMs = 6UL * 3600UL * 1000UL;
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectTimeoutMs = 5000;
constexpr unsigned long kRequestTimeoutMs = 8000;
/**
 * Upper bound on the wire bytes of one response. The body is stream-parsed
 * through a field filter (see fetchUpdate), so only kept fields reach RAM, but
 * a hostile or broken server could still send an endless aircraft list; the
 * cap turns that out-of-memory reboot into a logged, recoverable failure.
 * A busy 25 km view is ~40 KB.
 */
constexpr size_t kMaxBodyBytes = 128u * 1024u;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
unsigned long s_last_update_ms = 0;
int s_last_http_status = 0;
volatile unsigned long s_last_attempt_ms = 0;
PollFn s_poll_fn = nullptr;
SemaphoreHandle_t s_mutex = nullptr;

/** Which feed to ask (index into kFeeds). Fetch task only. */
util::FeedFailover s_failover(kFeedCount, kFailoverAfter, kPrimaryRetryMs);
/** s_failover.current(), for the settings page on the main loop. */
volatile size_t s_feed = 0;
/** When the fetch task moved to s_feed (millis()). */
unsigned long s_feed_since_ms = 0;
util::DailyBudget s_budget[kFeedCount] = {util::DailyBudget(kFeeds[0].daily_limit),
                                         util::DailyBudget(kFeeds[1].daily_limit)};

/**
 * Feed state kept through a software restart (self-repair, the daily one), in
 * RTC memory that a restart leaves alone: without it, every restart would
 * forget a refusal or a used-up allowance and start asking again at once.
 */
struct Persisted {
  uint32_t magic;
  /**
   * The firmware that wrote it. An update may be the very fix for a feed's
   * refusal, so it starts afresh instead of inheriting a 6 h rest (and then
   * failing its trial for lack of data).
   */
  char version[32];
  util::FeedFailover::Snapshot failover;
  util::DailyBudget::Snapshot budget[kFeedCount];
  uint32_t checksum;
};
constexpr uint32_t kPersistedMagic = 0x46454544;  // "FEED"
RTC_NOINIT_ATTR Persisted s_persisted;

uint32_t checksumOf(const Persisted& p) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&p);
  uint32_t h = 2166136261u;  // FNV-1a over everything before the checksum
  for (size_t i = 0; i < offsetof(Persisted, checksum); ++i) {
    h = (h ^ bytes[i]) * 16777619u;
  }
  return h;
}

/** Resting/rationed feeds in words; written by the fetch task under s_mutex. */
char s_status[160] = "";

/** Fetch task only: the settings page reads the copy in s_status. */
void composeStatus() {
  char text[sizeof(s_status)] = "";
  const unsigned long now = millis();
  size_t used = 0;
  for (size_t i = 0; i < kFeedCount && used < sizeof(text); ++i) {
    const unsigned long left = s_failover.restLeftMs(i, now);
    int w = 0;
    if (left > 0) {
      const unsigned long min = (left + 59999UL) / 60000UL;
      w = snprintf(text + used, sizeof(text) - used, "%s%s %s, trying again in %lu h %lu min",
                   used ? "; " : "", kFeeds[i].name,
                   s_failover.refusals(i) ? "refused" : "resting", min / 60, min % 60);
    } else if (kFeeds[i].daily_limit > 0 && s_budget[i].used(now) > 0) {
      w = snprintf(text + used, sizeof(text) - used, "%s%s: %u of %u requests today",
                   used ? "; " : "", kFeeds[i].name, s_budget[i].used(now),
                   kFeeds[i].daily_limit);
    }
    if (w > 0) {
      used += static_cast<size_t>(w);
    }
  }
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  memcpy(s_status, text, sizeof(s_status));
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

/** Fetch task only. */
void persist() {
  composeStatus();
  const unsigned long now = millis();
  s_persisted.magic = kPersistedMagic;
  snprintf(s_persisted.version, sizeof(s_persisted.version), "%s", config::kFirmwareVersion);
  s_persisted.failover = s_failover.save(now);
  for (size_t i = 0; i < kFeedCount; ++i) {
    s_persisted.budget[i] = s_budget[i].save(now);
  }
  s_persisted.checksum = checksumOf(s_persisted);
}
/** The feed the aircraft on screen came from (index into kFeeds). */
size_t s_data_feed = 0;
/** Unix time from the last feed answer, and millis() then; 0 = none yet. */
volatile uint32_t s_utc_s = 0;
volatile unsigned long s_utc_at_ms = 0;

/**
 * Set by discardSweepsBefore(): a sweep whose fetch started before
 * s_min_start_ms is for an old centre and is dropped. One-shot — the fetch task
 * runs one fetch at a time, so the stale sweep always lands before the next
 * fresh one, and clearing the filter then keeps millis() wrap-around (49 days)
 * from ever making it drop good sweeps.
 */
bool s_filter_armed = false;
unsigned long s_min_start_ms = 0;

/** Publish parsed aircraft from feed `feed` to the shared buffer atomically. */
void publish(const Aircraft* src, size_t count, unsigned long started_ms, size_t feed) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const bool stale =
      s_filter_armed && static_cast<long>(started_ms - s_min_start_ms) < 0;
  if (!stale) {
    s_filter_armed = false;
    for (size_t i = 0; i < count; ++i) {
      s_aircraft[i] = src[i];
    }
    s_aircraft_count = count;
    s_last_update_ms = millis();  // base time for dead-reckoning
    s_data_feed = feed;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

/**
 * How long a feed that answered 429 asked to be left alone: Retry-After, else
 * X-RateLimit-Reset (adsb.lol), both in seconds; kRateLimitRestMs without.
 */
unsigned long restAfter429(HTTPClient& http) {
  long seconds = http.header("Retry-After").toInt();
  if (seconds <= 0) {
    seconds = http.header("X-RateLimit-Reset").toInt();
  }
  if (seconds <= 0) {
    return kRateLimitRestMs;
  }
  return std::min(kRateLimitRestMaxMs,
                  std::max(kRateLimitRestMinMs, static_cast<unsigned long>(seconds) * 1000UL));
}

/** Log a change of feed, and publish it for the settings page. */
void noteFeed() {
  const size_t feed = s_failover.current();
  if (feed != s_feed) {
    s_feed = feed;
    s_feed_since_ms = millis();
    Serial.printf("adsb: switching to %s\n", kFeeds[feed].name);
  }
  persist();
}

/** 401/403/404 (and 400): blocked, gone, or a request the feed no longer takes. */
bool refusal(int code) { return code == 400 || code == 401 || code == 403 || code == 404; }

/**
 * Count a failed fetch; hand over to the next feed when this one keeps failing
 * or rate-limits us (`rest_ms` > 0: leave it alone that long).
 */
void noteFailure(unsigned long rest_ms = 0) {
  if (rest_ms > 0) {
    Serial.printf("adsb: %s rate limited - resting it %lu min\n",
                  kFeeds[s_failover.current()].name, rest_ms / 60000UL);
  }
  s_failover.failure(millis(), rest_ms);
  noteFeed();
}

void noteSuccess() {
  s_failover.success(millis());
  noteFeed();
}

/**
 * Pumps the socket one byte at a time, in blocks, without buffering the whole
 * response.
 *
 * Each refill runs the network poll callback, which is why HTTPClient's own
 * body readers (getString/writeToStream) can't be used here -- they block
 * without giving the fetch task a chance to poll. That also means they can't
 * de-chunk for us, so the wire image comes out raw and BodyFramer unwraps it.
 *
 * Where the body ends is BodyFramer's business, not this class's: it stops
 * pulling at the right byte. Reading a little past that point into buffer_ is
 * harmless while the connection is torn down after every fetch.
 */
class PollingSocketSource {
 public:
  PollingSocketSource(HTTPClient& http, WiFiClient& stream,
                      unsigned long deadline)
      : http_(&http), stream_(&stream), deadline_(deadline) {}

  /** Next raw byte, or -1 once the socket closes or the deadline passes. */
  int read() {
    if (pos_ >= len_ && !refill()) {
      return -1;
    }
    return static_cast<unsigned char>(buffer_[pos_++]);
  }

 private:
  bool refill() {
    pos_ = 0;
    len_ = 0;
    while (millis() < deadline_) {
      pollNetwork();
      const int available = stream_->available();
      if (available > 0) {
        const int to_read = available > static_cast<int>(sizeof(buffer_))
                                ? static_cast<int>(sizeof(buffer_))
                                : available;
        const int read_bytes = stream_->readBytes(buffer_, to_read);
        if (read_bytes > 0) {
          total_ += static_cast<size_t>(read_bytes);
          if (total_ > kMaxBodyBytes) {
            Serial.printf("adsb: response over %u bytes - dropped\n",
                          static_cast<unsigned>(kMaxBodyBytes));
            return false;
          }
          len_ = static_cast<size_t>(read_bytes);
          return true;
        }
      }
      if (!http_->connected() && stream_->available() <= 0) {
        break;  // server closed and the socket is drained
      }
      delay(1);
    }
    return false;
  }

  HTTPClient* http_;
  WiFiClient* stream_;
  unsigned long deadline_;
  char buffer_[512];
  size_t pos_ = 0;
  size_t len_ = 0;
  size_t total_ = 0;
};

using BodyReader = services::http::BodyFramer<PollingSocketSource>;

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

/**
 * kNoHeading: an aircraft that reports neither track nor heading gets NAN and
 * is drawn without a direction and not dead-reckoned. readsb's "dir" is no
 * substitute: it is the bearing from the point we asked about to the
 * aircraft, so it pointed every such aircraft away from the radar's centre.
 */
float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  return NAN;  // see kNoHeading
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  return NAN;  // see kNoHeading
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
  }
}

/** Optional string field, or "" when the feed omits it. */
const char* jsonStringOr(const JsonObject& obj, const char* key,
                         const char* fallback) {
  if (!obj[key].is<const char*>()) {
    return fallback;
  }
  const char* value = obj[key].as<const char*>();
  return value != nullptr ? value : fallback;
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));

  float alt = 0.0f;
  ac->alt_ft = (readJsonFloat(plane, "alt_baro", &alt) ||
                readJsonFloat(plane, "alt_geom", &alt))
                   ? static_cast<int32_t>(lroundf(alt))
                   : kAltUnknown;
  float rate = 0.0f;
  if (readJsonFloat(plane, "baro_rate", &rate) ||
      readJsonFloat(plane, "geom_rate", &rate)) {
    rate = std::max(-32000.0f, std::min(32000.0f, rate));
    ac->vrate_fpm = static_cast<int16_t>(lroundf(rate));
  }

  // category, emergency, squawk and dbFlags are all optional in the readsb
  // schema these feeds derive from, and most aircraft send none of them; every
  // classifier below falls back rather than guessing.
  const char* category = jsonStringOr(plane, "category", "");
  const char* emergency = jsonStringOr(plane, "emergency", "");
  const char* squawk = jsonStringOr(plane, "squawk", "");
  const uint32_t db_flags =
      plane["dbFlags"].is<unsigned>() ? plane["dbFlags"].as<unsigned>() : 0u;

  const util::aircraft::Shape shape = util::aircraft::shapeOf(ac->type, category);
  ac->klass = static_cast<uint8_t>(shape.klass);
  ac->engines = shape.engines;
  ac->rear_engines = shape.rear_engines;
  ac->alert_flags =
      util::aircraft::alertFlags(ac->type, emergency, squawk, db_flags);
}

}  // namespace

void discardSweepsBefore(unsigned long ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  s_min_start_ms = ms;
  s_filter_armed = true;
  s_aircraft_count = 0;
  s_last_update_ms = 0;
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

void init() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
  if (s_persisted.magic == kPersistedMagic && s_persisted.checksum == checksumOf(s_persisted) &&
      strncmp(s_persisted.version, config::kFirmwareVersion, sizeof(s_persisted.version)) == 0) {
    const unsigned long now = millis();
    s_failover.restore(s_persisted.failover, now);
    for (size_t i = 0; i < kFeedCount; ++i) {
      s_budget[i].restore(s_persisted.budget[i], now);
    }
    s_feed = s_failover.current();
    s_feed_since_ms = now;
    Serial.printf("adsb: feed state kept from before the restart (on %s)\n",
                  kFeeds[s_feed].name);
  }
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

unsigned long lastUpdateMs() { return s_last_update_ms; }

int lastHttpStatus() { return s_last_http_status; }

unsigned long lastAttemptMs() { return s_last_attempt_ms; }

size_t snapshotAircraft(Aircraft* out, size_t max_out,
                        unsigned long* out_last_update_ms) {
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  const size_t count =
      s_aircraft_count < max_out ? s_aircraft_count : max_out;
  for (size_t i = 0; i < count; ++i) {
    out[i] = s_aircraft[i];
  }
  if (out_last_update_ms != nullptr) {
    *out_last_update_ms = s_last_update_ms;
  }
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
  return count;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km,
                 unsigned long started_ms) {
  s_last_attempt_ms = started_ms;
  const bool usable = s_failover.select(millis());
  noteFeed();
  if (!usable) {
    // Every feed asked for a rest: wait it out (the fetch task backs off).
    s_last_http_status = 429;
    s_failover.failure(millis());
    return false;
  }
  const size_t feed_index = s_failover.current();
  const Feed& feed = kFeeds[feed_index];
  if (feed.daily_limit > 0 && s_budget[feed_index].used(millis()) >= feed.daily_limit) {
    const unsigned long wait_ms = s_budget[feed_index].msUntilFree(millis());
    Serial.printf("adsb: %s daily allowance used - resting it %lu min\n", feed.name,
                  wait_ms / 60000UL);
    s_failover.rest(millis(), wait_ms);
    noteFeed();
    s_last_http_status = 429;
    return false;
  }
  if (!services::tls::enoughMemory()) {
    Serial.printf("adsb: not enough memory for HTTPS (largest block %u) - skipped\n",
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    s_last_http_status = 0;
    return false;  // the radar's problem, not the feed's: no failover
  }

  char url[112];
  const float nm = kmToNauticalMiles(fetch_radius_km);
  snprintf(url, sizeof(url), feed.url, center_lat, center_lon, feed.whole_nm ? ceilf(nm) : nm);

  // Keep only the fields we render or classify on (fillTagFields); the rest
  // never reaches RAM.
  JsonDocument filter;
  filter["now"] = true;  // the feed's clock: tells the radar when night falls
  JsonObject f = filter["ac"].add<JsonObject>();
  for (const char* key :
       {"lat", "lon", "true_heading", "mag_heading", "track", "dir", "gs",
        "tas", "ias", "alt_baro", "alt_geom", "seen_pos", "flight", "hex", "t",
        "category", "emergency", "squawk", "dbFlags", "baro_rate",
        "geom_rate"}) {
    f[key] = true;
  }

  WiFiClientSecure client;
  services::tls::configure(client, feed.host, kRequestTimeoutMs);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    noteFailure();
    return false;
  }
  http.setReuse(false);
  http.setUserAgent(config::kUserAgent);
  // A feed that moves its endpoint usually says so with a redirect.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(2);

  // HTTPClient only records Transfer-Encoding in the collected headers when
  // it is asked for up front; _transferEncoding itself is private.
  // The other two say how long to rest a feed that answers 429.
  static const char* kWantedHeaders[] = {"Transfer-Encoding", "Retry-After",
                                         "X-RateLimit-Reset"};
  http.collectHeaders(kWantedHeaders, 3);

  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kRequestTimeoutMs);
  if (feed.daily_limit > 0) {
    s_budget[feed_index].take(millis());  // counted when sent, whatever comes back
  }
  const int code = http.GET();
  services::tls::noteResult(client, feed.host, code);
  s_last_http_status = code;
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: %s HTTP %d\n", feed.name, code);
    const unsigned long rest_ms = code == 429 ? restAfter429(http) : 0;
    http.end();
    if (refusal(code)) {
      s_failover.refused(millis());
      Serial.printf("adsb: %s refused (%d) - resting it %lu h\n", feed.name, code,
                    s_failover.restLeftMs(feed_index, millis()) / 3600000UL);
      noteFeed();
    } else {
      noteFailure(rest_ms);
    }
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: no response stream");
    http.end();
    noteFailure();
    return false;
  }

  // On HTTP/1.1 the CDN answers with Transfer-Encoding: chunked, and
  // getStreamPtr() hands back the raw socket -- chunk sizes and all. BodyFramer
  // strips that framing back off.
  const services::http::BodyFraming framing =
      http.header("Transfer-Encoding").equalsIgnoreCase("chunked")
          ? services::http::BodyFraming::kChunked
          : services::http::BodyFraming::kIdentity;

  PollingSocketSource source(http, *stream, millis() + kRequestTimeoutMs);
  BodyReader body(source, framing, http.getSize());
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));
  // Read off the terminating chunk the parser stopped short of, so the socket
  // sits at the end of the message. Not needed while every fetch builds its own
  // connection, but a prerequisite for ever reusing one.
  body.drain();
  http.end();
  if (err) {
    if (body.framingError()) {
      Serial.println("adsb: malformed chunked body");
    } else if (body.bytesRead() == 0) {
      Serial.println("adsb: empty response");
    } else {
      Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    }
    noteFailure();
    return false;
  }
  // Both feeds send "ac": [] for an empty sky. Without the list this is not
  // an answer we understand (an error page, a changed API): count it as a
  // failure, so the other feed gets a turn and the no-data restart still works.
  if (!doc["ac"].is<JsonArray>()) {
    Serial.printf("adsb: %s answered without an aircraft list\n", feed.name);
    s_last_http_status = 0;
    noteFailure();
    return false;
  }

  // Parse into a buffer of its own, then publish atomically so a reader on
  // another thread never sees a half-updated list. Static: 4 KB is too much
  // for the fetch task's stack on top of a TLS handshake (fetch task only).
  static Aircraft parsed[kMaxAircraft];
  size_t n = 0;
  JsonArray ac = doc["ac"].as<JsonArray>();
  if (!ac.isNull()) {
    for (JsonObject plane : ac) {
      if (n >= kMaxAircraft) {
        break;
      }
      if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
        continue;
      }
      if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
        continue;
      }

      parsed[n] = Aircraft{};
      parsed[n].lat = plane["lat"].as<float>();
      parsed[n].lon = plane["lon"].as<float>();
      parsed[n].nose_deg = pickNoseHeading(plane);
      parsed[n].track_deg = pickTrackHeading(plane);
      parsed[n].gs_knots = pickGroundSpeed(plane);

      // seen_pos: seconds since this position was measured. Use it as the
      // dead-reckoning age offset, capped so a very stale fix isn't flung far.
      float seen_pos = 0.0f;
      readJsonFloat(plane, "seen_pos", &seen_pos);
      if (seen_pos < 0.0f) seen_pos = 0.0f;
      if (seen_pos > 30.0f) seen_pos = 30.0f;
      parsed[n].pos_age_ms = static_cast<uint32_t>(seen_pos * 1000.0f);

      fillTagFields(&parsed[n], plane);
      ++n;
    }
  }

  // "now" is Unix time in milliseconds (readsb); accept seconds too.
  const double feed_now = doc["now"] | 0.0;
  if (feed_now > 1.0e12) {
    s_utc_s = static_cast<uint32_t>(feed_now / 1000.0);
    s_utc_at_ms = millis();
  } else if (feed_now > 1.0e9) {
    s_utc_s = static_cast<uint32_t>(feed_now);
    s_utc_at_ms = millis();
  }
  publish(parsed, n, started_ms, feed_index);
  Serial.printf("adsb: %s %u aircraft\n", feed.name, static_cast<unsigned>(n));
  noteSuccess();
  return true;
}

const char* feedName() { return kFeeds[s_feed].name; }

bool utcNow(uint32_t* unix_s) {
  const uint32_t base = s_utc_s;
  const unsigned long at = s_utc_at_ms;
  if (base == 0) {
    return false;
  }
  *unix_s = base + static_cast<uint32_t>((millis() - at) / 1000UL);
  return true;
}

const char* dataFeedName() { return kFeeds[s_data_feed].name; }

bool dataVerified() { return services::tls::verifying(kFeeds[s_data_feed].host); }

bool allFeedsFailing() { return s_failover.allFailing(); }

bool allFeedsResting() { return s_failover.allResting(millis()); }

unsigned long wantedIntervalMs() {
  const Feed& feed = kFeeds[s_feed];
  if (feed.slow_after_ms > 0 && millis() - s_feed_since_ms >= feed.slow_after_ms) {
    return feed.slow_interval_ms;
  }
  return config::kAdsbFetchIntervalMs;
}

void feedStatusText(char* out, size_t n) {
  if (n == 0) {
    return;
  }
  if (s_mutex != nullptr) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
  snprintf(out, n, "%s", s_status);
  if (s_mutex != nullptr) {
    xSemaphoreGive(s_mutex);
  }
}

}  // namespace services::adsb
