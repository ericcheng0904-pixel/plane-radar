#include "ui/radar_display.h"

#include <Arduino.h>
#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "data/airline_brands.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/routes.h"
#include "services/wifi_setup.h"
#include "ui/aircraft_glyph.h"
#include "ui/blend.h"
#include "ui/alert.h"
#include "ui/info_card.h"
#include "ui/panel_color.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"
#include "ui/theme.h"
#include "ui/trails.h"
#include "ui/water_overlay.h"
#include "ui/wide_line.h"
#include "util/aircraft_class.h"
#include "util/solar.h"
#include "util/format.h"

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
const lgfx::GFXfont* s_cardinal_gfx = &fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;

lgfx::LovyanGFX* s_draw = &tft;
/**
 * The frame is drawn in two halves through one 240 x 120 buffer. A whole
 * 240 x 240 frame (115 KB) left no room for the TLS connection the ADS-B feeds
 * now insist on; half of it does. s_strip owns the pixels and sends them to
 * the panel; s_frame is a full-screen view onto them, clipped to the half being
 * drawn, so everything keeps drawing in screen coordinates.
 */
constexpr int kStripRows = radar::kSize / 2;
constexpr int kStrips = radar::kSize / kStripRows;
LGFX_Sprite s_strip(&tft);
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;
/** The frame's clock: both halves are drawn at one instant, so they meet without a seam. */
unsigned long s_now_ms = 0;

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

/** Font sizes for the active text size step (portal setting). */
const radar::FontStepFonts& stepFonts() {
  constexpr size_t kStepCount =
      sizeof(radar::kFontStepFonts) / sizeof(radar::kFontStepFonts[0]);
  const uint8_t step = radar::fontStep();
  return radar::kFontStepFonts[step < kStepCount ? step : 0];
}

/** Smooth fonts draw at their native size; the bitmap fallback picks by height. */
void applyRoleStyle(size_t font_index, const lgfx::GFXfont* fallback) {
  if (displayFontIsSmooth()) {
    displayFontEnsureLoaded(*s_draw, font_index);
  } else {
    displayFontSetBitmap(*s_draw, fallback);
  }
}

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  if (!displayFontIsSmooth()) {
    const lgfx::GFXfont* candidates[] = {&fonts::FreeSansBold12pt7b,
                                         &fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(kUiFontPx[stepFonts().cardinal], candidates, 2);
    s_scale_gfx =
        pickGfxFontClosest(kUiFontPx[stepFonts().scale], candidates, 2);
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  if (!displayFontIsSmooth()) {
    const lgfx::GFXfont* candidates[] = {&fonts::FreeSansBold12pt7b,
                                         &fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(kUiFontPx[stepFonts().tag], candidates, 2);
  }

  s_tag_label_metrics_ready = true;
}

/**
 * GC9A01 modules with a BGR panel need R and B swapped for a colour to come out
 * as intended. The aircraft red is the one the hardware is known to render
 * correctly, so every colour added since goes through the same correction.
 */
uint16_t panelColor(uint8_t r, uint8_t g, uint8_t b) {
  return config::kDisplayRgbOrder ? tft.color565(b, g, r) : tft.color565(r, g, b);
}

void initPalette() {
  // Background, grid and runways were set without the glass correction and
  // showed as brown, olive-green and khaki on the panel; they now look the way
  // they are named.
  radar::kColorBackground = glassColor(theme::current().backdrop_edge);
  radar::kColorGrid = glassColor(theme::current().ring_outer);
  radar::kColorLabel = tft.color565(255, 255, 255);
  radar::kColorCenter = tft.color565(255, 255, 255);
  // GC9A01 BGR panel: swap R/B in color565 so logical red renders red on screen.
  if (config::kDisplayRgbOrder) {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftB, radar::kAircraftG, radar::kAircraftR);
  } else {
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
  }
  radar::kColorTrackVector =
      tft.color565(radar::kTrackR, radar::kTrackG, radar::kTrackB);
  radar::kColorTagType =
      tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorTagAltitude =
      tft.color565(radar::kTagAltR, radar::kTagAltG, radar::kTagAltB);
  radar::kColorRunway = panelColor(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel =
      panelColor(radar::kRunwayLabelR, radar::kRunwayLabelG, radar::kRunwayLabelB);
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  // Longitude degrees shrink toward the poles; scale by cos(latitude) so
  // east-west distance isn't overstated away from the equator.
  const float center_lat_rad =
      static_cast<float>(services::location::lat()) * kDegToRad;
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           cosf(center_lat_rad);
  *dy_km =
      static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

/**
 * Dead-reckon an aircraft's position from its last-fetched fix along its ground
 * track, so it moves smoothly between ADS-B updates. Uses the same flat
 * 1° ≈ 111 km projection as offsetKmFromCenter(), so it round-trips exactly.
 */
void extrapolatedLatLon(const services::adsb::Aircraft& plane,
                        unsigned long base_ms, float* lat, float* lon) {
  *lat = plane.lat;
  *lon = plane.lon;
  if (base_ms == 0 || plane.gs_knots <= 0.0f) {
    return;
  }
  // Elapsed since the fix was measured = time since fetch + the fix's own age,
  // capped: past a couple of missed fetches a straight-line guess is fiction.
  // A sweep published after the frame's clock was read is zero seconds old.
  const unsigned long since_fetch =
      static_cast<long>(s_now_ms - base_ms) > 0 ? s_now_ms - base_ms : 0;
  const unsigned long elapsed_ms =
      std::min(since_fetch + plane.pos_age_ms, config::kDeadReckonMaxMs);
  const float elapsed_h = static_cast<float>(elapsed_ms) / 3600000.0f;
  const float dist_km = plane.gs_knots * 1.852f * elapsed_h;  // knots -> km
  if (dist_km <= 0.0f || std::isnan(plane.track_deg)) {
    return;  // no direction known: leave it where it was seen
  }
  const float rad = plane.track_deg * kDegToRad;  // track: 0 = N, 90 = E
  *lat = plane.lat + (dist_km * cosf(rad)) / kKmPerDeg;
  // A degree of longitude is cos(lat) shorter than one of latitude; without
  // this, east-west motion ran 26% slow at 42 N and jumped at every fetch.
  *lon = plane.lon + (dist_km * sinf(rad)) / (kKmPerDeg * cosf(plane.lat * kDegToRad));
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y, uint16_t color) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx, color);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int nose_len, int* tip_x,
             int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * nose_len));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * nose_len));
}

