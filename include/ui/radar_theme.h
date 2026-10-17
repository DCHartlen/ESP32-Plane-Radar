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

/** House at the radar centre (radar_shapes.cpp): scale of its shape data, and its radius. */
constexpr float kHomeMarkerScale = 1.35f;
constexpr int kHomeMarkerRadiusPx = px(5.5f * kHomeMarkerScale);

/**
 * Aircraft icons (radar_shapes.cpp): every shape is drawn at this scale, and the radius
 * covers it at any rotation. Keep the radii in step with the shape data; tag placement and
 * the ring inset use them.
 */
constexpr float kAircraftIconScale = 0.8f;
constexpr int kIconRadiusGenericPx = px(12.0f * kAircraftIconScale);
constexpr int kIconRadiusJetPx = px(14.5f * kAircraftIconScale);
constexpr int kIconRadiusBizJetPx = px(14.0f * kAircraftIconScale);
constexpr int kIconRadiusLightPx = px(12.0f * kAircraftIconScale);
constexpr int kIconRadiusGliderPx = px(14.5f * kAircraftIconScale);
constexpr int kIconRadiusHelicopterPx = px(13.0f * kAircraftIconScale);
constexpr int kIconRadiusBalloonPx = px(10.0f * kAircraftIconScale);
constexpr int kAircraftIconMaxRadiusPx = px(14.5f * kAircraftIconScale);
/** drawWideLine half-width for speed vectors. */
constexpr float kAircraftTrackLineHalfWidth = pxF(1.0f);
/** Speed vectors use the icon's altitude colour, mixed this much toward the background. */
constexpr float kAircraftTrackDimming = 0.35f;

constexpr float kRunwayLineWidthPx = pxF(2.0f);
constexpr float kRunwayLineHalfWidth = kRunwayLineWidthPx * 0.5f;
constexpr int kRunwayLabelHeightPx = kCardinalLabelHeightPx;
constexpr int kRunwayLabelGapPx = px(3);
/**
 * Landmark lines (landmark_overlay.cpp): shores, islands, rivers and the international
 * border, drawn under the grid. Water is a deep blue, darker than the cyan and ice-blue
 * altitude colours and bluer than the teal runways; the border is a dashed mauve, a hue the
 * altitude scale skips. Water lines are thinner than the grid stroke so the grid stays on top.
 */
constexpr float kShoreLineHalfWidth = pxF(0.75f);
constexpr float kIslandLineHalfWidth = pxF(0.6f);
constexpr float kRiverLineHalfWidth = pxF(0.525f);
constexpr float kBorderLineHalfWidth = pxF(1.0f);
/** Border dashes: visible length of each dash and of each gap. */
constexpr float kBorderDashPx = pxF(5.0f);
constexpr float kBorderGapPx = pxF(3.5f);
/** Gap from the icon's radius to its tag (px). */
constexpr int kAircraftLabelGapPx = px(1);
/**
 * Every corner of a tag stays within this radius of the centre, so the round bezel can't
 * hide part of it. A guess at the visible circle; measure it on the panel.
 */
constexpr int kTagScreenRadiusPx = kCenterX - px(2);
/** Keep symbol centroid inside outer ring by at least this inset (px). */
constexpr int kAircraftInsideRingInsetPx = kAircraftIconMaxRadiusPx + px(1);

/**
 * Beyond-ring traffic: a cue on the screen rim at the aircraft's bearing (fixed radius).
 * Moving aircraft get a notched arrow (radar_shapes.cpp) pointing along their track, so it
 * shows whether they're heading in or out; stationary ones (gs 0) and ones with no track get
 * a plain dot. Both are coloured by altitude.
 */
constexpr int kBeyondRingDotRadiusPx = px(4);
/** Covers the arrow at any rotation (tip 6, back corners sqrt(4² + 4.5²) ≈ 6.0). */
constexpr int kBeyondRingMarkerRadiusPx = px(6.5f);
constexpr int kBeyondRingScreenMarginPx = px(2);
/** Text line height (fontHeight, px) for aircraft tags (slightly above scale label). */
constexpr int kAircraftTagLabelHeightPx = px(13);
/**
 * Climb/descend arrow after a tag's altitude: a triangle (radar_shapes.cpp, 7 wide, 6 tall)
 * shown when the vertical rate is beyond kClimbArrowMinFpm either way.
 */
constexpr int kClimbArrowMinFpm = 300;
constexpr int kClimbArrowWidthPx = px(7);
constexpr int kClimbArrowGapPx = px(2);

