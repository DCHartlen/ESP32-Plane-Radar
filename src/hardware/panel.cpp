#include "hardware/panel.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "config.h"

namespace {

using namespace config;

Arduino_XCA9554SWSPI s_expander(kExpanderPinTftReset, kExpanderPinTftCs, kExpanderPinTftSck,
                                kExpanderPinTftMosi, &Wire, kExpanderAddr);

Arduino_ESP32RGBPanel s_rgb_panel(
    kPanelPinDe, kPanelPinVsync, kPanelPinHsync, kPanelPinPclk,
    kPanelPinR[0], kPanelPinR[1], kPanelPinR[2], kPanelPinR[3], kPanelPinR[4],
    kPanelPinG[0], kPanelPinG[1], kPanelPinG[2], kPanelPinG[3], kPanelPinG[4], kPanelPinG[5],
    kPanelPinB[0], kPanelPinB[1], kPanelPinB[2], kPanelPinB[3], kPanelPinB[4],
    kPanelHsyncPolarity, kPanelHsyncFrontPorch, kPanelHsyncPulseWidth, kPanelHsyncBackPorch,
    kPanelVsyncPolarity, kPanelVsyncFrontPorch, kPanelVsyncPulseWidth, kPanelVsyncBackPorch,
    kPanelPclkActiveNeg, kPanelPclkHz, /*useBigEndian=*/false,
    /*de_idle_high=*/0, /*pclk_idle_high=*/0, kPanelBounceBufferPx);

// Rotation 0 keeps draw16bitBeRGBBitmap on its fast path; 180° is done in the canvas instead.
Arduino_RGB_Display s_gfx(kDisplayWidth, kDisplayHeight, &s_rgb_panel, /*rotation=*/0,
                          /*auto_flush=*/true, &s_expander, GFX_NOT_DEFINED,
                          hd40015c40_init_operations, sizeof(hd40015c40_init_operations));

SemaphoreHandle_t s_expander_mutex = nullptr;
bool s_ready = false;

class ExpanderLock {
 public:
  ExpanderLock() { xSemaphoreTake(s_expander_mutex, portMAX_DELAY); }
  ~ExpanderLock() { xSemaphoreGive(s_expander_mutex); }
};

bool expanderPresent() {
  Wire.beginTransmission(kExpanderAddr);
  return Wire.endTransmission() == 0;
}

}  // namespace

bool panelInit() {
  if (s_ready) {
    return true;
  }
  s_expander_mutex = xSemaphoreCreateMutex();
  Wire.begin(kI2cPinSda, kI2cPinScl, kI2cHz);

  if (!expanderPresent()) {
    Serial.printf("panel: no PCA9554 at 0x%02X\n", kExpanderAddr);
    return false;
  }

  ExpanderLock lock;
  if (!s_gfx.begin()) {
    Serial.println("panel: RGB framebuffer alloc failed");
    return false;
  }
  s_gfx.fillScreen(0x0000);

  s_expander.pinMode(kExpanderPinButtonUp, INPUT);
  s_expander.pinMode(kExpanderPinButtonDown, INPUT);
  s_expander.pinMode(kExpanderPinBacklight, OUTPUT);
  s_expander.digitalWrite(kExpanderPinBacklight, HIGH);

  Serial.printf("panel: %dx%d, pclk %ld Hz, bounce %u px\n", kDisplayWidth, kDisplayHeight,
                static_cast<long>(kPanelPclkHz), static_cast<unsigned>(kPanelBounceBufferPx));
  s_ready = true;
  return true;
}

void panelBacklight(bool on) {
  if (!s_ready) {
    return;
  }
  ExpanderLock lock;
  s_expander.digitalWrite(kExpanderPinBacklight, on ? HIGH : LOW);
}

void panelPushBe565(const uint16_t* buf, int x, int y, int w, int h) {
  if (!s_ready) {
    return;
  }
  // Arduino_GFX takes a non-const pointer but only reads the bitmap.
  s_gfx.draw16bitBeRGBBitmap(x, y, const_cast<uint16_t*>(buf), w, h);
}

bool panelReadButton(uint8_t pca_pin) {
  if (!s_ready) {
    return false;
  }
  ExpanderLock lock;
  return s_expander.digitalRead(pca_pin) == HIGH;
}