/** Class drawn for this aircraft, honouring the per-class icon setting. */
uint8_t glyphClass(uint8_t klass) {
  return radar::showClassIcons() ? klass
                                 : static_cast<uint8_t>(util::aircraft::Klass::kJet);
}

/** Sweep theme: one turn of the beam, and how far the afterglow lasts. */
constexpr unsigned long kSweepPeriodMs = 4000;
constexpr float kSweepWedgeDeg = 60.0f;
/** A blip never fades below this share of full brightness. */
constexpr float kSweepMinGlow = 0.22f;

/** Beam bearing now, degrees clockwise from north. */
float sweepBearing() {
  return static_cast<float>(s_now_ms % kSweepPeriodMs) * 360.0f / kSweepPeriodMs;
}

/** 1 just after the beam crossed (x, y), falling to kSweepMinGlow a turn later. */
float sweepGlow(int x, int y) {
  const float bearing =
      atan2f(static_cast<float>(x - radar::kCenterX),
             static_cast<float>(radar::kCenterY - y)) / kDegToRad;
  float age = sweepBearing() - bearing;
  while (age < 0.0f) {
    age += 360.0f;
  }
  while (age >= 360.0f) {
    age -= 360.0f;
  }
  const float glow = 1.0f - age / 360.0f;
  return kSweepMinGlow + (1.0f - kSweepMinGlow) * glow * glow;
}

/** Colour of an aircraft (and its tag altitude) under the current theme. */
Rgb aircraftRgb(const services::adsb::Aircraft& plane, int x, int y) {
  const theme::Style& st = theme::current();
  if (st.sweep) {
    return mix(st.backdrop_centre, st.aircraft, sweepGlow(x, y));
  }
  return st.altitude_ramp ? glyph::altitudeColor(plane.alt_ft) : st.aircraft;
}

void drawAircraftSymbol(int cx, int cy, const services::adsb::Aircraft& plane) {
  const uint16_t fill = glassColor(aircraftRgb(plane, cx, cy));
  if (std::isnan(plane.nose_deg) && plane.klass != static_cast<uint8_t>(util::aircraft::Klass::kRotor)) {
    // Which way it points is unknown: a dot, rather than a nose that lies.
    s_draw->fillCircle(cx, cy, 3, fill);
    s_draw->drawCircle(cx, cy, 4, glassColor(theme::current().glyph_outline));
    return;
  }
  glyph::draw(*s_draw, cx, cy, plane.nose_deg, glyphClass(plane.klass), 1.0f, fill,
              glassColor(theme::current().glyph_outline), plane.engines, plane.rear_engines);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, int nose_len, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, nose_len, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  wideLine(*s_draw, tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth, color);
}

void applyTagStyle() { applyRoleStyle(stepFonts().tag, s_tag_gfx); }

/**
 * Top tag line: the flight number, IATA-style when the airline is known
 * ("SWA1966" -> "WN1966"), otherwise the callsign as received. The airline's
 * name and logo live on the info card, where there is room for them.
 */
void tagTitle(const services::adsb::Aircraft& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }
  const data::airline_brands::AirlineBrand* brand =
      data::airline_brands::brandForCallsign(plane.callsign);
  util::format::flightNumber(plane.callsign, brand != nullptr ? brand->iata : nullptr,
                             out, out_len);
}

/** "5,000 ft", or the feed's own text ("GND") when there is no number. */
void tagAltitude(const services::adsb::Aircraft& plane, char* out, size_t len) {
  if (plane.alt_ft == services::adsb::kAltUnknown) {
    snprintf(out, len, "%s", plane.alt);
    return;
  }
  util::format::thousands(plane.alt_ft, out, len);
  strncat(out, " ft", len - strlen(out) - 1);
}

/** Tag corners must stay this close to the centre to be fully on the glass. */
constexpr int kTagDiscRadius = 117;

bool insideDisc(const card::Rect& r) {
  const int xs[2] = {r.x, r.x + r.w};
  const int ys[2] = {r.y, r.y + r.h};
  for (int x : xs) {
    for (int y : ys) {
      if (distSqFromCenter(x, y) > kTagDiscRadius * kTagDiscRadius) {
        return false;
      }
    }
  }
  return true;
}

/** Text of one tag: flight number, route (else type), altitude — or line 1 only. */
struct TagText {
  char title[12];
  char line2[16];
  services::routes::Shown::Kind route;  // kNone: line2 is the aircraft type
  char alt[16];
  int w;
  int h;
  bool compact;
};

/** Width of the arrow drawn before/after a single airport ("->ORD", "ORD->"). */
constexpr int kTagArrowW = 11;

/** Right-pointing arrow centred on (cx, cy). */
void drawRouteArrow(int cx, int cy, uint16_t color) {
  s_draw->drawFastHLine(cx - 4, cy, 6, color);
  s_draw->fillTriangle(cx + 4, cy, cx + 1, cy - 3, cx + 1, cy + 3, color);
}

