#include "ui/radar_frame.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock.h"
#include "ui/canvas_pixels.h"
#include "ui/landmark_overlay.h"
#include "ui/radar_range.h"
#include "ui/radar_shapes.h"
#include "ui/radar_theme.h"

namespace ui::radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorAlert = 0xFE40;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
uint16_t kColorAltGround = 0x9CD3;
uint16_t kColorAltUnknown = 0xFFFF;

namespace {

/** The current mode's background and grid, for colours mixed from them. */
Rgb s_background_rgb{kBgR, kBgG, kBgB};
Rgb s_grid_rgb{kGridR, kGridG, kGridB};

/** Scale label is a little shorter than the cardinal letters. */
constexpr int kScaleLabelHeightPx = kCardinalLabelHeightPx - kScaleBelowCardinalPx;

uint16_t color565(const Rgb& c) { return canvas.color565(c.r, c.g, c.b); }

void applyCardinalStyle() { displayFontApply(canvas, kCardinalLabelHeightPx); }

void applyScaleStyle() { displayFontApply(canvas, kScaleLabelHeightPx); }

struct CardinalLabel {
  const char* text;
  int x;
  int y;
  textdatum_t datum;
};

constexpr CardinalLabel kCardinalLabels[] = {
    {"N", kCenterX, kCardinalNorthOffsetY, textdatum_t::top_center},
    {"S", kCenterX, kSize - 1 + kCardinalSouthOffsetY, textdatum_t::bottom_center},
    {"W", kCardinalSideInsetPx, kCenterY, textdatum_t::middle_left},
    {"E", kSize - 1 - kCardinalSideInsetPx, kCenterY, textdatum_t::middle_right},
};

/** Box of a cardinal letter, for the datums kCardinalLabels uses; cardinal font applied. */
ScreenRect cardinalLabelRect(const CardinalLabel& label) {
  const int w = canvas.textWidth(label.text);
  const int h = canvas.fontHeight();
  switch (label.datum) {
    case textdatum_t::top_center: return {label.x - w / 2, label.y, w, h};
    case textdatum_t::bottom_center: return {label.x - w / 2, label.y - h, w, h};
    case textdatum_t::middle_left: return {label.x, label.y - h / 2, w, h};
    case textdatum_t::middle_right: return {label.x - w, label.y - h / 2, w, h};
    default: return {label.x, label.y, w, h};
  }
}

int scaleLabelAnchorX() { return kCenterX + kGridOuterRadius - kScaleGapFromOuterRing; }

/** Scale label's background box, right edge padded past the middle_right anchor (x, y). */
ScreenRect scaleLabelRect(const char* text, int x, int y) {
  applyScaleStyle();
  const int tw = canvas.textWidth(text);
  const int th = canvas.fontHeight();
  constexpr int kPadX = px(3);
  constexpr int kPadY = px(2);
  return {x - tw - kPadX, y - th / 2 - kPadY, tw + kPadX * 2, th + kPadY * 2};
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  // One filled annulus: stacked 1 px circles leave pinholes once the stroke is thick.
  const int thickness = std::max(1, static_cast<int>(lroundf(kGridStrokeHalfWidth * 2.0f)));
  canvas.fillArc(cx, cy, std::max(0, r - thickness + 1), r, 0.0f, 360.0f, color);
}

/**
 * The clock's outline: the text drawn at 12 points around a circle, on a transparent
 * background (8 left notches at the corners once it got thicker).
 */
void drawStringOutline(lgfx::LGFXBase& gfx, const char* text, int x, int y, uint16_t color) {
  constexpr int kSteps = 12;
  constexpr float kStepRad = 6.2831853f / kSteps;
  constexpr float o = kClockOutlinePx;
  gfx.setTextColor(color);
  for (int i = 0; i < kSteps; ++i) {
    const int dx = static_cast<int>(lroundf(cosf(i * kStepRad) * o));
    const int dy = static_cast<int>(lroundf(sinf(i * kStepRad) * o));
    gfx.drawString(text, x + dx, y + dy);
  }
}

enum class ClockPass : uint8_t { Outline, Text };

/** One pass of the clock (time and suffix) into gfx, whose top-left is screen (ox, oy). */
void drawClockPass(lgfx::LGFXBase& gfx, const ClockLayout& clock, int ox, int oy,
                   ClockPass pass, uint16_t color) {
  gfx.setTextDatum(textdatum_t::baseline_left);
  const int y = clock.baseline_y - oy;
  const auto part = [&](const char* text, int x, int height_px) {
    displayFontApply(gfx, height_px);
    if (pass == ClockPass::Outline) {
      drawStringOutline(gfx, text, x, y, color);
    } else {
      gfx.setTextColor(color);
      gfx.drawString(text, x, y);
    }
  };
  part(clock.hhmm, clock.left_x - ox, kClockLabelHeightPx);
  if (clock.suffix_w > 0) {
    part(clock.suffix, clock.left_x + clock.time_w + clock.gap - ox, kAircraftTagLabelHeightPx);
  }
}

/**
 * The clock, rendered once per minute: two bytes per pixel, its alpha and how much of the
 * white text shows (black outline over the background, then white text). Blending this
 * costs a few ms, where drawing the 13 transparent anti-aliased strings cost ~60.
 */
struct ClockImage {
  char hhmm[8];
  char suffix[4];
  int x;
  int y;
  int w;
  int h;
  uint8_t* px;  // PSRAM, w * h * 2
};
ClockImage s_clock_image{};
LGFX_Sprite s_clock_sprite;

/**
 * A pixel's green level in the scratch sprite, 0–255: the coverage of what was drawn white
 * on black. The sprite stores RGB565 byte-swapped: RRRRRGGG GGGBBBBB.
 */
uint8_t clockCoverage(int x, int y) {
  const uint8_t* p = static_cast<const uint8_t*>(s_clock_sprite.getBuffer()) +
                     (static_cast<size_t>(y) * s_clock_sprite.width() + x) * 2;
  const uint32_t g6 = ((p[0] & 0x07u) << 3) | (p[1] >> 5);
  return static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
}

/** Renders the clock into s_clock_image if the text changed; false if out of memory. */
bool updateClockImage(const ClockLayout& clock) {
  ClockImage& img = s_clock_image;
  if (img.px != nullptr && strcmp(img.hhmm, clock.hhmm) == 0 &&
      strcmp(img.suffix, clock.suffix) == 0) {
    return true;
  }
  heap_caps_free(img.px);
  img = {};

  // Generous box: the outline, and glyphs that reach past their measured width or the
  // line height. Trimmed to the drawn pixels below.
  const int pad = kClockOutlinePx + px(2);
  const int ox = clock.left_x - pad;
  const int oy = clock.baseline_y - kClockLabelHeightPx - pad;
  const int w = clock.time_w + clock.gap + clock.suffix_w + pad * 2;
  const int h = kClockLabelHeightPx * 3 / 2 + pad * 2;

  s_clock_sprite.setPsram(true);
  s_clock_sprite.setColorDepth(16);
  if (s_clock_sprite.createSprite(w, h) == nullptr) {
    return false;
  }
  s_clock_sprite.setTextWrap(false);
  uint8_t* full = static_cast<uint8_t*>(
      heap_caps_malloc(static_cast<size_t>(w) * h * 2, MALLOC_CAP_SPIRAM));
  if (full == nullptr) {
    s_clock_sprite.deleteSprite();
    return false;
  }

  // Coverage of the outline and of the text, each drawn white on black.
  s_clock_sprite.fillSprite(TFT_BLACK);
  drawClockPass(s_clock_sprite, clock, ox, oy, ClockPass::Outline, TFT_WHITE);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      full[(y * w + x) * 2] = clockCoverage(x, y);
    }
  }
  s_clock_sprite.fillSprite(TFT_BLACK);
  drawClockPass(s_clock_sprite, clock, ox, oy, ClockPass::Text, TFT_WHITE);

  // On screen the result is bg * (1 - outline) * (1 - text) + white * text, so the alpha
  // is 1 - (1 - outline) * (1 - text).
  int min_x = w;
  int min_y = h;
  int max_x = -1;
  int max_y = -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t* p = &full[(y * w + x) * 2];
      const uint32_t text = clockCoverage(x, y);
      const uint32_t outline = p[0];
      p[0] = static_cast<uint8_t>(255 - ((255 - outline) * (255 - text) + 127) / 255);
      p[1] = static_cast<uint8_t>(text);
      if (p[0] != 0) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
      }
    }
  }
  s_clock_sprite.deleteSprite();

  if (max_x < 0) {  // nothing drawn; keep an empty image so this isn't redone every frame
    max_x = min_x = 0;
    max_y = min_y = 0;
    full[0] = 0;
  }
  img.w = max_x - min_x + 1;
  img.h = max_y - min_y + 1;
  img.x = ox + min_x;
  img.y = oy + min_y;
  img.px = static_cast<uint8_t*>(
      heap_caps_malloc(static_cast<size_t>(img.w) * img.h * 2, MALLOC_CAP_SPIRAM));
  if (img.px == nullptr) {
    heap_caps_free(full);
    img = {};
    return false;
  }
  for (int y = 0; y < img.h; ++y) {
    memcpy(&img.px[y * img.w * 2], &full[((min_y + y) * w + min_x) * 2],
           static_cast<size_t>(img.w) * 2);
  }
  heap_caps_free(full);
  snprintf(img.hhmm, sizeof(img.hhmm), "%s", clock.hhmm);
  snprintf(img.suffix, sizeof(img.suffix), "%s", clock.suffix);
  return true;
}

