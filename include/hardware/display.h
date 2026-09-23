#pragma once

#include <LovyanGFX.hpp>

/** Full-screen 16-bit drawing surface in PSRAM (RGB565, stored byte-swapped). */
extern LGFX_Sprite canvas;

/** Panel + canvas + font. Halts with a serial error if either can't be set up. */
void displayInit();

/** Copy the whole canvas to the panel. */
void displayPresent();

/** Copy one canvas rectangle to the panel (clipped to the screen). */
void displayPresentRect(int x, int y, int w, int h);