int32_t line2Width(const TagText& t) {
  const bool one_end = t.route == services::routes::Shown::Kind::kArriving ||
                       t.route == services::routes::Shown::Kind::kDeparting;
  return s_draw->textWidth(t.line2) + (one_end ? kTagArrowW : 0);
}

void measureTag(const services::adsb::Aircraft& plane, const services::routes::Shown& shown,
                bool compact, TagText* t) {
  initTagLabelMetrics();
  applyTagStyle();
  tagTitle(plane, t->title, sizeof(t->title));
  t->route = shown.kind;
  snprintf(t->line2, sizeof(t->line2), "%s",
           shown.kind == services::routes::Shown::Kind::kNone ? plane.type : shown.codes);
  tagAltitude(plane, t->alt, sizeof(t->alt));
  t->compact = compact;
  const int line_h = s_draw->fontHeight();
  t->w = compact ? s_draw->textWidth(t->title)
                 : std::max({s_draw->textWidth(t->title), line2Width(*t),
                             s_draw->textWidth(t->alt)});
  t->h = line_h * (compact ? 1 : 3);
}

/**
 * First free spot for a w x h block beside a glyph of radius r at (x, y):
 * beside it toward the centre, beside it away from the centre, then above and
 * below on either side. Free means on the glass and clear of every obstacle
 * except the aircraft's own glyph (`skip`).
 */
bool placeTag(int x, int y, int r, int w, int h, const card::Rect* obstacles,
              size_t count, size_t skip, card::Rect* out) {
  const int g = radar::kAircraftLabelGapPx;
  const bool west = x < radar::kCenterX;
  const int inward = west ? x + r + g : x - r - g - w;
  const int outward = west ? x - r - g - w : x + r + g;
  const int in_edge = west ? x + r - 2 : x - r - w + 2;
  const int out_edge = west ? x - r - w + 2 : x + r - 2;
  const card::Rect candidates[] = {
      {inward, y - h / 2, w, h},   {outward, y - h / 2, w, h},
      {in_edge, y - r - h, w, h},  {in_edge, y + r, w, h},
      {out_edge, y - r - h, w, h}, {out_edge, y + r, w, h},
  };
  for (const card::Rect& c : candidates) {
    if (!insideDisc(c)) {
      continue;
    }
    bool clear = true;
    for (size_t i = 0; i < count && clear; ++i) {
      clear = i == skip || !c.overlaps(obstacles[i]);
    }
    if (clear) {
      *out = c;
      return true;
    }
  }
  return false;
}

void drawTagAt(const card::Rect& box, const services::adsb::Aircraft& plane,
               const TagText& t) {
  applyTagStyle();
  s_draw->setTextDatum(textdatum_t::top_left);
  const int line_h = s_draw->fontHeight();
  int ly = box.y;
  // Foreground only: anti-aliased edges blend into whatever is underneath.
  const theme::Style& st = theme::current();
  s_draw->setTextColor(glassColor(st.tag_title));
  s_draw->drawString(t.title, box.x, ly);
  if (t.compact) {
    return;
  }
  ly += line_h;
  if (t.line2[0] != '\0') {
    using Kind = services::routes::Shown::Kind;
    const uint16_t c = glassColor(t.route == Kind::kNone ? st.tag_type : st.tag_route);
    s_draw->setTextColor(c);
    const int mid = ly + s_draw->fontHeight() / 2;
    if (t.route == Kind::kArriving) {  // "->ORD"
      drawRouteArrow(box.x + kTagArrowW / 2 - 1, mid, c);
      s_draw->drawString(t.line2, box.x + kTagArrowW, ly);
    } else if (t.route == Kind::kDeparting) {  // "ORD->"
      s_draw->drawString(t.line2, box.x, ly);
      drawRouteArrow(box.x + s_draw->textWidth(t.line2) + kTagArrowW / 2 + 1, mid, c);
    } else {
      s_draw->drawString(t.line2, box.x, ly);
    }
  }
  ly += line_h;
  if (t.alt[0] != '\0') {
    s_draw->setTextColor(
        glassColor(st.tag_alt_ramp ? glyph::altitudeColor(plane.alt_ft) : st.tag_alt));
    s_draw->drawString(t.alt, box.x, ly);
  }
}

/**
 * Thin grey tail through the aircraft's recent positions, oldest first, ending
 * at the symbol. Drawn before every symbol so tails never cover traffic.
 */
void drawTrail(const services::adsb::Aircraft& plane, int x, int y) {
  ui::trails::Point history[radar::kTrailDrawPoints];
  const size_t count =
      ui::trails::historyFor(plane.hex, history, radar::kTrailDrawPoints);
  if (count == 0) {
    return;
  }

  const theme::Style& st = theme::current();
  int prev_x = 0;
  int prev_y = 0;
  latLonToScreen(history[0].lat, history[0].lon, &prev_x, &prev_y);
  if (st.trail_end_dot &&
      distSqFromCenter(prev_x, prev_y) <= radar::kGridOuterRadius * radar::kGridOuterRadius) {
    s_draw->fillCircle(prev_x, prev_y, 1, glassColor(st.lock));
  }
  for (size_t i = 1; i <= count; ++i) {
    int next_x = x;
    int next_y = y;
    if (i < count) {
      latLonToScreen(history[i].lat, history[i].lon, &next_x, &next_y);
    }
    // A tail entering the ring from outside would otherwise cut across the grid.
    if (distSqFromCenter(prev_x, prev_y) <=
            radar::kGridOuterRadius * radar::kGridOuterRadius &&
        distSqFromCenter(next_x, next_y) <=
            radar::kGridOuterRadius * radar::kGridOuterRadius) {
      const float t = static_cast<float>(i) / static_cast<float>(count);
      s_draw->drawLine(prev_x, prev_y, next_x, next_y,
                       glassColor(mix(st.backdrop_edge, st.trail, t)));
    }
    prev_x = next_x;
    prev_y = next_y;
  }
}

