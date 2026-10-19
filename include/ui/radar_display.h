#pragma once

#include <cstdint>

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/** How often the loop should redraw: faster while the Sweep theme's beam turns. */
unsigned long radarDisplayRedrawIntervalMs();

/** Drop cached label fonts after a portal setting changed. Redraw follows. */
void radarDisplayInvalidateStyle();

/**
 * Start the armed alert flash, if any, and clear it. The pulse is drawn into
 * the following frames for the duration set in the portal; nothing blocks.
 */
void radarDisplayStartAlert();

/**
 * Receives a finished band of the frame: `rows` rows from screen row `y0`,
 * RGB565 big-endian (LovyanGFX's sprite layout: high byte first), row-major,
 * `width` pixels per row. Valid only during the call.
 */
using FrameRowsFn = void (*)(const uint8_t* pixels, int y0, int rows, int width,
                             void* ctx);

/**
 * Draw a fresh frame for the portal's /screen.bmp, handing it to `sink` band
 * by band, top first, instead of to the panel. The frame is never held whole,
 * so there is no other way to read it. False until the radar has been shown.
 * Main loop only (it draws).
 */
bool radarDisplayCapture(FrameRowsFn sink, void* ctx);

/**
 * Allocate the frame buffer now without drawing. Setup step 2 shows instead of
 * the radar, and a town lookup may run meanwhile; taking the 57 KB block as
 * soon as Wi-Fi is up, as the radar always did, keeps the heap laid out the
 * way the radar's own fetches have been measured against.
 */
bool radarDisplayReserve();

}  // namespace ui
