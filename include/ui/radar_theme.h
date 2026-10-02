#pragma once

#include <cstdint>

#include "config.h"
#include "ui/ui_scale.h"

namespace ui::radar {

/** Layout derives from the screen size; element sizes are px() of the old 240 px values. */
constexpr int kSize = config::kDisplayWidth;
constexpr int kCenterX = kSize / 2;
constexpr int kCenterY = kSize / 2;

/** Text line height (fontHeight, px) for N/S/E/W. */
constexpr int kCardinalLabelHeightPx = px(14);
/** Scale label is this many px shorter than cardinals. */
constexpr int kScaleBelowCardinalPx = px(3);

/** Outermost grid ring (inside edge labels). */
constexpr int kGridOuterRadius = kCenterX - kCardinalLabelHeightPx + px(1);

/** N: offset from top edge (top_center, negative = up). */
constexpr int kCardinalNorthOffsetY = px(-1);
/** S: offset from bottom edge (bottom_center, positive = down). */
constexpr int kCardinalSouthOffsetY = px(3);
/** W/E: inset from the side edges; the round bezel clips glyph corners at x = 0. */
constexpr int kCardinalSideInsetPx = px(3);

/** Gap between scale label right edge and outer ring on the east spoke (px). */
constexpr int kScaleGapFromOuterRing = px(6);

constexpr int kRingCount = 4;

/** Shared grid stroke: drawWideLine half-width; rings use the same total width. */
constexpr float kGridStrokeHalfWidth = pxF(1.0f);

constexpr int kCenterDotRadius = px(2);

/** Filled aircraft symbol (nose triangle), 1.5× the original to balance the larger text. */
constexpr int kAircraftNoseLenPx = px(12);
constexpr int kAircraftTailLenPx = px(4.5f);
constexpr int kAircraftTailHalfPx = px(6);
/**
 * Track vector: from the aircraft's position to where it will be after this many
 * seconds at its current groundspeed and track, at the active range's scale.
 */
constexpr float kAircraftTrackHorizonSec = 30.0f;
/** drawWideLine half-width for speed vectors. */
constexpr float kAircraftTrackLineHalfWidth = pxF(1.0f);

constexpr float kRunwayLineWidthPx = pxF(2.0f);
constexpr float kRunwayLineHalfWidth = kRunwayLineWidthPx * 0.5f;
constexpr int kRunwayLabelHeightPx = kCardinalLabelHeightPx;
constexpr int kRunwayLabelGapPx = px(3);
/** Gap from triangle edge to tag block (px). */
constexpr int kAircraftLabelGapPx = px(1);
/** Keep symbol centroid inside outer ring by at least this inset (px). */
constexpr int kAircraftInsideRingInsetPx =
    kAircraftNoseLenPx + kAircraftTailHalfPx + px(1);

/** Beyond-ring traffic: bearing cues on screen rim (correct direction, fixed radius). */
constexpr int kBeyondRingDotRadiusPx = px(4);
constexpr int kBeyondRingScreenMarginPx = px(2);
/** Text line height (fontHeight, px) for aircraft tags (slightly above scale label). */
constexpr int kAircraftTagLabelHeightPx = px(13);

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