/** Target-lock marker around the aircraft the info card is describing. */
void drawLockBrackets(int cx, int cy, int r) {
  const theme::Style& st = theme::current();
  const uint16_t c = glassColor(st.lock);
  if (st.lock_rings) {
    s_draw->drawCircle(cx, cy, r + 1, c);
    s_draw->drawCircle(cx, cy, r + 4, c);
    return;
  }
  constexpr int kArm = 4;
  for (int sx = -1; sx <= 1; sx += 2) {
    for (int sy = -1; sy <= 1; sy += 2) {
      const int x = cx + sx * r;
      const int y = cy + sy * r;
      s_draw->drawFastHLine(sx < 0 ? x : x - kArm + 1, y, kArm, c);
      s_draw->drawFastVLine(x, sy < 0 ? y : y - kArm + 1, kArm, c);
    }
  }
}

struct AircraftDrawItem {
  size_t index = 0;
  float lat = 0.0f;  // dead-reckoned, as drawn
  float lon = 0.0f;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

/**
 * Which end of the disc the card goes: never over the aircraft it describes,
 * otherwise wherever it hides fewer aircraft. It only moves when that helps, so
 * it does not flip back and forth as traffic drifts across its edge.
 */
bool chooseCardTop(const AircraftDrawItem* items, size_t count, int fx, int fy,
                   bool tall) {
  static bool s_top = false;
  const card::Rect bottom = card::rect(false, tall);
  const card::Rect top = card::rect(true, tall);
  constexpr int kMargin = 8;
  if (bottom.contains(fx, fy, kMargin)) {
    s_top = true;
  } else if (top.contains(fx, fy, kMargin)) {
    s_top = false;
  } else {
    int under_bottom = 0;
    int under_top = 0;
    for (size_t d = 0; d < count; ++d) {
      under_bottom += bottom.contains(items[d].x, items[d].y, kMargin) ? 1 : 0;
      under_top += top.contains(items[d].x, items[d].y, kMargin) ? 1 : 0;
    }
    if (s_top ? under_bottom < under_top : under_top < under_bottom) {
      s_top = !s_top;
    }
  }
  return s_top;
}

/** A tag that found room: whose (index into FramePlan::items), where, what it says. */
struct TagPlan {
  size_t item;
  card::Rect box;
  TagText text;
};

/**
 * Everything a frame shows, decided once before either half is drawn: the
 * snapshot, where each aircraft is, the card and every tag. Both halves paint
 * from it, so whatever straddles the middle comes out whole even when a sweep
 * or a route answer lands between them.
 */
struct FramePlan {
  services::adsb::Aircraft planes[services::adsb::kMaxAircraft];
  size_t n = 0;
  bool no_data = false;
  bool waiting_first = false;
  /** > 0: no data because the feed in use is rationed; its next answer is due this soon. */
  unsigned long next_update_in_ms = 0;
  AircraftDrawItem items[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t dot_count = 0;
  int featured = -1;
  int featured_x = 0;
  int featured_y = 0;
  bool card_here = false;
  bool card_tall = false;
  bool card_top = false;
  services::routes::Shown featured_route;
  TagPlan tags[services::adsb::kMaxAircraft];
  size_t tag_count = 0;
  unsigned long boot_held_ms = 0;
  int alert_radius = 0;
};

FramePlan s_plan;

/** The alert pulse under way, if any (radarDisplayStartAlert). */
struct AlertPulse {
  bool active = false;
  uint16_t color = 0;
  unsigned long start_ms = 0;
  unsigned long total_ms = 0;
};
AlertPulse s_alert;

void planAircraft(FramePlan& p) {
  initLabelMetrics();

  // Snapshot under the adsb lock (the fetch may run on another thread), then
  // read the frame's clock, so no sweep in the snapshot is newer than it.
  unsigned long base_ms = 0;
  p.n = services::adsb::snapshotAircraft(p.planes, services::adsb::kMaxAircraft, &base_ms);
  s_now_ms = millis();
  // No good fetch for this long (rate limit, outage): show an empty sky rather
  // than aircraft that are no longer where they are drawn.
  if (base_ms != 0 && s_now_ms - base_ms > config::kAircraftStaleMs) {
    p.n = 0;
  }
  // Say so when data has stopped (or never came): an empty sky and a broken
  // feed must not look the same.
  p.waiting_first = base_ms == 0;
  p.no_data = base_ms == 0 ? s_now_ms > config::kNoDataNoticeMs
                           : s_now_ms - base_ms > config::kNoDataNoticeMs;
  // In a long adsb.fi outage adsb.lol answers every 4.5 min (its ration), and
  // the aircraft go after 90 s as above: say when the next update is due
  // rather than "retrying...", which would look like a fault.
  const unsigned long paced_until = services::adsb::pacedUntilMs();
  p.next_update_in_ms =
      paced_until != 0 && static_cast<long>(paced_until - s_now_ms) > 0 ? paced_until - s_now_ms
                                                                        : 0;
  // No "set your location" prompt here any more: without a location the radar
  // is not shown at all, setup step 2 is (main.cpp).
  p.boot_held_ms = bootButtonHeldMs();

  p.draw_count = 0;
  p.dot_count = 0;
  for (size_t i = 0; i < p.n; ++i) {
    // Dead-reckoned position for smooth motion between fetches.
    float lat = 0.0f;
    float lon = 0.0f;
    extrapolatedLatLon(p.planes[i], base_ms, &lat, &lon);

    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      AircraftDrawItem& it = p.items[p.draw_count++];
      it.index = i;
      it.lat = lat;
      it.lon = lon;
      latLonToScreen(lat, lon, &it.x, &it.y);
      it.dist_sq = distSqFromCenter(it.x, it.y);
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(lat, lon, &dot_x, &dot_y)) {
      continue;
    }
    BeyondDotDrawItem& dot = p.dots[p.dot_count++];
    dot.index = i;
    dot.x = dot_x;
    dot.y = dot_y;
    dot.dist_sq = distSqFromCenter(dot_x, dot_y);
  }
  sortDrawItemsFarFirst(p.items, p.draw_count);
  sortBeyondDotsFarFirst(p.dots, p.dot_count);

