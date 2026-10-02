#ifdef PANEL_TEST

#include "hardware/panel_test.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "hardware/panel.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"

// Optional: -DPANEL_TEST_WIFI_SSID=\"name\" -DPANEL_TEST_WIFI_PASS=\"pass\".
// Without them the test uses the credentials the normal firmware saved.

namespace {

constexpr int kW = config::kDisplayWidth;
constexpr int kH = config::kDisplayHeight;
constexpr int kCx = kW / 2;
constexpr int kCy = kH / 2;

constexpr unsigned long kFetchIntervalMs = 5000;
constexpr unsigned long kButtonPollMs = 20;
constexpr float kFetchRadiusKm = 50.0f;

// Status line; any change redraws the whole test screen.
constexpr int kStatusX = kCx - 250;
constexpr int kStatusY = 560;
constexpr int kStatusW = 500;
constexpr int kStatusH = 40;

bool s_up_level = false;
bool s_down_level = false;
unsigned long s_last_button_poll_ms = 0;
uint32_t s_fetch_ok = 0;
uint32_t s_fetch_fail = 0;
unsigned long s_last_fetch_ms = 0;

void drawColorBars() {
  // Solid primaries: each must show as its own color, with no bleed between channels.
  constexpr int kLeft = kCx - 240;
  constexpr int kTop = 150;
  constexpr int kBarW = 120;
  constexpr int kBarH = 90;
  const uint16_t solids[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  for (int i = 0; i < 4; ++i) {
    canvas.fillRect(kLeft + i * kBarW, kTop, kBarW, kBarH, solids[i]);
  }

  // Per-channel ramps: smooth steps show every data line; a swapped or dead
  // bit shows as a jump or a repeated band.
  constexpr int kRampTop = kTop + kBarH + 10;
  constexpr int kRampH = 24;
  constexpr int kRampW = kBarW * 4;
  for (int x = 0; x < kRampW; ++x) {
    const uint8_t v = static_cast<uint8_t>(x * 255 / (kRampW - 1));
    canvas.drawFastVLine(kLeft + x, kRampTop, kRampH, canvas.color565(v, 0, 0));
    canvas.drawFastVLine(kLeft + x, kRampTop + kRampH, kRampH, canvas.color565(0, v, 0));
    canvas.drawFastVLine(kLeft + x, kRampTop + kRampH * 2, kRampH, canvas.color565(0, 0, v));
    canvas.drawFastVLine(kLeft + x, kRampTop + kRampH * 3, kRampH, canvas.color565(v, v, v));
  }
}

void drawCircles() {
  // Outer 1 px ring at the very edge: it should be fully visible and evenly thick.
  canvas.drawCircle(kCx, kCy, kW / 2 - 1, TFT_WHITE);
  canvas.drawCircle(kCx, kCy, kW / 2 - 6, TFT_YELLOW);
  for (int r = 60; r < kW / 2 - 20; r += 60) {
    canvas.drawCircle(kCx, kCy, r, TFT_DARKGREY);
  }
  canvas.drawFastHLine(0, kCy, kW, TFT_DARKGREY);
  canvas.drawFastVLine(kCx, 0, kH, TFT_DARKGREY);
  // N/S/W/E edge dots (red/green/blue/white): all four should be fully visible.
  canvas.fillCircle(kCx, 12, 6, TFT_RED);
  canvas.fillCircle(kCx, kH - 13, 6, TFT_GREEN);
  canvas.fillCircle(12, kCy, 6, TFT_BLUE);
  canvas.fillCircle(kW - 13, kCy, 6, TFT_WHITE);
}

void drawText() {
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  const int heights[] = {16, 33, 48};
  int y = 420;
  for (int height : heights) {
    displayFontApply(canvas, height);
    canvas.drawString("Plane Radar 720", kCx, y);
    y += canvas.fontHeight() + 6;
  }
  char info[64];
  snprintf(info, sizeof(info), "pclk %ld Hz  bounce %u px",
           static_cast<long>(config::kPanelPclkHz),
           static_cast<unsigned>(config::kPanelBounceBufferPx));
  displayFontApply(canvas, 20);
  canvas.drawString(info, kCx, 110);
}

void drawStatusLine() {
  canvas.fillRect(kStatusX, kStatusY, kStatusW, kStatusH, TFT_BLACK);
  canvas.drawRect(kStatusX, kStatusY, kStatusW, kStatusH, TFT_DARKGREY);
  char line[80];
  snprintf(line, sizeof(line), "fetch ok %lu fail %lu  UP %d DOWN %d",
           static_cast<unsigned long>(s_fetch_ok), static_cast<unsigned long>(s_fetch_fail),
           s_up_level ? 1 : 0, s_down_level ? 1 : 0);
  displayFontApply(canvas, 20);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.setTextColor(TFT_WHITE, TFT_BLACK);
  canvas.drawString(line, kCx, kStatusY + kStatusH / 2);
}

/** Draws every frame in full (the back buffer holds the frame before last) and logs times. */
void drawTestScreen(const char* reason) {
  const unsigned long t0 = millis();
  canvas.fillScreen(TFT_BLACK);
  drawCircles();
  drawColorBars();
  drawText();
  drawStatusLine();
  const unsigned long t1 = millis();
  displayPresent();
  Serial.printf("panel test: %s frame: draw %lu ms, present %lu ms\n", reason, t1 - t0,
                millis() - t1);
}

/** Logs raw expander levels on change; runs as the adsb poll hook too. */
void pollButtons() {
  if (millis() - s_last_button_poll_ms < kButtonPollMs) {
    return;
  }
  s_last_button_poll_ms = millis();
  const bool up = panelReadButton(config::kExpanderPinButtonUp);
  const bool down = panelReadButton(config::kExpanderPinButtonDown);
  if (up != s_up_level || down != s_down_level) {
    Serial.printf("buttons: UP(pin %u)=%d DOWN(pin %u)=%d\n", config::kExpanderPinButtonUp,
                  up ? 1 : 0, config::kExpanderPinButtonDown, down ? 1 : 0);
    s_up_level = up;
    s_down_level = down;
    drawTestScreen("button");
  }
}

void startWifi() {
  // The disconnect reason tells radio trouble (e.g. NO_AP_FOUND, timeouts) from bad credentials.
  WiFi.onEvent(
      [](WiFiEvent_t, WiFiEventInfo_t info) {
        const auto reason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
        Serial.printf("panel test: Wi-Fi disconnected, reason %u (%s)\n",
                      static_cast<unsigned>(reason), WiFi.disconnectReasonName(reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.mode(WIFI_STA);
#if defined(PANEL_TEST_WIFI_SSID) && defined(PANEL_TEST_WIFI_PASS)
  WiFi.begin(PANEL_TEST_WIFI_SSID, PANEL_TEST_WIFI_PASS);
#else
  wifi_config_t saved = {};
  esp_wifi_get_config(WIFI_IF_STA, &saved);
  char ssid[sizeof(saved.sta.ssid) + 1] = {};
  memcpy(ssid, saved.sta.ssid, sizeof(saved.sta.ssid));
  Serial.printf("panel test: saved SSID \"%s\"\n", ssid[0] != '\0' ? ssid : "(none)");
  WiFi.begin();  // credentials saved by WiFiManager
#endif
  Serial.println("panel test: Wi-Fi connecting (saved or build-flag credentials)");
}

void fetchOnce() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("panel test: Wi-Fi not connected (status %d)\n", WiFi.status());
    return;
  }
  const unsigned long t0 = millis();
  const bool ok = services::adsb::fetchUpdate(services::location::lat(),
                                              services::location::lon(), kFetchRadiusKm);
  const unsigned long t1 = millis();
  ok ? ++s_fetch_ok : ++s_fetch_fail;
  Serial.printf("panel test: fetch %s in %lu ms, %u aircraft\n", ok ? "ok" : "FAILED",
                t1 - t0, static_cast<unsigned>(services::adsb::aircraftCount()));

  // Full frame under Wi-Fi load: watch for tearing or jitter here.
  drawTestScreen("fetch");
  // The bounce buffers live in internal RAM; HTTPS needs headroom there too.
  Serial.printf("panel test: internal heap %u KB free, %u KB min, %u KB largest\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

}  // namespace

void panelTestRun() {
  Serial.println("PANEL TEST build");
  Serial.printf("panel test: PSRAM %u KB free after framebuffers\n",
                static_cast<unsigned>(ESP.getFreePsram() / 1024));

  s_up_level = panelReadButton(config::kExpanderPinButtonUp);
  s_down_level = panelReadButton(config::kExpanderPinButtonDown);
  Serial.printf("buttons idle: UP(pin %u)=%d DOWN(pin %u)=%d. Press each to see its level.\n",
                config::kExpanderPinButtonUp, s_up_level ? 1 : 0,
                config::kExpanderPinButtonDown, s_down_level ? 1 : 0);
  drawTestScreen("first");

  services::location::init();
  services::adsb::setPollFn(pollButtons);
  startWifi();

  for (;;) {
    pollButtons();
    if (millis() - s_last_fetch_ms >= kFetchIntervalMs) {
      s_last_fetch_ms = millis();
      fetchOnce();
    }
    delay(1);
  }
}

#endif  // PANEL_TEST
