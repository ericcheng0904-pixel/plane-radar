#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

#include "hardware/display_font.h"
#include "ui/panel_color.h"

namespace ui::radar {

constexpr int kSize = 240;
constexpr int kCenterX = kSize / 2;
constexpr int kCenterY = kSize / 2;

/** Outermost grid ring (inside edge labels). */
constexpr int kGridOuterRadius = 107;

/** N: offset from top edge (top_center, negative = up). */
constexpr int kCardinalNorthOffsetY = -1;
/** S: offset from bottom edge (bottom_center, positive = down). */
constexpr int kCardinalSouthOffsetY = 3;

/** Gap between scale label right edge and outer ring on the east spoke (px). */
constexpr int kScaleGapFromOuterRing = 6;

/**
 * Embedded font (kUiFont* index) per label role at each text size step.
 * Step 0 reproduces the sizes the radar shipped with; later steps shrink the
 * labels without shrinking the glyphs, so text stays anti-aliased.
 */
struct FontStepFonts {
  uint8_t cardinal;
  uint8_t scale;
  uint8_t tag;
  uint8_t runway;
};

constexpr FontStepFonts kFontStepFonts[] = {
    {kUiFont15, kUiFont11, kUiFont13, kUiFont15},  // normal
    {kUiFont13, kUiFont9, kUiFont11, kUiFont13},   // small
    {kUiFont11, kUiFont9, kUiFont9, kUiFont11},    // smallest
};

constexpr int kRingCount = 4;

/** Shared grid stroke: drawWideLine half-width (~2 px total); rings use the same px count. */
constexpr float kGridStrokeHalfWidth = 1.0f;

constexpr int kCenterDotRadius = 2;

/**
 * Aircraft silhouettes, indexed by util::aircraft::Klass.
 *
 * At this size only two outlines survive rotation — a triangle and a rotor —
 * so weight class is carried by triangle size instead of by shape. Index 1 is
 * the symbol the radar has always drawn.
 */
struct AircraftShape {
  int nose;
  int tail;
  int half;
};

constexpr AircraftShape kAircraftShapes[] = {
    {6, 2, 3},    // light
    {8, 3, 4},    // jet
    {11, 4, 6},   // heavy
    {8, 3, 4},    // rotor: drawn as a disc, kept here for tag spacing
};
constexpr size_t kAircraftShapeCount =
    sizeof(kAircraftShapes) / sizeof(kAircraftShapes[0]);

/** Index into kAircraftShapes used when per-class icons are switched off. */
constexpr size_t kAircraftShapeDefault = 1;

constexpr int kRotorDiscRadiusPx = 3;
constexpr int kRotorBladeLenPx = 7;

/** Longest silhouette, for insets that must hold for every class. */
constexpr int kAircraftNoseLenPx = 11;
constexpr int kAircraftTailHalfPx = 6;
/** Track vector: ground distance covered in this many seconds at current gs. */
constexpr float kAircraftTrackHorizonSec = 60.0f;
/** Minimum visible vector when gs > 0 (px). */
constexpr int kAircraftSpeedLineMinPx = 2;
/** Track line length uses this outer_km, not the active range preset. */
constexpr float kAircraftTrackRefOuterKm = 13.3f;
/** Shorter than full 60 s horizon at ref scale; ×1.5 length boost applied. */
constexpr float kAircraftTrackLengthScale = 1.5f / 5.0f;
/** drawWideLine half-width for speed vectors (~2 px total). */
constexpr float kAircraftTrackLineHalfWidth = 1.0f;

constexpr float kRunwayLineWidthPx = 2.0f;
constexpr float kRunwayLineHalfWidth = kRunwayLineWidthPx * 0.5f;
constexpr int kRunwayLabelGapPx = 3;
/** Gap from triangle edge to tag block (px). */
constexpr int kAircraftLabelGapPx = 1;
/** Airline names are cut to this width so a tag cannot swallow the radar. */
constexpr int kAircraftTagMaxWidthPx = 96;

/**
 * Trail length in time. Points are recorded once per fetch, so the count is
 * derived from the fetch interval rather than fixed: at 10 s per fetch a fixed
 * seven points drew 70 s of history as long lines across the disc.
 */
constexpr unsigned long kTrailSeconds = 30;
constexpr size_t kTrailDrawPoints =
    (kTrailSeconds * 1000 + config::kAdsbFetchIntervalMs - 1) / config::kAdsbFetchIntervalMs;

// Colours now live per theme in ui/theme.cpp.
constexpr int kTickMinorPx = 3;
constexpr int kTickMajorPx = 6;
/** How far the speed vector is mixed from background toward the glyph colour. */
constexpr float kTrackVectorStrength = 0.6f;
/** Keep symbol centroid inside outer ring by at least this inset (px). */
constexpr int kAircraftInsideRingInsetPx =
    kAircraftNoseLenPx + kAircraftTailHalfPx + 1;

/** Beyond-ring traffic: bearing cues on screen rim (correct direction, fixed radius). */
constexpr int kBeyondRingDotRadiusPx = 4;
constexpr int kBeyondRingScreenMarginPx = 2;

/** RGB565 palette targets (applied in initPalette). */
constexpr uint8_t kBgR = 4;
constexpr uint8_t kBgG = 10;
constexpr uint8_t kBgB = 28;
constexpr uint8_t kGridR = 16;
constexpr uint8_t kGridG = 100;
constexpr uint8_t kGridB = 32;
constexpr uint8_t kAircraftR = 255;
constexpr uint8_t kAircraftG = 0;
constexpr uint8_t kAircraftB = 0;
constexpr uint8_t kTrackR = 255;
constexpr uint8_t kTrackG = 0;
constexpr uint8_t kTrackB = 255;
constexpr uint8_t kTagTypeR = 255;
constexpr uint8_t kTagTypeG = 200;
constexpr uint8_t kTagTypeB = 0;
constexpr uint8_t kTagAltR = 90;
constexpr uint8_t kTagAltG = 200;
constexpr uint8_t kTagAltB = 255;
constexpr uint8_t kRunwayR = 56;
constexpr uint8_t kRunwayG = 150;
constexpr uint8_t kRunwayB = 170;
/** Lighter teal for ICAO labels (vs runway lines). */
constexpr uint8_t kRunwayLabelR = 110;
constexpr uint8_t kRunwayLabelG = 210;
constexpr uint8_t kRunwayLabelB = 230;

/**
 * Alert pulse: a ring sweeping out from the centre, repeated for the duration
 * set in the portal. Colours go through the same panel correction as the
 * aircraft red, which is the one the hardware is known to render correctly.
 */
constexpr uint8_t kAlertEmergencyR = 255;
constexpr uint8_t kAlertEmergencyG = 40;
constexpr uint8_t kAlertEmergencyB = 30;
constexpr uint8_t kAlertMilitaryR = 255;
constexpr uint8_t kAlertMilitaryG = 170;
constexpr uint8_t kAlertMilitaryB = 0;
constexpr uint8_t kAlertNotableR = 0;
constexpr uint8_t kAlertNotableG = 220;
constexpr uint8_t kAlertNotableB = 255;

/** One sweep of the ring from centre to rim. */
constexpr unsigned long kAlertPulseMs = 900;
/** Target frame interval during the animation (~22 fps). */
constexpr unsigned long kAlertFrameMs = 45;
constexpr float kAlertRingHalfWidth = 2.0f;

extern uint16_t kColorBackground;
extern uint16_t kColorGrid;
extern uint16_t kColorLabel;
extern uint16_t kColorCenter;
extern uint16_t kColorAircraft;
extern uint16_t kColorTrackVector;
extern uint16_t kColorTagType;
extern uint16_t kColorTagAltitude;
extern uint16_t kColorRunway;
extern uint16_t kColorRunwayLabel;

}  // namespace ui::radar