  // The card features one of the nearest aircraft; items are far-first.
  size_t nearest[services::adsb::kMaxAircraft];
  for (size_t d = 0; d < p.draw_count; ++d) {
    nearest[d] = p.items[p.draw_count - 1 - d].index;
  }
  p.featured = card::choose(p.planes, nearest, p.draw_count, s_now_ms);
  p.card_here = p.featured >= 0 && s_draw == &s_frame;
  p.featured_route = services::routes::Shown{};
  for (size_t d = 0; d < p.draw_count; ++d) {
    if (static_cast<int>(p.items[d].index) == p.featured) {
      p.featured_x = p.items[d].x;
      p.featured_y = p.items[d].y;
      if (p.card_here) {
        p.featured_route =
            services::routes::describe(p.planes[p.featured], p.items[d].lat, p.items[d].lon);
      }
    }
  }
  p.card_tall =
      p.card_here && p.featured_route.kind != services::routes::Shown::Kind::kNone;
  p.card_top = p.card_here && chooseCardTop(p.items, p.draw_count, p.featured_x,
                                            p.featured_y, p.card_tall);

  // Tags avoid every glyph, the card and each other. Nearest aircraft are
  // placed first; one that finds no room keeps just its flight number, or
  // goes without.
  static card::Rect obstacles[services::adsb::kMaxAircraft * 2 + 1];
  size_t n_obs = 0;
  for (size_t d = 0; d < p.draw_count; ++d) {
    const int r = glyph::radiusPx(glyphClass(p.planes[p.items[d].index].klass), 1.0f);
    obstacles[n_obs++] = card::Rect{p.items[d].x - r, p.items[d].y - r, 2 * r, 2 * r};
  }
  if (p.card_here) {
    obstacles[n_obs++] = card::rect(p.card_top, p.card_tall);
  }
  p.tag_count = 0;
  for (size_t k = p.draw_count; k-- > 0;) {
    const size_t i = p.items[k].index;
    if (p.card_here && static_cast<int>(i) == p.featured) {
      continue;  // the card says it all
    }
    const int r = glyph::radiusPx(glyphClass(p.planes[i].klass), 1.0f);
    const services::routes::Shown shown =
        services::routes::describe(p.planes[i], p.items[k].lat, p.items[k].lon);
    TagPlan& tag = p.tags[p.tag_count];
    bool placed = false;
    const bool minimal = theme::current().sweep;
    for (bool compact : {false, true}) {
      if (minimal && !compact) {
        continue;
      }
      measureTag(p.planes[i], shown, compact, &tag.text);
      placed = placeTag(p.items[k].x, p.items[k].y, r, tag.text.w, tag.text.h, obstacles,
                        n_obs, k, &tag.box);
      if (placed) {
        break;
      }
    }
    if (placed) {
      tag.item = k;
      obstacles[n_obs++] = tag.box;
      ++p.tag_count;
    }
  }

  // The alert pulse: a ring sweeping out from the centre, restarting every
  // kAlertPulseMs, for as long as the portal setting says.
  p.alert_radius = 0;
  if (s_alert.active) {
    const unsigned long elapsed = s_now_ms - s_alert.start_ms;
    if (elapsed >= s_alert.total_ms) {
      s_alert.active = false;
    } else {
      const float phase =
          static_cast<float>(elapsed % radar::kAlertPulseMs) / radar::kAlertPulseMs;
      p.alert_radius = static_cast<int>(
          phase * (radar::kCenterX - radar::kBeyondRingScreenMarginPx));
    }
  }
}

void paintAircraft(const FramePlan& p) {
  if (p.no_data) {
    applyRoleStyle(kUiFont11, &fonts::FreeSans9pt7b);
    s_draw->setTextDatum(textdatum_t::middle_center);
    s_draw->setTextColor(glassColor(Rgb{255, 190, 90}));
    s_draw->drawString("No flight data", radar::kCenterX, 182);
    s_draw->setTextColor(glassColor(Rgb{170, 175, 190}));
    char next[32] = "";
    if (p.next_update_in_ms > 0) {
      snprintf(next, sizeof(next), "next update in %lu min",
               (p.next_update_in_ms + 59999UL) / 60000UL);
    }
    s_draw->drawString(p.next_update_in_ms > 0 ? next
                       : p.waiting_first       ? "waiting for the first update"
                                               : "retrying...",
                       radar::kCenterX, 197);
    s_draw->setTextDatum(textdatum_t::top_left);
  }

  if (radar::showTrails()) {
    for (size_t d = 0; d < p.draw_count; ++d) {
      drawTrail(p.planes[p.items[d].index], p.items[d].x, p.items[d].y);
    }
  }
  // The card goes under the traffic, not over it: drawn last it hid every
  // aircraft beneath it, and as it rotated and moved between top and bottom
  // every few seconds, aircraft kept vanishing and reappearing.
  if (p.card_here) {
    card::draw(s_frame, p.planes[p.featured], p.card_top, radar::useMiles(),
               p.card_tall ? &p.featured_route : nullptr);
  }

  for (size_t d = 0; d < p.dot_count; ++d) {
    drawBeyondRingDot(p.dots[d].x, p.dots[d].y,
                      glassColor(glyph::altitudeColor(p.planes[p.dots[d].index].alt_ft)));
  }

  for (size_t d = 0; d < p.draw_count; ++d) {
    const services::adsb::Aircraft& plane = p.planes[p.items[d].index];
    const int x = p.items[d].x;
    const int y = p.items[d].y;
    if (radar::showTrackVectors() && !std::isnan(plane.track_deg)) {
      const Rgb dim = mix(theme::current().backdrop_edge, aircraftRgb(plane, x, y),
                          radar::kTrackVectorStrength);
      drawSpeedVector(x, y, plane.nose_deg, plane.track_deg, plane.gs_knots,
                      glyph::radiusPx(glyphClass(plane.klass), 1.0f), glassColor(dim));
    }
    drawAircraftSymbol(x, y, plane);
  }
  if (p.card_here) {
    drawLockBrackets(p.featured_x, p.featured_y,
                     glyph::radiusPx(glyphClass(p.planes[p.featured].klass), 1.0f) + 2);
  }

  for (size_t t = 0; t < p.tag_count; ++t) {
    drawTagAt(p.tags[t].box, p.planes[p.items[p.tags[t].item].index], p.tags[t].text);
  }
}

void applyCardinalStyle() {
  applyRoleStyle(stepFonts().cardinal, s_cardinal_gfx);
}

void applyScaleStyle() { applyRoleStyle(stepFonts().scale, s_scale_gfx); }

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabel(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);
  s_draw->setTextColor(glassColor(theme::current().scale_label));
  s_draw->drawString(text, x, y);
}

