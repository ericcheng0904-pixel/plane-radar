#include "ui/aircraft_glyph.h"

#include <cmath>
#include <cstddef>

#include "ui/wide_line.h"
#include "util/aircraft_class.h"

namespace ui::glyph {

namespace {

struct Tri {
  float x0, y0, x1, y1, x2, y2;
};

// Templates: nose up (-y), units are map pixels at scale 1.0, centred on the
// aircraft's reference point. Wings and tailplane are listed for the right
// side only and mirrored when drawn.

// Swept-wing jet, ~17 px nose to tail.
constexpr Tri kJetBody[] = {
    {0.0f, -8.5f, -1.3f, -6.0f, 1.3f, -6.0f},   // nose
    {-1.3f, -6.0f, 1.3f, -6.0f, 1.3f, 7.0f},    // fuselage
    {-1.3f, -6.0f, 1.3f, 7.0f, -1.3f, 7.0f},
    {-1.3f, 7.0f, 1.3f, 7.0f, 0.0f, 8.5f},      // tail cone
};
constexpr Tri kJetSide[] = {
    {1.0f, -2.8f, 8.2f, 3.0f, 8.2f, 4.4f},      // wing leading half
    {1.0f, -2.8f, 8.2f, 4.4f, 1.0f, 1.6f},      // wing trailing half
    {1.0f, 4.8f, 4.2f, 7.4f, 4.2f, 8.3f},       // tailplane
    {1.0f, 4.8f, 4.2f, 8.3f, 1.0f, 7.0f},
};

// Straight-wing light aircraft, ~12 px.
constexpr Tri kLightBody[] = {
    {0.0f, -6.0f, -1.1f, -4.5f, 1.1f, -4.5f},
    {-1.1f, -4.5f, 1.1f, -4.5f, 0.8f, 5.0f},
    {-1.1f, -4.5f, 0.8f, 5.0f, -0.8f, 5.0f},
};
constexpr Tri kLightSide[] = {
    {0.8f, -2.4f, 6.8f, -2.2f, 6.8f, -0.4f},
    {0.8f, -2.4f, 6.8f, -0.4f, 0.8f, -0.2f},
    {0.6f, 3.4f, 3.0f, 3.6f, 3.0f, 5.0f},
    {0.6f, 3.4f, 3.0f, 5.0f, 0.6f, 5.0f},
};

// Heavies reuse the jet at this factor.
constexpr float kHeavyScale = 1.35f;

/**
 * The jet family, built from the classic jet: how big against it, how wide
 * the fuselage (kJetBody is 1.3 either side), and whether engine pods hang
 * under the wings or sit on the rear fuselage. Size carries most of the
 * difference at 15-25 px; the pods tell two engines from four.
 */
struct JetSpec {
  float scale;
  float body;
};
constexpr JetSpec kBizJetSpec{0.72f, 1.0f};
constexpr JetSpec kRegionalSpec{0.85f, 1.1f};
constexpr JetSpec kNarrowSpec{1.0f, 1.3f};
constexpr JetSpec kWideSpec{1.3f, 1.9f};
constexpr JetSpec kWideQuadSpec{1.4f, 1.9f};
// Turboprop: straight wing, propellers ahead of it.
constexpr float kPropScale = 0.9f;

constexpr float kRotorDiscR = 3.2f;
constexpr float kRotorBladeLen = 7.5f;

constexpr float kDegToRad = 0.01745329252f;

struct Pose {
  float cx, cy, s, c, scale;
  int ox, oy;  // outline offset
  int px(float x, float y) const {
    return static_cast<int>(lroundf(cx + (x * c - y * s) * scale)) + ox;
  }
  int py(float x, float y) const {
    return static_cast<int>(lroundf(cy + (x * s + y * c) * scale)) + oy;
  }
};

void fillTri(lgfx::LovyanGFX& gfx, const Pose& p, const Tri& t, float mirror,
             uint16_t color) {
  gfx.fillTriangle(p.px(t.x0 * mirror, t.y0), p.py(t.x0 * mirror, t.y0),
                   p.px(t.x1 * mirror, t.y1), p.py(t.x1 * mirror, t.y1),
                   p.px(t.x2 * mirror, t.y2), p.py(t.x2 * mirror, t.y2), color);
}

template <size_t NB, size_t NS>
void drawTemplate(lgfx::LovyanGFX& gfx, const Pose& p, const Tri (&body)[NB],
                  const Tri (&side)[NS], uint16_t color) {
  for (const Tri& t : body) {
    fillTri(gfx, p, t, 1.0f, color);
  }
  for (const Tri& t : side) {
    fillTri(gfx, p, t, 1.0f, color);
    fillTri(gfx, p, t, -1.0f, color);
  }
}

/** An axis-aligned box in template units, as two triangles. */
void fillBox(lgfx::LovyanGFX& gfx, const Pose& p, float x0, float y0, float x1, float y1,
             uint16_t color) {
  fillTri(gfx, p, Tri{x0, y0, x1, y0, x1, y1}, 1.0f, color);
  fillTri(gfx, p, Tri{x0, y0, x1, y1, x0, y1}, 1.0f, color);
}

/** The same box and its mirror image across the fuselage. */
void fillBoxPair(lgfx::LovyanGFX& gfx, const Pose& p, float x0, float y0, float x1,
                 float y1, uint16_t color) {
  fillBox(gfx, p, x0, y0, x1, y1, color);
  fillBox(gfx, p, -x1, y0, -x0, y1, color);
}

/** Pod positions out along each wing for an engine count (per side). */
int podsPerSide(uint8_t engines, float out[3]) {
  switch (engines) {
    case 2:
    case 3:  // the third is in the tail, inside the fuselage outline
      out[0] = 3.4f;
      return 1;
    case 4:
      out[0] = 2.8f;
      out[1] = 5.4f;
      return 2;
    case 6:
    case 8:
      out[0] = 2.6f;
      out[1] = 4.4f;
      out[2] = 6.2f;
      return 3;
    default:
      return 0;
  }
}

void drawJet(lgfx::LovyanGFX& gfx, Pose p, const JetSpec& spec, uint8_t engines,
             bool rear, uint16_t color) {
  p.scale *= spec.scale;
  const float w = spec.body;
  const Tri body[] = {
      {0.0f, -8.5f, -w, -6.0f, w, -6.0f},
      {-w, -6.0f, w, -6.0f, w, 7.0f},
      {-w, -6.0f, w, 7.0f, -w, 7.0f},
      {-w, 7.0f, w, 7.0f, 0.0f, 8.5f},
  };
  for (const Tri& t : body) {
    fillTri(gfx, p, t, 1.0f, color);
  }
  // Wings and tailplane move out with a wider fuselage.
  const float dx = w - 1.3f;
  for (const Tri& t : kJetSide) {
    const Tri moved{t.x0 + dx, t.y0, t.x1 + dx, t.y1, t.x2 + dx, t.y2};
    fillTri(gfx, p, moved, 1.0f, color);
    fillTri(gfx, p, moved, -1.0f, color);
  }
  if (engines == 0) {
    return;
  }
  if (rear) {
    // Beside the rear fuselage, ahead of the tailplane.
    fillBoxPair(gfx, p, w + 0.1f, 2.6f, w + 1.6f, 5.4f, color);
    return;
  }
  float xs[3];
  const int n = podsPerSide(engines, xs);
  for (int i = 0; i < n; ++i) {
    const float x = xs[i] + dx;
    // Ahead of the swept leading edge (kJetSide: from (1.0,-2.8) to (8.2,3.0)).
    const float lead = -2.8f + (x - 1.0f - dx) * (5.8f / 7.2f);
    fillBoxPair(gfx, p, x - 0.7f, lead - 2.0f, x + 0.7f, lead + 0.6f, color);
  }
}

void drawTurboprop(lgfx::LovyanGFX& gfx, Pose p, uint8_t engines, uint16_t color) {
  p.scale *= kPropScale;
  const Tri body[] = {
      {0.0f, -7.5f, -1.1f, -5.5f, 1.1f, -5.5f},
      {-1.1f, -5.5f, 1.1f, -5.5f, 1.1f, 6.0f},
      {-1.1f, -5.5f, 1.1f, 6.0f, -1.1f, 6.0f},
      {-1.1f, 6.0f, 1.1f, 6.0f, 0.0f, 7.4f},
  };
  for (const Tri& t : body) {
    fillTri(gfx, p, t, 1.0f, color);
  }
  const Tri side[] = {
      {1.0f, -2.2f, 8.0f, -1.9f, 8.0f, -0.6f},  // straight wing, slight taper
      {1.0f, -2.2f, 8.0f, -0.6f, 1.0f, -0.3f},
      {0.9f, 4.6f, 3.6f, 4.9f, 3.6f, 6.0f},     // tailplane
      {0.9f, 4.6f, 3.6f, 6.0f, 0.9f, 6.2f},
  };
  for (const Tri& t : side) {
    fillTri(gfx, p, t, 1.0f, color);
    fillTri(gfx, p, t, -1.0f, color);
  }
  if (engines == 1) {
    fillBox(gfx, p, -2.6f, -8.4f, 2.6f, -7.6f, color);  // propeller on the nose
    return;
  }
  const float two[] = {3.2f};
  const float four[] = {2.8f, 5.6f};
  const float* xs = engines >= 4 ? four : two;
  const int n = engines >= 4 ? 2 : (engines >= 2 ? 1 : 0);
  for (int i = 0; i < n; ++i) {
    const float x = xs[i];
    fillBoxPair(gfx, p, x - 0.65f, -3.7f, x + 0.65f, 0.2f, color);  // nacelle
    fillBoxPair(gfx, p, x - 2.0f, -4.5f, x + 2.0f, -3.7f, color);   // propeller
  }
}

void drawRotor(lgfx::LovyanGFX& gfx, const Pose& p, uint16_t color) {
  const int cx = static_cast<int>(lroundf(p.cx)) + p.ox;
  const int cy = static_cast<int>(lroundf(p.cy)) + p.oy;
  gfx.fillCircle(cx, cy, static_cast<int>(lroundf(kRotorDiscR * p.scale)), color);
  for (int i = 0; i < 2; ++i) {
    // Blades at 45 degrees to the heading, so the X reads as a rotor.
    const float a = (i == 0 ? 45.0f : -45.0f) * kDegToRad;
    const float bx = sinf(a) * kRotorBladeLen;
    const float by = -cosf(a) * kRotorBladeLen;
    wideLine(gfx, p.px(-bx, -by), p.py(-bx, -by), p.px(bx, by), p.py(bx, by),
             0.7f * p.scale, color);
  }
}

void drawShape(lgfx::LovyanGFX& gfx, const Pose& p, uint8_t klass, uint8_t engines,
               bool rear, uint16_t color) {
  using util::aircraft::Klass;
  switch (static_cast<Klass>(klass)) {
    case Klass::kBizJet:
      drawJet(gfx, p, kBizJetSpec, engines, rear, color);
      return;
    case Klass::kRegional:
      drawJet(gfx, p, kRegionalSpec, engines, rear, color);
      return;
    case Klass::kNarrow:
      drawJet(gfx, p, kNarrowSpec, engines, rear, color);
      return;
    case Klass::kWide:
      drawJet(gfx, p, engines >= 4 ? kWideQuadSpec : kWideSpec, engines, rear, color);
      return;
    case Klass::kTurboprop:
      drawTurboprop(gfx, p, engines, color);
      return;
    case Klass::kRotor:
      drawRotor(gfx, p, color);
      return;
    case Klass::kLight:
      drawTemplate(gfx, p, kLightBody, kLightSide, color);
      return;
    case Klass::kHeavy: {
      Pose heavy = p;
      heavy.scale *= kHeavyScale;
      drawTemplate(gfx, heavy, kJetBody, kJetSide, color);
      return;
    }
    case Klass::kJet:
    default:
      drawTemplate(gfx, p, kJetBody, kJetSide, color);
      return;
  }
}

}  // namespace

Rgb altitudeColor(int32_t alt_ft) {
  struct Stop {
    int32_t ft;
    Rgb c;
  };
  static constexpr Stop kStops[] = {
      {0, {255, 128, 40}},       // orange: ground to pattern altitude
      {4000, {255, 205, 60}},    // amber: approach / departure
      {12000, {150, 235, 90}},   // lime: climbing / descending
      {24000, {70, 205, 255}},   // sky: upper climb
      {36000, {180, 140, 255}},  // violet: cruise
  };
  if (alt_ft == INT32_MIN) {
    return Rgb{200, 200, 200};
  }
  if (alt_ft <= kStops[0].ft) {
    return kStops[0].c;
  }
  for (size_t i = 1; i < sizeof(kStops) / sizeof(kStops[0]); ++i) {
    if (alt_ft <= kStops[i].ft) {
      const float t = static_cast<float>(alt_ft - kStops[i - 1].ft) /
                      static_cast<float>(kStops[i].ft - kStops[i - 1].ft);
      return mix(kStops[i - 1].c, kStops[i].c, t);
    }
  }
  return kStops[sizeof(kStops) / sizeof(kStops[0]) - 1].c;
}

void draw(lgfx::LovyanGFX& gfx, int cx, int cy, float heading_deg,
          uint8_t klass, float scale, uint16_t fill, uint16_t outline,
          uint8_t engines, bool rear_engines) {
  const float rad = heading_deg * kDegToRad;
  Pose p{static_cast<float>(cx), static_cast<float>(cy), sinf(rad), cosf(rad),
         scale, 0, 0};
  if (outline != fill) {
    static constexpr int kOffsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& o : kOffsets) {
      p.ox = o[0];
      p.oy = o[1];
      drawShape(gfx, p, klass, engines, rear_engines, outline);
    }
    p.ox = 0;
    p.oy = 0;
  }
  drawShape(gfx, p, klass, engines, rear_engines, fill);
}

int radiusPx(uint8_t klass, float scale) {
  using util::aircraft::Klass;
  float r = 8.5f;
  switch (static_cast<Klass>(klass)) {
    case Klass::kRotor:
      r = kRotorBladeLen * 0.75f;
      break;
    case Klass::kLight:
      r = 6.8f;
      break;
    case Klass::kHeavy:
      r = 8.5f * kHeavyScale;
      break;
    case Klass::kBizJet:
      r = 8.5f * kBizJetSpec.scale;
      break;
    case Klass::kRegional:
      r = 8.5f * kRegionalSpec.scale;
      break;
    case Klass::kWide:
      r = 8.5f * kWideQuadSpec.scale;
      break;
    case Klass::kTurboprop:
      r = 8.0f * kPropScale;
      break;
    default:
      break;
  }
  return static_cast<int>(lroundf(r * scale)) + 1;
}

}  // namespace ui::glyph
