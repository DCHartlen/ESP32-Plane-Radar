#pragma once

#include <cstdint>

/**
 * Qualia RGB panel + PCA9554 expander. The only module that includes Arduino_GFX:
 * it runs the NV3052C init over the expander's bit-banged SPI and owns the
 * PSRAM framebuffer that the RGB peripheral scans out.
 */

/** Expander, panel init sequence, RGB peripheral, backlight on. False on failure (logged). */
bool panelInit();

void panelBacklight(bool on);

/** Copy a contiguous w×h block of big-endian RGB565 pixels into the framebuffer at (x, y). */
void panelPushBe565(const uint16_t* buf, int x, int y, int w, int h);

/** Raw level (true = high) of an expander pin. Expander access is guarded by a mutex. */
bool panelReadButton(uint8_t pca_pin);
