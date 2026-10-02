#pragma once

#include <LovyanGFX.hpp>

/**
 * Full-screen 16-bit drawing surface (RGB565, native byte order). It points at the
 * panel's back framebuffer in PSRAM, which holds the frame before last, so every
 * frame must be drawn in full.
 */
extern LGFX_Sprite canvas;

/** Panel + canvas + font. Halts with a serial error if the panel can't be set up. */
void displayInit();

/** Show the canvas on the next refresh, then point it at the other framebuffer. */
void displayPresent();

/** Re-align the panel's scan-out after a flash write may have shifted the image. */
void displayResync();
