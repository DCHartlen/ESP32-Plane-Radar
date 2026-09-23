#include "hardware/display.h"

#include <Arduino.h>

#include <algorithm>

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

const uint16_t* canvasPixels() {
  return static_cast<const uint16_t*>(canvas.getBuffer());
}

}  // namespace

void displayInit() {
  if (!panelInit()) {
    haltWithError("panel init failed (expander or RGB framebuffer)");
  }

  canvas.setPsram(true);
  canvas.setColorDepth(16);
  if (!canvas.createSprite(config::kDisplayWidth, config::kDisplayHeight)) {
    haltWithError("canvas alloc failed: check PSRAM (qio_opi, BOARD_HAS_PSRAM)");
  }
  canvas.setRotation(config::kDisplayRotate180 ? 2 : 0);
  canvas.setTextWrap(false);
  canvas.fillScreen(TFT_BLACK);
  displayFontInit();
  displayPresent();
}

void displayPresent() {
  panelPushBe565(canvasPixels(), 0, 0, config::kDisplayWidth, config::kDisplayHeight);
}

void displayPresentRect(int x, int y, int w, int h) {
  // Rect is in canvas drawing coordinates; map it to buffer coordinates when rotated.
  if (config::kDisplayRotate180) {
    x = config::kDisplayWidth - (x + w);
    y = config::kDisplayHeight - (y + h);
  }
  const int x0 = std::max(x, 0);
  const int y0 = std::max(y, 0);
  const int x1 = std::min(x + w, config::kDisplayWidth);
  const int y1 = std::min(y + h, config::kDisplayHeight);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  // The canvas row stride is the full width, so push one row at a time.
  const uint16_t* pixels = canvasPixels();
  for (int row = y0; row < y1; ++row) {
    panelPushBe565(pixels + row * config::kDisplayWidth + x0, x0, row, x1 - x0, 1);
  }
}