/** Blends s_clock_image straight into the canvas buffer (RGB565, native byte order). */
void blendClockImage() {
  const ClockImage& img = s_clock_image;
  uint16_t* fb = static_cast<uint16_t*>(canvas.getBuffer());
  if (fb == nullptr || img.px == nullptr) {
    return;
  }
  for (int y = 0; y < img.h; ++y) {
    const int sy = img.y + y;
    if (sy < 0 || sy >= kFbH) {
      continue;
    }
    const uint8_t* src = &img.px[y * img.w * 2];
    for (int x = 0; x < img.w; ++x, src += 2) {
      const uint32_t alpha = src[0];
      const int sx = img.x + x;
      if (alpha == 0 || sx < 0 || sx >= kFbW) {
        continue;
      }
      const size_t index = fbIndex(sx, sy);
      const uint32_t text = src[1];
      const uint32_t keep = 255 - alpha;
      const uint32_t dst = fb[index];
      const uint32_t r = ((dst >> 11) * keep + 31 * text + 127) / 255;
      const uint32_t g = (((dst >> 5) & 0x3F) * keep + 63 * text + 127) / 255;
      const uint32_t b = ((dst & 0x1F) * keep + 31 * text + 127) / 255;
      fb[index] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
  }
}

/** One row of the clear colour, in internal RAM so clearing never reads PSRAM back. */
alignas(4) uint16_t s_clear_row[config::kDisplayWidth];

}  // namespace