/** RGB565 palette targets (applied in applyPalette). Plane mode: navy with a green grid. */
constexpr uint8_t kBgR = 4;
constexpr uint8_t kBgG = 10;
constexpr uint8_t kBgB = 28;
constexpr uint8_t kGridR = 16;
constexpr uint8_t kGridG = 100;
constexpr uint8_t kGridB = 32;
/** Weather mode: a greyer navy with a blue-grey grid, so the green rain stands out. */
constexpr uint8_t kWeatherBgR = 8;
constexpr uint8_t kWeatherBgG = 16;
constexpr uint8_t kWeatherBgB = 24;
constexpr uint8_t kWeatherGridR = 90;
constexpr uint8_t kWeatherGridG = 110;
constexpr uint8_t kWeatherGridB = 120;
/** Tag type line: neutral grey, so the colours stay free for altitude. */
constexpr uint8_t kTagTypeR = 170;
constexpr uint8_t kTagTypeG = 180;
constexpr uint8_t kTagTypeB = 190;
/** Headline of the "no data" message. */
constexpr uint8_t kAlertR = 255;
constexpr uint8_t kAlertG = 200;
constexpr uint8_t kAlertB = 0;
constexpr uint8_t kRunwayR = 56;
constexpr uint8_t kRunwayG = 150;
constexpr uint8_t kRunwayB = 170;
/** Lighter teal for ICAO labels (vs runway lines). */
constexpr uint8_t kRunwayLabelR = 110;
constexpr uint8_t kRunwayLabelG = 210;
constexpr uint8_t kRunwayLabelB = 230;

/**
 * Landmark lines: shores and islands, rivers, and the border. Rivers are a dim, greyed
 * steel blue, so the saturated blue reads as a lake shore and the two don't blur together.
 */
constexpr uint8_t kShoreR = 48;
constexpr uint8_t kShoreG = 100;
constexpr uint8_t kShoreB = 196;
constexpr uint8_t kRiverR = 62;
constexpr uint8_t kRiverG = 86;
constexpr uint8_t kRiverB = 124;
/**
 * Rivers fade toward the background and lose detail as the range widens: none of either up
 * to kRiverFadeNearKm (outer ring), the full amount from kRiverFadeFarKm, linear between.
 * At the full amount the colour is kRiverFarFade of the way to the background, and points
 * closer than kRiverFarMinSegmentPx to the last drawn one are skipped (meanders smooth out).
 */
constexpr float kRiverFadeNearKm = 34.0f;
constexpr float kRiverFadeFarKm = 133.0f;
constexpr float kRiverFarFade = 0.5f;
constexpr float kRiverFarMinSegmentPx = pxF(1.4f);
/**
 * In weather mode the rain is drawn over the landmark lines, and the lines are blended back
 * over it at this alpha (0-255), so they show through the rain.
 */
constexpr uint8_t kLandmarkOverRainAlpha = 110;
constexpr uint8_t kBorderR = 150;
constexpr uint8_t kBorderG = 92;
constexpr uint8_t kBorderB = 150;

/**
 * Altitude colour (icon, rim marker, speed vector and the tag's altitude line): warm to
 * cool, red-orange near the ground through yellow and cream to cyan at cruise.
 * Interpolated between stops, clamped beyond the ends. It skips green (the grid) and
 * magenta/violet, which clash with the green grid and sat right where airport traffic is.
 */
struct AltitudeColorStop {
  int32_t alt_ft;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};
constexpr AltitudeColorStop kAltitudeColorStops[] = {
    // Landed aircraft are filtered out and the lowest airborne ones are ~1000 ft, so the
    // gradient starts there and spends most of its range on approach and climb-out.
    // Yellow is a saturated gold and the next stop a cool, unsaturated blue-white, so they
    // differ in saturation as well as hue (a warm cream read as the same yellow).
    {1000, 255, 80, 40},      // red-orange: short final, just airborne
    {3000, 255, 140, 20},     // amber: approach, circuit
    {6000, 255, 210, 0},      // gold: climb and descent
    {12000, 215, 228, 255},   // pale blue-white (lighter and less saturated than ice blue)
    {20000, 150, 210, 255},   // ice blue
    {30000, 30, 200, 255},    // cyan: overflights
};
/** On the ground, and altitude not reported (grey, so it can't pass for the cream band). */
constexpr uint8_t kAltGroundR = 130;
constexpr uint8_t kAltGroundG = 130;
constexpr uint8_t kAltGroundB = 130;
constexpr uint8_t kAltUnknownR = 200;
constexpr uint8_t kAltUnknownG = 200;
constexpr uint8_t kAltUnknownB = 200;

/** Clock: centred on the south spoke, this far below centre as a fraction of the outer ring. */
constexpr float kClockCenterRingFraction = 0.66f;
/** Line height (fontHeight, px) of HH:MM; the AM/PM suffix uses the tag size. */
constexpr int kClockLabelHeightPx = px(30);
/** Black outline thickness around the clock text. */
constexpr int kClockOutlinePx = px(2.5f);
/**
 * Black outline around the labels (cardinals, scale, airports, the radar's age): thinner
 * than the clock's, for text about half its height.
 */
constexpr float kLabelOutlinePx = pxF(1.5f);
/** Gap between the time and its AM/PM suffix. */
constexpr int kClockSuffixGapPx = px(2);

extern uint16_t kColorBackground;
extern uint16_t kColorGrid;
extern uint16_t kColorLabel;
extern uint16_t kColorCenter;
extern uint16_t kColorTagType;
extern uint16_t kColorAlert;
extern uint16_t kColorRunway;
extern uint16_t kColorRunwayLabel;
extern uint16_t kColorAltGround;
extern uint16_t kColorAltUnknown;

}  // namespace ui::radar
