#include "ui/runway_overlay.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <cmath>
#include <cstdlib>

#include "data/large_airports.h"
#include "hardware/display_font.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/wide_line.h"

namespace ui::runway {
namespace {

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;
constexpr size_t kMaxAirportLabels = 32;

bool s_in_range[data::large_airports::kAirportCount];
bool s_label_pending[data::large_airports::kAirportCount];

/** Embedded font for the ICAO tags at the active text size step. */
size_t runwayLabelFont() {
  constexpr size_t kStepCount =
      sizeof(radar::kFontStepFonts) / sizeof(radar::kFontStepFonts[0]);
  const uint8_t step = radar::fontStep();
  return radar::kFontStepFonts[step < kStepCount ? step : 0].runway;
}

void applyRunwayLabelStyle(lgfx::LGFXBase& gfx) {
  if (displayFontIsSmooth()) {
    displayFontEnsureLoaded(gfx, runwayLabelFont());
  } else {
    displayFontSetBitmap(gfx, &fonts::FreeSansBold12pt7b);
  }
}

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

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

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km =
      static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
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

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (distSqFromCenter(x0, y0) <= r_sq || distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int fx = x0 - cx;
  const int fy = y0 - cy;
  const int a = dx * dx + dy * dy;
  if (a == 0) {
    return false;
  }
  const int b = 2 * (fx * dx + fy * dy);
  const int c = fx * fx + fy * fy - r_sq;
  int disc = b * b - 4 * a * c;
  if (disc < 0) {
    return false;
  }
  disc = static_cast<int>(sqrtf(static_cast<float>(disc)));
  const float inv2a = 1.0f / (2.0f * static_cast<float>(a));
  const float t0 = (-static_cast<float>(b) - disc) * inv2a;
  const float t1 = (-static_cast<float>(b) + disc) * inv2a;
  return (t0 >= 0.0f && t0 <= 1.0f) || (t1 >= 0.0f && t1 <= 1.0f);
}

void drawBoldRunwayLabel(lgfx::LGFXBase& gfx, const char* ident, int mx, int my) {
  gfx.setTextDatum(textdatum_t::bottom_center);
  // Foreground only, so the label blends into the scope's gradient.
  gfx.setTextColor(radar::kColorRunwayLabel);
  gfx.drawString(ident, mx - 1, my);
  gfx.drawString(ident, mx + 1, my);
  gfx.drawString(ident, mx, my);
}

bool drawRunwayLine(lgfx::LGFXBase& gfx, const data::large_airports::Runway& rw) {
  const float le_lat = e7ToDeg(rw.le_lat_e7);
  const float le_lon = e7ToDeg(rw.le_lon_e7);
  const float he_lat = e7ToDeg(rw.he_lat_e7);
  const float he_lon = e7ToDeg(rw.he_lon_e7);

  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  latLonToScreen(le_lat, le_lon, &x0, &y0);
  latLonToScreen(he_lat, he_lon, &x1, &y1);

  if (!segmentIntersectsDisc(x0, y0, x1, y1)) {
    return false;
  }

  clipPointToOuterRing(x0, y0, &x1, &y1);
  clipPointToOuterRing(x1, y1, &x0, &y0);

  wideLine(gfx, x0, y0, x1, y1, radar::kRunwayLineHalfWidth, radar::kColorRunway);
  return true;
}

void offsetLabelFromCenter(int ax, int ay, int* lx, int* ly) {
  const int dx = ax - radar::kCenterX;
  const int dy = ay - radar::kCenterY;
  const float len = sqrtf(static_cast<float>(dx * dx + dy * dy));
  const int gap = radar::kRunwayLabelGapPx;
  if (len < 1.0f) {
    *lx = ax;
    *ly = ay - gap;
    return;
  }
  *lx = ax + static_cast<int>(lroundf(dx / len * static_cast<float>(gap)));
  *ly = ay + static_cast<int>(lroundf(dy / len * static_cast<float>(gap)));
}

void clipPointOntoOuterRing(int* x, int* y) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int dx = *x - cx;
  const int dy = *y - cy;
  const int d_sq = dx * dx + dy * dy;
  const int r_sq = r * r;
  if (d_sq <= r_sq || d_sq == 0) {
    return;
  }
  const float scale = static_cast<float>(r) / sqrtf(static_cast<float>(d_sq));
  *x = cx + static_cast<int>(lroundf(static_cast<float>(dx) * scale));
  *y = cy + static_cast<int>(lroundf(static_cast<float>(dy) * scale));
}

void drawAirportLabel(lgfx::LGFXBase& gfx,
                      const data::large_airports::Airport& ap) {
  int ax = 0;
  int ay = 0;
  latLonToScreen(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &ax, &ay);
  clipPointOntoOuterRing(&ax, &ay);

  int lx = 0;
  int ly = 0;
  offsetLabelFromCenter(ax, ay, &lx, &ly);
  drawBoldRunwayLabel(gfx, ap.ident, lx, ly);
}

}  // namespace

