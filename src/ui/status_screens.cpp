#include "ui/status_screens.h"

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>

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
constexpr float kSpinnerStepDeg = 6.0f;

char s_connecting_ssid[33];
char s_ssid_line[33];
constexpr int kConnectingTextMaxWidthPx = static_cast<int>(0.9f * config::kDisplayWidth);
float s_spinner_angle_deg = -90.0f;

/** Line heights in design px (old 240 px screen); scaled with px(). */
constexpr float kTitleHeight = 18.5f;
constexpr float kEmphasisHeight = 18.0f;
constexpr float kBodyHeight = 17.0f;
constexpr float kNoteHeight = 16.0f;
constexpr float kConnectingDetailHeight = 15.0f;

struct TextLine {
  const char* text;
  float design_height;
};

int applyLineStyle(const TextLine& line) {
  const int h = px(line.design_height);
  displayFontApply(canvas, h);
  return h;
}

void drawTextBlock(uint16_t bg, uint16_t fg, const TextLine* lines, size_t count) {
  canvas.fillScreen(bg);
  canvas.setTextColor(fg, bg);
  canvas.setTextDatum(textdatum_t::middle_center);

  int total_h = 0;
  for (size_t i = 0; i < count; ++i) {
    total_h += px(lines[i].design_height);
    if (i + 1 < count) {
      total_h += kLineGap;
    }
  }

  int y = (config::kDisplayHeight - total_h) / 2;
  for (size_t i = 0; i < count; ++i) {
    const int h = applyLineStyle(lines[i]);
    canvas.drawString(lines[i].text, kCenterX, y + h / 2);
    y += h + kLineGap;
  }
  displayPresent();
}

void applyConnectingDetailStyle() {
  displayFontApply(canvas, px(kConnectingDetailHeight));
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
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.setTextColor(config::kTextOnBlack, config::kColorBlack);

  applyConnectingDetailStyle();
  const int detail_h = canvas.fontHeight();
  const int total_h = detail_h * 2 + kLineGap;
  int y = (config::kDisplayHeight - total_h) / 2;
  canvas.drawString("Connecting to", kCenterX, y + detail_h / 2);
  y += detail_h + kLineGap;
  canvas.drawString(s_ssid_line, kCenterX, y + detail_h / 2);
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
  }
}

/** The whole screen every tick: the back buffer holds the frame before last. */
void drawConnectingScreen() {
  canvas.fillScreen(config::kColorBlack);
  drawConnectingText();
  drawSpinnerDots();
  displayPresent();
}

}  // namespace

void statusScreenConnectingBegin(const char* ssid) {
  const char* name = (ssid != nullptr && ssid[0] != '\0') ? ssid : "network";
  strncpy(s_connecting_ssid, name, sizeof(s_connecting_ssid) - 1);
  s_connecting_ssid[sizeof(s_connecting_ssid) - 1] = '\0';
  fitSsidLine();
  s_spinner_angle_deg = -90.0f;
  drawConnectingScreen();
}

void statusScreenConnectingTick() {
  s_spinner_angle_deg += kSpinnerStepDeg;
  if (s_spinner_angle_deg >= 270.0f) {
    s_spinner_angle_deg -= 360.0f;
  }
  drawConnectingScreen();
}

void statusScreenPortal() {
  const TextLine lines[] = {
      {"Wi-Fi setup", kTitleHeight},
      {"1. Join network:", kBodyHeight},
      {config::kPortalApName, kEmphasisHeight},
      {"2. Open in browser:", kBodyHeight},
      {config::kPortalHostUrl, kEmphasisHeight},
      {"or 192.168.4.1", kNoteHeight},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenConnectFailed() {
  const TextLine lines[] = {
      {"Could not connect", kTitleHeight},
      {"Check Wi-Fi password", kNoteHeight},
      {"and signal strength.", kNoteHeight},
      {"Hold UP 3 sec", kNoteHeight},
      {"to reset Wi-Fi", kNoteHeight},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}

void statusScreenWifiReset() {
  const TextLine lines[] = {
      {"Wi-Fi reset", kTitleHeight},
      {"Restarting...", kBodyHeight},
  };
  drawTextBlock(config::kColorYellow, config::kTextOnYellow, lines,
                sizeof(lines) / sizeof(lines[0]));
}
