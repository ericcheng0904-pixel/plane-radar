#include "ui/wide_line.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

/** Coverage at or above this paints the pixel solid, as drawWideLine does. */
constexpr float kSolid = 0.93f;

}  // namespace

void wideLine(lgfx::LGFXBase& gfx, float ax, float ay, float bx, float by, float r,
              uint16_t color) {
  int32_t clip_x = 0;
  int32_t clip_y = 0;
  int32_t clip_w = 0;
  int32_t clip_h = 0;
  gfx.getClipRect(&clip_x, &clip_y, &clip_w, &clip_h);
  if (clip_w <= 0 || clip_h <= 0 || !(r >= 0.0f)) {
    return;
  }
  const int x_min = static_cast<int>(clip_x);
  const int x_max = static_cast<int>(clip_x + clip_w - 1);

  // Pixel centres sit on integer coordinates, as in drawWideLine; a pixel's
  // coverage is how far it lies inside the edge, capped at 1 by kSolid.
  const float reach = r + 0.5f;
  const float y_top = std::min(ay, by);
  const float y_bot = std::max(ay, by);
  const int row0 = std::max(static_cast<int>(clip_y), static_cast<int>(floorf(y_top - reach)));
  const int row1 =
      std::min(static_cast<int>(clip_y + clip_h - 1), static_cast<int>(ceilf(y_bot + reach)));
  if (row0 > row1) {
    return;
  }
  const float dx = bx - ax;
  const float dy = by - ay;
  const float len2 = dx * dx + dy * dy;

  auto coverage = [&](int x, int y) {
    const float px = static_cast<float>(x) - ax;
    const float py = static_cast<float>(y) - ay;
    float t = len2 > 0.0f ? (px * dx + py * dy) / len2 : 0.0f;
    t = std::min(1.0f, std::max(0.0f, t));
    const float ex = px - t * dx;
    const float ey = py - t * dy;
    return reach - sqrtf(ex * ex + ey * ey);
  };
  auto plot = [&](int x, int y, float a) {
    if (a >= kSolid) {
      gfx.drawPixel(x, y, color);
    } else {
      gfx.fillRectAlpha(x, y, 1, 1, static_cast<uint8_t>(a * 255.0f), color);
    }
  };

  gfx.startWrite();
  for (int y = row0; y <= row1; ++y) {
    // The point of this row nearest the segment: where the segment crosses
    // the row, else level with the nearer end. The line is convex, so the
    // row's covered pixels are one run around that point: walk out both ways.
    const float yf = static_cast<float>(y);
    float xc;
    if (yf <= y_top) {
      xc = ay <= by ? ax : bx;
    } else if (yf >= y_bot) {
      xc = ay >= by ? ax : bx;
    } else {
      xc = ax + (yf - ay) / dy * dx;  // y_top < y < y_bot, so dy != 0
    }
    // The run holds that point, so if it holds any pixel it holds one of the
    // two either side. Clamped to the clip, a miss means it lies outside.
    auto clampX = [&](int x) { return std::min(x_max, std::max(x_min, x)); };
    const int below = static_cast<int>(floorf(xc));
    int start = clampX(below);
    float a = coverage(start, y);
    if (a <= 0.0f) {
      start = clampX(below + 1);
      a = coverage(start, y);
      if (a <= 0.0f) {
        continue;
      }
    }
    plot(start, y, a);
    for (int x = start - 1; x >= x_min && (a = coverage(x, y)) > 0.0f; --x) {
      plot(x, y, a);
    }
    for (int x = start + 1; x <= x_max && (a = coverage(x, y)) > 0.0f; ++x) {
      plot(x, y, a);
    }
  }
  gfx.endWrite();
}

}  // namespace ui