namespace {

/**
 * Runways near the radar centre, found once per range / centre change. Testing
 * all of kRunwayCount every frame cost ~50 ms (a soft-float cosf per airport)
 * of a 4 Hz redraw; only a handful are ever in range.
 */
constexpr size_t kMaxVisibleRunways = 64;
uint16_t s_visible[kMaxVisibleRunways];
size_t s_visible_count = 0;
float s_cached_radius_km = -1.0f;
double s_cached_lat = 1000.0;
double s_cached_lon = 1000.0;

void refreshVisibleRunways() {
  const float radius_km = radar::fetchRadiusKm();
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  if (radius_km == s_cached_radius_km && lat == s_cached_lat && lon == s_cached_lon) {
    return;
  }
  s_cached_radius_km = radius_km;
  s_cached_lat = lat;
  s_cached_lon = lon;
  s_visible_count = 0;

  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    s_in_range[i] = false;
    s_label_pending[i] = false;  // reused here as "distance already tested"
  }
  for (size_t i = 0; i < data::large_airports::kRunwayCount; ++i) {
    const uint16_t ap_idx = data::large_airports::kRunways[i].airport_idx;
    if (!s_label_pending[ap_idx]) {
      s_label_pending[ap_idx] = true;
      const auto& ap = data::large_airports::kAirports[ap_idx];
      float dx_km = 0.0f;
      float dy_km = 0.0f;
      float dist_km = 0.0f;
      offsetKmFromCenter(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &dx_km, &dy_km,
                         &dist_km);
      s_in_range[ap_idx] = (dist_km <= radius_km);
    }
    if (s_in_range[ap_idx] && s_visible_count < kMaxVisibleRunways) {
      s_visible[s_visible_count++] = static_cast<uint16_t>(i);
    }
  }
}

}  // namespace

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) {
    return;
  }
  refreshVisibleRunways();

  uint16_t label_airports[kMaxAirportLabels];
  size_t label_count = 0;
  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    s_label_pending[i] = false;
  }

  for (size_t v = 0; v < s_visible_count; ++v) {
    const auto& rw = data::large_airports::kRunways[s_visible[v]];
    const uint16_t ap_idx = rw.airport_idx;
    if (!drawRunwayLine(gfx, rw)) {
      continue;
    }
    if (!s_label_pending[ap_idx] && label_count < kMaxAirportLabels) {
      s_label_pending[ap_idx] = true;
      label_airports[label_count++] = ap_idx;
    }
  }

  if (label_count == 0) {
    return;
  }

  applyRunwayLabelStyle(gfx);
  for (size_t i = 0; i < label_count; ++i) {
    drawAirportLabel(gfx, data::large_airports::kAirports[label_airports[i]]);
  }
}

}  // namespace ui::runway
