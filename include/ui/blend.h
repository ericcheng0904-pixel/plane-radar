#pragma once

#include <LovyanGFX.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ui {

/**
 * Blend pixels x0 <= x < x1 of row y toward `tint` by `amount` (0 = unchanged,
 * 256 = tint), straight in a 16-bit LovyanGFX sprite buffer (big-endian RGB565,
 * `width` pixels per row). Per-channel blending is the same in either R/B
 * order, so the panel's colour swap needs no care. The caller clips.
 */
inline void blendSpan(uint8_t* buf, int width, int y, int x0, int x1, uint16_t tint,
                      int amount) {
  if (amount <= 0 || x1 <= x0) {
    return;
  }
  const int keep = 256 - amount;
  const int tr = (tint >> 11) * amount;
  const int tg = ((tint >> 5) & 0x3F) * amount;
  const int tb = (tint & 0x1F) * amount;
  uint8_t* p = buf + (static_cast<size_t>(y) * width + x0) * 2;
  for (int x = x0; x < x1; ++x, p += 2) {
    const uint16_t v = static_cast<uint16_t>((p[0] << 8) | p[1]);
    const int r5 = ((v >> 11) * keep + tr) >> 8;
    const int g6 = (((v >> 5) & 0x3F) * keep + tg) >> 8;
    const int b5 = ((v & 0x1F) * keep + tb) >> 8;
    const uint16_t o = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
    p[0] = static_cast<uint8_t>(o >> 8);
    p[1] = static_cast<uint8_t>(o);
  }
}

/**
 * blendSpan into a sprite, clipped to its clip rectangle. The radar frame is a
 * full-screen view onto a half-height buffer (see radar_display.cpp): only the
 * rows inside the clip rectangle are backed by memory, so every direct write
 * must come through here.
 */
inline void blendSpanClipped(LGFX_Sprite& f, int y, int x0, int x1, uint16_t tint,
                             int amount) {
  int32_t cx = 0;
  int32_t cy = 0;
  int32_t cw = 0;
  int32_t ch = 0;
  f.getClipRect(&cx, &cy, &cw, &ch);
  if (y < cy || y >= cy + ch) {
    return;
  }
  auto* buf = static_cast<uint8_t*>(f.getBuffer());
  if (buf == nullptr) {
    return;
  }
  blendSpan(buf, f.width(), y, std::max(x0, static_cast<int>(cx)),
            std::min(x1, static_cast<int>(cx + cw)), tint, amount);
}

/** Rows [*y0, *y1) of `f` that drawing can reach: its clip rectangle's. */
inline void clipRows(LGFX_Sprite& f, int* y0, int* y1) {
  int32_t cx = 0;
  int32_t cy = 0;
  int32_t cw = 0;
  int32_t ch = 0;
  f.getClipRect(&cx, &cy, &cw, &ch);
  *y0 = static_cast<int>(cy);
  *y1 = static_cast<int>(cy + ch);
}

}  // namespace ui
