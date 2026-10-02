#include "hardware/display.h"

#include <Arduino.h>

#include "config.h"
#include "hardware/display_font.h"
#include "hardware/panel.h"

LGFX_Sprite canvas;

namespace {

[[noreturn]] void haltWithError(const char* msg) {
  for (;;) {
    Serial.printf("FATAL: %s\n", msg);
    delay(2000);
  }
}

/** setBuffer keeps the rotation, font and text settings. */
void attachCanvasToBackBuffer() {
  canvas.setBuffer(panelBackBuffer(), config::kDisplayWidth, config::kDisplayHeight);
}

}  // namespace

void displayInit() {
  if (!panelInit()) {
    haltWithError("panel init failed (expander, or RGB framebuffers: check PSRAM)");
  }

  // esp_lcd scans out RGB565 in native byte order. Set the depth before the first
  // setBuffer: setBuffer's depth argument truncates rgb565_nonswapped, and setColorDepth
  // on a sprite that has a buffer allocates a new one.
  canvas.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
  attachCanvasToBackBuffer();
  canvas.setRotation(config::kDisplayRotate180 ? 2 : 0);
  canvas.setTextWrap(false);
  canvas.fillScreen(TFT_BLACK);
  displayFontInit();
  displayPresent();
}

void displayPresent() {
  panelSwap();
  attachCanvasToBackBuffer();
}

void displayResync() { panelResync(); }
