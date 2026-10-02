#include "ui/status_screens.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <iterator>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "ui/ui_scale.h"

namespace {

using ui::px;

constexpr int kLineGap = px(6);
constexpr int kCenterX = config::kDisplayWidth / 2;
constexpr int kCenterY = config::kDisplayHeight / 2;

constexpr int kSpinnerDotCount = 10;
constexpr int kSpinnerRadius = kCenterX - px(7);
constexpr int kSpinnerDotRadius = px(2);
constexpr int kSpinnerEraseRadius = px(4);
constexpr float kSpinnerStepDeg = 6.0f;

struct SpinnerDot {
  int x = 0;
  int y = 0;
  bool drawn = false;
};

char s_connecting_ssid[33];
char s_ssid_line[33];
constexpr int kConnectingTextMaxWidthPx = static_cast<int>(0.9f * config::kDisplayWidth);
float s_spinner_angle_deg = -90.0f;
SpinnerDot s_spinner_dots[kSpinnerDotCount];
bool s_connecting_text_drawn = false;

constexpr auto& kGfxTitle = fonts::FreeSans18pt7b;
constexpr auto& kGfxBody = fonts::FreeSans12pt7b;
constexpr auto& kGfxDetail = fonts::Font2;
constexpr auto& kPortalGfxTitle = fonts::FreeSansBold18pt7b;
constexpr auto& kPortalGfxBody = fonts::FreeSansBold12pt7b;
constexpr auto& kPortalGfxEmphasis = fonts::FreeSansBold18pt7b;
constexpr auto& kConnectingGfxDetail = fonts::FreeSans9pt7b;

struct TextLine {
  const char* text;
  /** VLW text size on the original 240 px screen; scaled by ui::kElementScale. */
  float vlw_size;
  const lgfx::GFXfont* gfx_font;
};

void setVlwDesignSize(float design_size) {
  displayFontSetSmoothSize(canvas, design_size * ui::kElementScale);
}

int lineHeightGfx(const lgfx::GFXfont* font) {
  displayFontSetBitmap(canvas, font);
  return canvas.fontHeight();
}

int lineHeightVlw(float design_size) {
  setVlwDesignSize(design_size);
  return canvas.fontHeight();
}

void applyLineStyle(const TextLine& line) {
  if (displayFontIsSmooth()) {
    setVlwDesignSize(line.vlw_size);
  } else {
    displayFontSetBitmap(canvas, line.gfx_font);
  }
}

void drawTextBlock(uint16_t bg, uint16_t fg, const TextLine* lines, size_t count) {
  canvas.fillScreen(bg);
  canvas.setTextColor(fg, bg);
  canvas.setTextDatum(textdatum_t::middle_center);

  int total_h = 0;
  for (size_t i = 0; i < count; ++i) {
    if (displayFontIsSmooth()) {
      total_h += lineHeightVlw(lines[i].vlw_size);
    } else {
      total_h += lineHeightGfx(lines[i].gfx_font);
    }
    if (i + 1 < count) {
      total_h += kLineGap;
    }
  }

  int y = (config::kDisplayHeight - total_h) / 2;
  for (size_t i = 0; i < count; ++i) {
    applyLineStyle(lines[i]);
    const int h =
        displayFontIsSmooth() ? lineHeightVlw(lines[i].vlw_size)
                              : lineHeightGfx(lines[i].gfx_font);
    canvas.drawString(lines[i].text, kCenterX, y + h / 2);
    y += h + kLineGap;
  }
  displayPresent();
}

constexpr float kConnectingDetailVlw = 0.92f;

void applyConnectingDetailStyle() {
  if (displayFontIsSmooth()) {
    setVlwDesignSize(kConnectingDetailVlw);
  } else {
    displayFontSetBitmap(canvas, &kConnectingGfxDetail);
  }
}

/** SSID on one line; truncate with … if wider than kConnectingTextMaxWidthPx. */
void fitSsidLine() {
  strncpy(s_ssid_line, s_connecting_ssid, sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
  applyConnectingDetailStyle();
  if (canvas.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
    return;
  }
  const size_t len = strlen(s_connecting_ssid);
  for (size_t n = len; n > 0; --n) {
    snprintf(s_ssid_line, sizeof(s_ssid_line), "%.*s…", static_cast<int>(n),
             s_connecting_ssid);
    if (canvas.textWidth(s_ssid_line) <= kConnectingTextMaxWidthPx) {
      return;
    }
  }
  strncpy(s_ssid_line, "…", sizeof(s_ssid_line) - 1);
  s_ssid_line[sizeof(s_ssid_line) - 1] = '\0';
}

void drawConnectingText() {
  canvas.fillScreen(config::kColorBlack);

  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.setTextColor(config::kTextOnBlack, config::kColorBlack);

  applyConnectingDetailStyle();
  const int detail_h = canvas.fontHeight();
  const int total_h = detail_h * 2 + kLineGap;
  const int block_top = (config::kDisplayHeight - total_h) / 2;
  constexpr int kPanelPadY = px(8);
  canvas.fillRect(kCenterX - kConnectingTextMaxWidthPx / 2, block_top - kPanelPadY,
               kConnectingTextMaxWidthPx, total_h + kPanelPadY * 2, config::kColorBlack);

  int y = block_top;
  canvas.drawString("Connecting to", kCenterX, y + detail_h / 2);
  y += detail_h + kLineGap;
  canvas.drawString(s_ssid_line, kCenterX, y + detail_h / 2);

  s_connecting_text_drawn = true;
}

/** Push just the area a spinner dot (or its erase disc) covers. */
void presentSpinnerDot(const SpinnerDot& dot) {
  displayPresentRect(dot.x - kSpinnerEraseRadius, dot.y - kSpinnerEraseRadius,
                     kSpinnerEraseRadius * 2 + 1, kSpinnerEraseRadius * 2 + 1);
}

void eraseSpinnerDots() {
  for (int i = 0; i < kSpinnerDotCount; ++i) {
    if (!s_spinner_dots[i].drawn) {
      continue;
    }
    canvas.fillCircle(s_spinner_dots[i].x, s_spinner_dots[i].y, kSpinnerEraseRadius,
                   config::kColorBlack);
    s_spinner_dots[i].drawn = false;
  }
}

void drawSpinnerDots() {
  constexpr float kDegToRad = 0.01745329252f;
  const float head_rad = s_spinner_angle_deg * kDegToRad;

  for (int i = 0; i < kSpinnerDotCount; ++i) {
    const float a = head_rad - static_cast<float>(i) * (6.283185307f / kSpinnerDotCount);
    const int x = kCenterX + static_cast<int>(std::lround(std::cos(a) * kSpinnerRadius));
    const int y = kCenterY + static_cast<int>(std::lround(std::sin(a) * kSpinnerRadius));

    const int fade = 255 - i * 22;
    const uint16_t color = canvas.color565(0, fade, 0);
    canvas.fillSmoothCircle(x, y, kSpinnerDotRadius, color);

    s_spinner_dots[i].x = x;
    s_spinner_dots[i].y = y;
    s_spinner_dots[i].drawn = true;
  }
}

}  // namespace

void statusScreenConnectingBegin(const char* ssid) {
  const char* name = (ssid != nullptr && ssid[0] != '\0') ? ssid : "network";
  strncpy(s_connecting_ssid, name, sizeof(s_connecting_ssid) - 1);
  s_connecting_ssid[sizeof(s_connecting_ssid) - 1] = '\0';
  fitSsidLine();
  s_spinner_angle_deg = -90.0f;
  for (auto& dot : s_spinner_dots) {
    dot.drawn = false;
  }
  s_connecting_text_drawn = false;
  drawConnectingText();
  drawSpinnerDots();
  displayPresent();
}

void statusScreenConnectingTick() {
  if (!s_connecting_text_drawn) {
    drawConnectingText();
    displayPresent();
  }
  SpinnerDot previous[kSpinnerDotCount];
  std::copy(std::begin(s_spinner_dots), std::end(s_spinner_dots), previous);
  eraseSpinnerDots();
  s_spinner_angle_deg += kSpinnerStepDeg;
  if (s_spinner_angle_deg >= 270.0f) {
    s_spinner_angle_deg -= 360.0f;
  }
  drawSpinnerDots();
  // Push old and new dot areas only after both passes, so dots never blink.
  for (int i = 0; i < kSpinnerDotCount; ++i) {
    if (previous[i].drawn) {
      presentSpinnerDot(previous[i]);
    }
    presentSpinnerDot(s_spinner_dots[i]);
  }
}

void statusScreenPortal() {
  const TextLine lines[] = {
      {"Wi-Fi setup", 1.15f, &kPortalGfxTitle},
      {"1. Join network:", 1.05f, &kPortalGfxBody},
      {config::kPortalApName, 1.12f, &kPortalGfxEmphasis},
      {"2. Open in browser:", 1.05f, &kPortalGfxBody},
      {config::kPortalHostUrl, 1.12f, &kPortalGfxEmphasis},
      {"or 192.168.4.1", 1.0f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenConnectFailed() {
  const TextLine lines[] = {
      {"Could not connect", 1.15f, &kGfxTitle},
      {"Check Wi-Fi password", 1.0f, &kGfxBody},
      {"and signal strength.", 1.0f, &kGfxBody},
      {"Hold UP 3 sec", 1.0f, &kGfxBody},
      {"to reset Wi-Fi", 1.0f, &kGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenWifiReset() {
  const TextLine lines[] = {
      {"Wi-Fi reset", 1.15f, &kPortalGfxTitle},
      {"Restarting...", 1.05f, &kPortalGfxBody},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}