/** Range rings: hairlines inside, a brighter outer ring as the scope's edge. */
void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    const bool outer = i == radar::kRingCount;
    const theme::Style& st = theme::current();
    s_draw->drawCircle(cx, cy, r, glassColor(outer ? st.ring_outer : st.ring_inner));
  }
  if (theme::current().double_bezel) {
    s_draw->drawCircle(cx, cy, outer_radius + 3, glassColor(theme::current().ring_outer));
  }
}

/** Crosshair, or a spoke every 360 / spokes degrees. */
void drawCrosshairs(int cx, int cy, int radius) {
  const theme::Style& st = theme::current();
  const uint16_t c = glassColor(st.spoke);
  if (st.spokes <= 4) {
    s_draw->drawFastVLine(cx, cy - radius, 2 * radius + 1, c);
    s_draw->drawFastHLine(cx - radius, cy, 2 * radius + 1, c);
    return;
  }
  const float step = 360.0f / st.spokes;
  for (int i = 0; i < st.spokes; ++i) {
    const float a = i * step * kDegToRad;
    s_draw->drawLine(cx, cy, cx + static_cast<int>(lroundf(sinf(a) * radius)),
                     cy - static_cast<int>(lroundf(cosf(a) * radius)), c);
  }
}

/** Compass ticks inside the outer ring: every 10 degrees, longer every 30. */
void drawBezelTicks(int cx, int cy, int radius) {
  constexpr float kDegToRad = 0.01745329252f;
  for (int deg = 0; deg < 360; deg += 10) {
    if (deg % 90 == 0) {
      continue;  // the crosshair already marks N/E/S/W
    }
    const bool major = deg % 30 == 0;
    const int inner = radius - (major ? radar::kTickMajorPx : radar::kTickMinorPx);
    const float s = sinf(deg * kDegToRad);
    const float c = cosf(deg * kDegToRad);
    const theme::Style& st = theme::current();
    s_draw->drawLine(cx + static_cast<int>(lroundf(s * inner)),
                     cy - static_cast<int>(lroundf(c * inner)),
                     cx + static_cast<int>(lroundf(s * (radius - 1))),
                     cy - static_cast<int>(lroundf(c * (radius - 1))),
                     glassColor(major ? st.tick_major : st.tick_minor));
    if (major && st.double_bezel) {
      // Accent mark between the two bezel rings.
      s_draw->fillCircle(cx + static_cast<int>(lroundf(s * (radius + 2))),
                         cy - static_cast<int>(lroundf(c * (radius + 2))), 1,
                         glassColor(st.accent));
    }
  }
}

/** Scope glass: a faint glow in the middle falling off to near black at the rim. */
void drawBackdrop(int cx, int cy) {
  const theme::Style& st = theme::current();
  s_draw->fillScreen(glassColor(st.backdrop_edge));
  constexpr int kSteps = 12;
  for (int i = 0; i < kSteps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(kSteps - 1);
    const int r = static_cast<int>(lroundf(radar::kCenterX * (1.0f - 0.85f * t)));
    s_draw->fillCircle(cx, cy, r,
                       glassColor(mix(st.backdrop_edge, st.backdrop_centre, t)));
  }
}

