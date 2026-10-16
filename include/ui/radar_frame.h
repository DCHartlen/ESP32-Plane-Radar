#pragma once

#include <LovyanGFX.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "ui/radar_geometry.h"

namespace ui::radar {

/**
 * The parts of a frame both modes draw (radar_display for aircraft, weather_display for the
 * weather radar): background, grid, labels, a message box and the clock. All draw into
 * `canvas`, at the current mode's range and in its palette.
 */

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

inline Rgb mixRgb(const Rgb& a, const Rgb& b, float t) {
  const auto mix = [t](uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(lroundf(x + (y - x) * t));
  };
  return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

/** Sets the kColor* values for the current mode. Call at the start of every frame. */
void applyPalette();

/**
 * Clears the round glass to the background, with the landmark lines on it. Brings the
 * landmark layer up to date first. Pixels outside the glass are left as they were.
 */
void clearFrame();

void drawRings();
void drawCrosshairs();
/** The house at the radar centre. */
void drawHome();
void drawCardinalLabels();
/** The ring-3 distance on the east spoke, on a background box. */
void drawScaleLabel();

/**
 * Text with a dark outline all round, like the clock, blended over whatever is under it (no
 * background box). Rendered once per text, size and placement and cached, so a label that
 * doesn't change costs ~1 ms a frame.
 */
void drawOutlinedText(const char* text, int height_px, int x, int y, textdatum_t datum,
                      uint16_t color);

/** Boxes of the cardinal letters and the scale label, for aircraft tag placement. */
size_t cardinalLabelCount();
ScreenRect cardinalLabelBox(size_t i);
ScreenRect scaleLabelBox();

/**
 * A headline (alert colour) and an optional detail line on a background box centred on
 * (kCenterX, center_y). Shown instead of the data when there isn't any to show.
 */
void drawMessageBox(const char* headline, const char* detail, int center_y);
/** Where the message box goes: between the first and second rings, below the centre. */
int messageBoxCenterY();

struct ClockLayout {
  char hhmm[8];
  char suffix[4];
  int left_x;
  int baseline_y;
  int time_w;
  int gap;
  int suffix_w;
  ScreenRect box;  // digits and outline, for tag placement
};

/** Measures the clock on the south spoke; false until the time is synced. */
bool layoutClock(ClockLayout* clock);
/** Local time on the south spoke; draw it last so it sits above everything. */
void drawClock(const ClockLayout& clock);

}  // namespace ui::radar