void applyPalette() {
  if (mode() == Mode::Weather) {
    s_background_rgb = {kWeatherBgR, kWeatherBgG, kWeatherBgB};
    s_grid_rgb = {kWeatherGridR, kWeatherGridG, kWeatherGridB};
  } else {
    s_background_rgb = {kBgR, kBgG, kBgB};
    s_grid_rgb = {kGridR, kGridG, kGridB};
  }
  kColorBackground = color565(s_background_rgb);
  kColorGrid = color565(s_grid_rgb);
  kColorLabel = canvas.color565(255, 255, 255);
  kColorCenter = canvas.color565(255, 255, 255);
  kColorAlert = canvas.color565(kAlertR, kAlertG, kAlertB);
  kColorTagType = canvas.color565(kTagTypeR, kTagTypeG, kTagTypeB);
  kColorRunway = canvas.color565(kRunwayR, kRunwayG, kRunwayB);
  kColorRunwayLabel = canvas.color565(kRunwayLabelR, kRunwayLabelG, kRunwayLabelB);
  kColorAltGround = canvas.color565(kAltGroundR, kAltGroundG, kAltGroundB);
  kColorAltUnknown = canvas.color565(kAltUnknownR, kAltUnknownG, kAltUnknownB);
}

/**
 * fillScreen for a colour whose two bytes differ fills a short run, then memcpys PSRAM to
 * PSRAM, reading back as much as it writes (~100 ms). This copies an internal row instead,
 * and only the part of each row inside the round glass: the corners (~21% of the buffer)
 * are never seen, so whatever was drawn there can stay.
 *
 * Where a row has landmark lines, that span is copied from the landmark layer instead
 * (background with the lines on it), so those pixels are written once rather than cleared
 * and then overwritten.
 */