void drawCenterDot(int cx, int cy) {
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawRangeLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabel(scale_label, scaleLabelAnchorX(cx, outer_radius), cy);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  initPalette();
  drawBackdrop(cx, cy);
  water::draw(gfx, s_draw == &s_frame ? &s_frame : nullptr);
  if (theme::current().land_dots) {
    water::drawLandDots(gfx, glassColor(theme::current().land_dot));
  }
  drawCrosshairs(cx, cy, grid_r);
  drawRings(cx, cy, grid_r);
  drawBezelTicks(cx, cy, grid_r);
  runway::drawLargeAirportRunways(gfx);
  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawRangeLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

/**
 * Blend a triangle toward `tint` (0..256 = none..full) straight in the sprite
 * buffer (big-endian RGB565), so the beam glows over land, water and rings
 * alike instead of painting them out.
 */
void blendTriangle(LGFX_Sprite& f, float x0, float y0, float x1, float y1, float x2,
                   float y2, uint16_t tint, int amount) {
  if (amount <= 0) {
    return;
  }
  int row0 = 0;
  int row1 = 0;
  clipRows(f, &row0, &row1);
  const float xs[3] = {x0, x1, x2};
  const float ys[3] = {y0, y1, y2};
  const int y_lo = std::max(row0, static_cast<int>(ceilf(std::min({y0, y1, y2}) - 0.5f)));
  const int y_hi =
      std::min(row1 - 1, static_cast<int>(floorf(std::max({y0, y1, y2}) - 0.5f)));
  for (int y = y_lo; y <= y_hi; ++y) {
    const float yc = static_cast<float>(y) + 0.5f;
    float left = 1e9f;
    float right = -1e9f;
    for (int e = 0; e < 3; ++e) {
      const float ax = xs[e], ay = ys[e], bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
      if ((yc < ay) == (yc < by)) {
        continue;
      }
      const float x = ax + (yc - ay) * (bx - ax) / (by - ay);
      left = std::min(left, x);
      right = std::max(right, x);
    }
    blendSpanClipped(f, y, static_cast<int>(ceilf(left - 0.5f)),
                     static_cast<int>(ceilf(right - 0.5f)), tint, amount);
  }
}

/** Sweep theme: the beam and its fading wedge of afterglow behind it. */
void drawSweepBeam(LGFX_Sprite& f) {
  const theme::Style& st = theme::current();
  const float cx = static_cast<float>(radar::kCenterX);
  const float cy = static_cast<float>(radar::kCenterY);
  const float r = static_cast<float>(radar::kGridOuterRadius);
  const float bearing = sweepBearing();
  const uint16_t tint = glassColor(st.beam);
  constexpr int kSlices = 20;
  constexpr int kMaxAmount = 110;
  for (int k = 0; k < kSlices; ++k) {
    const float a0 = (bearing - (k + 1) * kSweepWedgeDeg / kSlices) * kDegToRad;
    const float a1 = (bearing - k * kSweepWedgeDeg / kSlices) * kDegToRad;
    const float fade = 1.0f - static_cast<float>(k) / kSlices;
    blendTriangle(f, cx, cy, cx + sinf(a0) * r, cy - cosf(a0) * r, cx + sinf(a1) * r,
                  cy - cosf(a1) * r, tint, static_cast<int>(kMaxAmount * fade * fade));
  }
  const float a = bearing * kDegToRad;
  wideLine(f, radar::kCenterX, radar::kCenterY,
           radar::kCenterX + static_cast<int>(lroundf(sinf(a) * r)),
           radar::kCenterY - static_cast<int>(lroundf(cosf(a) * r)), 0.8f, tint);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_strip.setColorDepth(16);
  if (!s_strip.createSprite(radar::kSize, kStripRows)) {
    Serial.println("radar: frame buffer alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

/**
 * Point s_frame at half `k` of the screen: its buffer starts where row 0
 * would be if the strip were part of a whole frame, and the clip rectangle
 * keeps every drawing call inside the rows the strip really holds.
 */
void selectStrip(int k) {
  const uintptr_t strip = reinterpret_cast<uintptr_t>(s_strip.getBuffer());
  const uintptr_t row_bytes = static_cast<uintptr_t>(radar::kSize) * 2u;
  const uintptr_t origin = strip - static_cast<uintptr_t>(k * kStripRows) * row_bytes;
  s_frame.setBuffer(reinterpret_cast<void*>(origin), radar::kSize, radar::kSize, 16);
  s_frame.setClipRect(0, k * kStripRows, radar::kSize, kStripRows);
}

/**
 * While BOOT is held, say what will happen and when, so a press while handling
 * the radar is noticed and let go long before it clears the Wi-Fi.
 */
void drawResetCountdown(const FramePlan& p) {
  const unsigned long held = p.boot_held_ms;
  if (held < config::kBootResetWarnMs) {
    return;
  }
  const unsigned long left_ms =
      held >= config::kBootResetHoldMs ? 0 : config::kBootResetHoldMs - held;
  const int x = 40, y = 92, w = 160, h = 56;
  s_draw->fillRoundRect(x, y, w, h, 10, glassColor(Rgb{40, 24, 0}));
  s_draw->drawRoundRect(x, y, w, h, 10, glassColor(Rgb{255, 190, 60}));
  applyRoleStyle(kUiFont13, &fonts::FreeSansBold9pt7b);
  s_draw->setTextDatum(textdatum_t::top_center);
  s_draw->setTextColor(glassColor(Rgb{255, 255, 255}));
  s_draw->drawString("Let go now: radar info", x + w / 2, y + 7);
  s_draw->drawString("Keep holding: new Wi-Fi", x + w / 2, y + 22);
  char secs[24];
  snprintf(secs, sizeof(secs), "in %lu s",
           static_cast<unsigned long>((left_ms + 999) / 1000));
  applyRoleStyle(kUiFont11, &fonts::FreeSans9pt7b);
  s_draw->setTextColor(glassColor(Rgb{255, 200, 90}));
  s_draw->drawString(secs, x + w / 2, y + 38);
  s_draw->setTextDatum(textdatum_t::top_left);
}

/** One ring of the alert pulse. */
void drawAlertRing(const FramePlan& p) {
  if (p.alert_radius <= 1) {
    return;
  }
  const int thickness =
      std::max(1, static_cast<int>(radar::kAlertRingHalfWidth * 2.0f));
  for (int i = 0; i < thickness && p.alert_radius - i > 0; ++i) {
    s_draw->drawCircle(radar::kCenterX, radar::kCenterY, p.alert_radius - i,
                       s_alert.color);
  }
}

/**
 * Night: the backlight is wired full on (no dimming pin), so the picture
 * itself is dimmed, to kNightKeep/256 of its brightness, from dusk to dawn
 * at the radar's location. Decided once a minute.
 */
constexpr int kNightKeep = 90;
unsigned long s_night_checked_ms = 0;
bool s_night = false;

bool nightNow() {
  if (s_night_checked_ms != 0 && millis() - s_night_checked_ms < 60000UL) {
    return s_night;
  }
  const unsigned long now = millis();
  s_night_checked_ms = now != 0 ? now : 1;  // 0 means "never"; not `| 1`, a future time
  uint32_t utc = 0;
  s_night = radar::dimAtNight() && services::location::isSet() &&
            services::adsb::utcNow(&utc) &&
            util::solar::isNight(services::location::lat(), services::location::lon(), utc);
  return s_night;
}

/** Scale a finished half (big-endian RGB565) toward black. */
void dimStrip(uint8_t* p, size_t pixels, int keep) {
  for (size_t i = 0; i < pixels; ++i, p += 2) {
    const uint16_t v = static_cast<uint16_t>((p[0] << 8) | p[1]);
    const int r5 = ((v >> 11) * keep) >> 8;
    const int g6 = (((v >> 5) & 0x3F) * keep) >> 8;
    const int b5 = ((v & 0x1F) * keep) >> 8;
    const uint16_t o = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
    p[0] = static_cast<uint8_t>(o >> 8);
    p[1] = static_cast<uint8_t>(o);
  }
}

/**
 * Plan the frame once, then draw it half by half: each half is composited off
 * screen and sent in one push, so labels never show an erase/redraw gap.
 * `sink`, when given, also receives each finished half (for /screen.bmp).
 */
void renderFrame(FrameRowsFn sink, void* ctx) {
  {
    const DrawScope scope(s_frame);
    planAircraft(s_plan);
  }
  water::prepare();  // once, so both halves see the same mask
  const bool night = nightNow();
  for (int k = 0; k < kStrips; ++k) {
    selectStrip(k);
    drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
    if (theme::current().sweep) {
      drawSweepBeam(s_frame);
    }
    {
      const DrawScope scope(s_frame);
      paintAircraft(s_plan);
      drawResetCountdown(s_plan);
      drawAlertRing(s_plan);
    }
    // Everything above must keep to the clip rectangle (see selectStrip); a
    // call that resets it writes over the heap (as drawWideLine did, hence
    // ui::wideLine). Say so, once, rather than corrupt memory silently.
    static bool s_clip_reported = false;
    int32_t clip_x = 0, clip_y = 0, clip_w = 0, clip_h = 0;
    s_frame.getClipRect(&clip_x, &clip_y, &clip_w, &clip_h);
    if (!s_clip_reported && (clip_y != k * kStripRows || clip_h != kStripRows)) {
      s_clip_reported = true;
      Serial.printf("radar: BUG - a draw call reset the clip to y=%d h=%d\n",
                    static_cast<int>(clip_y), static_cast<int>(clip_h));
    }
    if (night) {
      dimStrip(static_cast<uint8_t*>(s_strip.getBuffer()),
               static_cast<size_t>(radar::kSize) * kStripRows, kNightKeep);
    }
    if (sink != nullptr) {
      sink(static_cast<const uint8_t*>(s_strip.getBuffer()), k * kStripRows, kStripRows,
           radar::kSize, ctx);
    } else {
      s_strip.pushSprite(0, k * kStripRows);
    }
  }
  tft.setTextDatum(textdatum_t::top_left);
}

}  // namespace

namespace {

uint16_t alertColor(alert::Kind kind) {
  switch (kind) {
    case alert::Kind::kEmergency:
      return panelColor(radar::kAlertEmergencyR, radar::kAlertEmergencyG,
                        radar::kAlertEmergencyB);
    case alert::Kind::kMilitary:
      return panelColor(radar::kAlertMilitaryR, radar::kAlertMilitaryG,
                        radar::kAlertMilitaryB);
    default:
      return panelColor(radar::kAlertNotableR, radar::kAlertNotableG,
                        radar::kAlertNotableB);
  }
}

}  // namespace

void radarDisplayStartAlert() {
  const alert::Kind kind = alert::consume();
  const uint8_t seconds = radar::alertSeconds();
  // Without the frame buffer every redraw goes straight to the panel, so the
  // pulse's fast redraws would only flicker; the alert is dropped, as before.
  if (kind == alert::Kind::kNone || seconds == 0 || !s_frame_ready) {
    return;
  }
  s_alert.active = true;
  s_alert.color = alertColor(kind);
  s_alert.start_ms = millis();
  s_alert.total_ms = static_cast<unsigned long>(seconds) * 1000UL;
}

void radarDisplayReleaseBuffer() {
  s_strip.deleteSprite();
  s_frame_ready = false;
}

void radarDisplayInvalidateStyle() {
  s_night_checked_ms = 0;  // the night setting may have changed with the rest
  s_label_metrics_ready = false;
  s_tag_label_metrics_ready = false;
}

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame(nullptr, nullptr);
    return;
  }

  // Fallback when the frame buffer can't be allocated: draw straight to the
  // panel (it flickers, but it shows the sky).
  const DrawScope scope(tft);
  planAircraft(s_plan);
  water::prepare();
  drawStaticGrid(tft);
  paintAircraft(s_plan);
  tft.setTextDatum(textdatum_t::top_left);
}

void radarDisplayRefreshAircraft() { radarDisplayDraw(); }

unsigned long radarDisplayRedrawIntervalMs() {
  if (s_alert.active) {
    return radar::kAlertFrameMs;
  }
  // The beam needs ~8 fps to turn smoothly; everything else is fine at 4.
  return theme::current().sweep ? 125 : config::kRadarRedrawIntervalMs;
}

bool radarDisplayCapture(FrameRowsFn sink, void* ctx) {
  if (!s_frame_ready || sink == nullptr) {
    return false;
  }
  initPalette();
  initLabelMetrics();
  renderFrame(sink, ctx);
  return true;
}

bool radarDisplayReserve() { return ensureFrameSprite(); }

}  // namespace ui
