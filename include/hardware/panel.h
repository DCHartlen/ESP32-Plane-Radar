#pragma once

#include <cstdint>

/**
 * Qualia RGB panel + PCA9554 expander. The only module that includes Arduino_GFX, which
 * it uses for the expander and the NV3052C init sequence (bit-banged SPI over the
 * expander). The RGB output is an esp_lcd panel with two PSRAM framebuffers: one is on
 * screen while the next frame is drawn into the other, and presenting swaps them.
 */

/** Expander, panel init sequence, RGB peripheral, backlight on. False on failure (logged). */
bool panelInit();

void panelBacklight(bool on);

/** The framebuffer that isn't on screen (RGB565, native byte order). Draw the next frame here. */
uint16_t* panelBackBuffer();

/**
 * Show the back buffer from the next refresh, wait until the old one is off screen, then
 * make the old one the back buffer. It still holds the frame before last.
 */
void panelSwap();

/**
 * Restart the scan-out from line 0 at the next VSYNC. Fixes a shifted image after a
 * flash write stalled the bounce-buffer refill (the refill ISR isn't IRAM-safe).
 */
void panelResync();

/** Raw level (true = high) of an expander pin. Expander access is guarded by a mutex. */
bool panelReadButton(uint8_t pca_pin);
