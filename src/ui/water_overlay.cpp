#include "ui/water_overlay.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "data/water.h"
#include "services/radar_location.h"
#include "ui/blend.h"
#include "ui/wide_line.h"
#include "ui/panel_color.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/theme.h"

namespace ui::water {

namespace {

constexpr int kSize = radar::kSize;
constexpr int kWordsPerRow = (kSize + 31) / 32;
constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 0.01745329252f;
/** Rows rasterised per pass over the edge list, and crossings kept per row. */
constexpr int kBandRows = 16;
constexpr int kMaxCrossings = 96;

/** Bit x of row y: word x / 32, bit x % 32. */
uint32_t s_mask[kSize][kWordsPerRow];
bool s_have_water = false;

// What the mask was built for; any change triggers a rebuild.
float s_built_outer_km = -1.0f;
double s_built_lat = 1000.0;
double s_built_lon = 1000.0;

struct Projection {
  double lat;
  double lon;
  float px_per_km;
  float cos_lat;
  float y(int32_t lat_e5) const {
    return radar::kCenterY -
           static_cast<float>(lat_e5 * 1e-5 - lat) * kKmPerDeg * px_per_km;
  }
  float x(int32_t lon_e5) const {
    return radar::kCenterX +
           static_cast<float>(lon_e5 * 1e-5 - lon) * kKmPerDeg * cos_lat * px_per_km;
  }
};

/** The projection the current mask was built with; the shore uses it too. */
Projection s_proj{0.0, 0.0, 1.0f, 1.0f};

/** Whether the generated square holds everything the screen can show. */
bool dataCovers(const Projection& p) {
  const float view_km = (radar::kSize / 2) / p.px_per_km;
  const float lat_room_km =
      data::water::kHalfSpanLatE5 * 1e-5f * kKmPerDeg -
      std::fabs(static_cast<float>(p.lat - data::water::kCenterLatE5 * 1e-5)) * kKmPerDeg;
  const float lon_room_km =
      (data::water::kHalfSpanLonE5 * 1e-5f -
       std::fabs(static_cast<float>(p.lon - data::water::kCenterLonE5 * 1e-5))) *
      kKmPerDeg * p.cos_lat;
  return data::water::kWaterPolygonCount > 0 && lat_room_km >= view_km &&
         lon_room_km >= view_km;
}

void setRun(uint32_t* row, int x0, int x1) {
  x0 = std::max(0, x0);
  x1 = std::min(kSize, x1);
  for (int x = x0; x < x1; ++x) {
    row[x >> 5] |= 1u << (x & 31);
  }
}

/** Even-odd scanline fill of every ring into s_mask, one band at a time. */
bool rasterise(const Projection& p) {
  memset(s_mask, 0, sizeof(s_mask));
  // Only needed while rasterising, so borrowed from the heap (~6 KB).
  auto* crossings = static_cast<float(*)[kMaxCrossings]>(
      malloc(sizeof(float) * kBandRows * kMaxCrossings));
  if (crossings == nullptr) {
    Serial.println("water: no memory to build the mask");
    return false;
  }
  uint8_t counts[kBandRows];
  bool overflowed = false;
  // Latitude (e5) of screen row y, to reject edges before projecting them.
  const auto rowLatE5 = [&](float y) {
    return static_cast<int32_t>(lroundf(
        static_cast<float>((p.lat + (radar::kCenterY - y) / (kKmPerDeg * p.px_per_km)) * 1e5)));
  };
  for (int band = 0; band < kSize; band += kBandRows) {
    const int rows = std::min(kBandRows, kSize - band);
    memset(counts, 0, sizeof(counts));
    const int32_t lat_top = rowLatE5(static_cast<float>(band) - 1.0f);
    const int32_t lat_bottom = rowLatE5(static_cast<float>(band + rows) + 1.0f);
    for (size_t r = 0; r < data::water::kWaterPolygonCount; ++r) {
      const data::water::Path& ring = data::water::kWaterPolygons[r];
      for (size_t i = 0; i < ring.count; ++i) {
        const data::water::Vertex& a = ring.points[i];
        const data::water::Vertex& b = ring.points[(i + 1) % ring.count];
        if ((a.lat_e5 > lat_top && b.lat_e5 > lat_top) ||
            (a.lat_e5 < lat_bottom && b.lat_e5 < lat_bottom)) {
          continue;  // edge entirely above or below this band
        }
        const float ay = p.y(a.lat_e5);
        const float by = p.y(b.lat_e5);
        if (ay == by) {
          continue;
        }
        const float ax = p.x(a.lon_e5);
        const float bx = p.x(b.lon_e5);
        const float y_lo = std::min(ay, by);
        const float y_hi = std::max(ay, by);
        const int first = std::max(band, static_cast<int>(ceilf(y_lo - 0.5f)));
        const int last = std::min(band + rows - 1, static_cast<int>(ceilf(y_hi - 0.5f)) - 1);
        for (int y = first; y <= last; ++y) {
          const float yc = static_cast<float>(y) + 0.5f;
          const int slot = y - band;
          if (counts[slot] < kMaxCrossings) {
            crossings[slot][counts[slot]++] = ax + (yc - ay) * (bx - ax) / (by - ay);
          } else {
            counts[slot] = kMaxCrossings + 1;  // parity lost: leave the row empty
            overflowed = true;
          }
        }
      }
    }
    for (int slot = 0; slot < rows; ++slot) {
      float* xs = crossings[slot];
      const int n = counts[slot];
      if (n > kMaxCrossings) {
        continue;
      }
      std::sort(xs, xs + n);
      for (int k = 0; k + 1 < n; k += 2) {
        setRun(s_mask[band + slot], static_cast<int>(ceilf(xs[k] - 0.5f)),
               static_cast<int>(ceilf(xs[k + 1] - 0.5f)));
      }
    }
  }
  free(crossings);
  // Bits past the last column count as water, so the right edge of the screen
  // never reads as a shore (see draw()).
  constexpr uint32_t kPad = ~((1u << (kSize & 31)) - 1u);
  static_assert(kSize % 32 != 0, "pad mask assumes a partial last word");
  for (int y = 0; y < kSize; ++y) {
    s_mask[y][kWordsPerRow - 1] |= kPad;
  }
  if (overflowed) {
    Serial.println("water: some rows had too many crossings and were skipped");
  }
  return true;
}

}  // namespace

void prepare() {
  const float outer_km = radar::rangeCurrent().outer_km;
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  if (outer_km == s_built_outer_km && lat == s_built_lat && lon == s_built_lon) {
    return;
  }
  const Projection p{lat, lon, static_cast<float>(radar::kGridOuterRadius) / outer_km,
                     cosf(static_cast<float>(lat) * kDegToRad)};
  s_proj = p;
  s_have_water = dataCovers(p);
  if (s_have_water) {
    const unsigned long t0 = millis();
    s_have_water = rasterise(p);
    if (!s_have_water) {
      return;  // retried next frame; keys stay unset
    }
    Serial.printf("water: mask for %.0f km built in %lu ms\n", outer_km, millis() - t0);
  }
  // Recorded only after a successful build, so a failed one is retried.
  s_built_outer_km = outer_km;
  s_built_lat = lat;
  s_built_lon = lon;
}

namespace {

/** First x >= from whose bit equals `set`, or kSize. */
int nextBit(const uint32_t* row, int from, bool set) {
  while (from < kSize) {
    const int w = from >> 5;
    uint32_t bits = set ? row[w] : ~row[w];
    bits &= ~0u << (from & 31);
    if (bits != 0) {
      return std::min(kSize, (w << 5) + __builtin_ctz(bits));
    }
    from = (w + 1) << 5;
  }
  return kSize;
}

}  // namespace

void draw(lgfx::LovyanGFX& gfx, LGFX_Sprite* frame) {
  if (!s_have_water) {
    return;
  }
  const theme::Style& st = theme::current();
  const uint16_t water = glassColor(st.water);
  // Only the rows of the half being drawn exist in the frame (radar_display).
  int row0 = 0;
  int row1 = kSize;
  if (frame != nullptr) {
    clipRows(*frame, &row0, &row1);
  }

  // Fill: tint the water rather than paint it, so the scope's vignette carries
  // on across the lake instead of stopping at a flat block of colour.
  for (int y = std::max(0, row0); y < std::min(kSize, row1); ++y) {
    const uint32_t* row = s_mask[y];
    for (int x = nextBit(row, 0, true); x < kSize;) {
      const int end = nextBit(row, x, false);
      if (frame != nullptr) {
        blendSpanClipped(*frame, y, x, end, water, st.water_alpha);
      } else {
        gfx.drawFastHLine(x, y, end - x, water);
      }
      x = nextBit(row, end, true);
    }
  }

  // Shore: the polygon's own edges as anti-aliased lines. The mask's pixel
  // edge was a staircase; these follow the coast.
  const uint16_t shore = glassColor(st.shore);
  constexpr float kMargin = 4.0f;
  const auto off = [&](float v) { return v < -kMargin || v > kSize + kMargin; };
  for (size_t r = 0; r < data::water::kWaterPolygonCount; ++r) {
    const data::water::Path& ring = data::water::kWaterPolygons[r];
    if (ring.count < 2) {
      continue;
    }
    float px = s_proj.x(ring.points[ring.count - 1].lon_e5);
    float py = s_proj.y(ring.points[ring.count - 1].lat_e5);
    for (size_t i = 0; i < ring.count; ++i) {
      const float x = s_proj.x(ring.points[i].lon_e5);
      const float y = s_proj.y(ring.points[i].lat_e5);
      // Skip edges wholly off one side of the screen (this also drops the
      // edges of the generated square, which lie far outside every range).
      const bool hidden = (px < -kMargin && x < -kMargin) ||
                          (px > kSize + kMargin && x > kSize + kMargin) ||
                          (py < -kMargin && y < -kMargin) ||
                          (py > kSize + kMargin && y > kSize + kMargin);
      if (!hidden && !(off(px) && off(x) && off(py) && off(y))) {
        wideLine(gfx, lroundf(px), lroundf(py), lroundf(x), lroundf(y), 0.6f, shore);
      }
      px = x;
      py = y;
    }
  }
}

void drawLandDots(lgfx::LovyanGFX& gfx, uint16_t color) {
  if (!s_have_water) {
    return;  // without water data there is no telling land from sea
  }
  constexpr int kPitch = 3;
  constexpr int kRimSq = (kSize / 2 - 2) * (kSize / 2 - 2);
  for (int y = 1; y < kSize; y += kPitch) {
    // Every other row shifted half a pitch: a halftone, not a grid.
    const int x0 = ((y / kPitch) & 1) ? 2 : 0;
    const int dy = y - kSize / 2;
    for (int x = x0; x < kSize; x += kPitch) {
      const int dx = x - kSize / 2;
      if (dx * dx + dy * dy > kRimSq || ((s_mask[y][x >> 5] >> (x & 31)) & 1u)) {
        continue;
      }
      gfx.drawPixel(x, y, color);
    }
  }
}

}  // namespace ui::water