void clearFrame() {
  // Only does work after a zoom, a mode switch or a location change.
  landmark::prepareLandmarks(canvas);

  const uint16_t color = kColorBackground;
  uint16_t* fb = static_cast<uint16_t*>(canvas.getBuffer());
  if (fb == nullptr) {
    canvas.fillScreen(color);
    return;
  }
  if (s_clear_row[0] != color || s_clear_row[config::kDisplayWidth - 1] != color) {
    std::fill(std::begin(s_clear_row), std::end(s_clear_row), color);
  }
  // The canvas is rgb565_nonswapped, so color565() values go in as they are.
  const auto fill = [](uint16_t* dst, int from, int to) {
    if (to > from) {
      memcpy(dst + from, s_clear_row, static_cast<size_t>(to - from) * sizeof(uint16_t));
    }
  };
  constexpr int kW = config::kDisplayWidth;
  constexpr float kRadius = kW * 0.5f;
  // Spans start and end on 16 px (one 32-byte cache line), which also leaves a few px of
  // margin outside the glass.
  constexpr int kAlignPx = 16;
  static_assert(kW % kAlignPx == 0, "rows must split into whole cache lines");
  for (int y = 0; y < config::kDisplayHeight; ++y) {
    const float dy = static_cast<float>(y) + 0.5f - config::kDisplayHeight * 0.5f;
    const float half_sq = kRadius * kRadius - dy * dy;
    const int half = half_sq > 0.0f ? static_cast<int>(ceilf(sqrtf(half_sq))) : 0;
    const int left = std::max(0, (kW / 2 - half) / kAlignPx * kAlignPx);
    const int right = kW - left;  // symmetric about the centre
    uint16_t* row = fb + static_cast<size_t>(y) * kW;
    int lines_left = 0;
    int lines_right = 0;
    const uint16_t* lines = landmark::landmarkRow(y, &lines_left, &lines_right);
    if (lines == nullptr) {
      fill(row, left, right);
      continue;
    }
    lines_left = std::clamp(lines_left, left, right);
    lines_right = std::clamp(lines_right, lines_left, right);
    fill(row, left, lines_left);
    memcpy(row + lines_left, lines + lines_left,
           static_cast<size_t>(lines_right - lines_left) * sizeof(uint16_t));
    fill(row, lines_right, right);
  }
}

void drawRings() {
  for (int i = 1; i <= kRingCount; ++i) {
    drawGridRing(kCenterX, kCenterY, kGridOuterRadius * i / kRingCount, kColorGrid);
  }
}

/**
 * Axis-aligned, so a solid odd-width core plus one partly covered line of pixels each side
 * looks like drawWideLine's anti-aliasing without blending every pixel (~25 ms → <1 ms).
 */
void drawCrosshairs() {
  const int cx = kCenterX;
  const int cy = kCenterY;
  const int radius = kGridOuterRadius;
  const float width = kGridStrokeHalfWidth * 2.0f;
  int core = std::max(1, static_cast<int>(width));
  if (core % 2 == 0) {
    --core;
  }
  const int half = core / 2;
  const int len = radius * 2 + 1;
  const float edge_coverage = (width - static_cast<float>(core)) * 0.5f;

  // Edges first, so the other line's core covers them where the two cross.
  if (edge_coverage > 0.05f) {
    const uint16_t edge_color =
        color565(mixRgb(s_background_rgb, s_grid_rgb, std::min(edge_coverage, 1.0f)));
    canvas.fillRect(cx - half - 1, cy - radius, 1, len, edge_color);
    canvas.fillRect(cx + half + 1, cy - radius, 1, len, edge_color);
    canvas.fillRect(cx - radius, cy - half - 1, len, 1, edge_color);
    canvas.fillRect(cx - radius, cy + half + 1, len, 1, edge_color);
  }
  canvas.fillRect(cx - half, cy - radius, core, len, kColorGrid);
  canvas.fillRect(cx - radius, cy - half, len, core, kColorGrid);
}

void drawHome() { drawHomeMarker(kCenterX, kCenterY, kColorCenter, kColorBackground); }

void drawCardinalLabels() {
  applyCardinalStyle();
  canvas.setTextColor(kColorLabel, kColorBackground);
  for (const CardinalLabel& label : kCardinalLabels) {
    canvas.setTextDatum(label.datum);
    canvas.drawString(label.text, label.x, label.y);
  }
}

void drawScaleLabel() {
  char text[12];
  formatCurrentRing3Label(text, sizeof(text));
  const ScreenRect box = scaleLabelRect(text, scaleLabelAnchorX(), kCenterY);
  canvas.fillRect(box.left, box.top, box.w, box.h, kColorBackground);
  canvas.setTextDatum(textdatum_t::middle_right);
  canvas.setTextColor(kColorGrid, kColorBackground);
  canvas.drawString(text, scaleLabelAnchorX(), kCenterY);
}

size_t cardinalLabelCount() { return sizeof(kCardinalLabels) / sizeof(kCardinalLabels[0]); }

ScreenRect cardinalLabelBox(size_t i) {
  applyCardinalStyle();
  return cardinalLabelRect(kCardinalLabels[i]);
}

ScreenRect scaleLabelBox() {
  char text[12];
  formatCurrentRing3Label(text, sizeof(text));
  return scaleLabelRect(text, scaleLabelAnchorX(), kCenterY);
}

int messageBoxCenterY() { return kCenterY + kGridOuterRadius * 3 / 8; }

void drawMessageBox(const char* headline, const char* detail, int center_y) {
  const int cx = kCenterX;
  constexpr int kPadX = px(4);
  constexpr int kPadY = px(2);
  const bool has_detail = detail != nullptr && detail[0] != '\0';

  displayFontApply(canvas, kCardinalLabelHeightPx);
  const int headline_w = canvas.textWidth(headline);
  const int headline_h = canvas.fontHeight();
  int detail_w = 0;
  int detail_h = 0;
  if (has_detail) {
    displayFontApply(canvas, kAircraftTagLabelHeightPx);
    detail_w = canvas.textWidth(detail);
    detail_h = canvas.fontHeight();
  }

  const int box_w = std::max(headline_w, detail_w) + kPadX * 2;
  const int box_h = headline_h + detail_h + kPadY * 2;
  const int top = center_y - box_h / 2;
  canvas.fillRect(cx - box_w / 2, top, box_w, box_h, kColorBackground);

  canvas.setTextDatum(textdatum_t::top_center);
  displayFontApply(canvas, kCardinalLabelHeightPx);
  canvas.setTextColor(kColorAlert, kColorBackground);
  canvas.drawString(headline, cx, top + kPadY);
  if (has_detail) {
    displayFontApply(canvas, kAircraftTagLabelHeightPx);
    canvas.setTextColor(kColorLabel, kColorBackground);
    canvas.drawString(detail, cx, top + kPadY + headline_h);
  }
}

bool layoutClock(ClockLayout* clock) {
  if (!services::clock::formatTime(clock->hhmm, sizeof(clock->hhmm), clock->suffix,
                                   sizeof(clock->suffix))) {
    return false;
  }

  displayFontApply(canvas, kAircraftTagLabelHeightPx);
  clock->suffix_w = clock->suffix[0] != '\0' ? canvas.textWidth(clock->suffix) : 0;
  displayFontApply(canvas, kClockLabelHeightPx);
  clock->time_w = canvas.textWidth(clock->hhmm);
  clock->gap = clock->suffix_w > 0 ? kClockSuffixGapPx : 0;

  // Both parts share a baseline. Digits are about half the line height tall, so a
  // baseline ~0.27 line heights below the centre puts them on it.
  const int center_y =
      kCenterY + static_cast<int>(kGridOuterRadius * kClockCenterRingFraction);
  clock->baseline_y = center_y + kClockLabelHeightPx * 27 / 100;
  const int total_w = clock->time_w + clock->gap + clock->suffix_w;
  clock->left_x = kCenterX - total_w / 2;

  // The digits span about ±0.3 line heights around center_y; add the outline all round.
  const int half_h = kClockLabelHeightPx * 32 / 100 + kClockOutlinePx;
  clock->box = {clock->left_x - kClockOutlinePx, center_y - half_h,
                total_w + kClockOutlinePx * 2, half_h * 2};
  return true;
}

void drawClock(const ClockLayout& clock) {
  if (updateClockImage(clock)) {
    blendClockImage();
    return;
  }
  // Out of PSRAM: draw it straight onto the canvas, the slow way.
  drawClockPass(canvas, clock, 0, 0, ClockPass::Outline, config::kColorBlack);
  drawClockPass(canvas, clock, 0, 0, ClockPass::Text, kColorLabel);
}

}  // namespace ui::radar
