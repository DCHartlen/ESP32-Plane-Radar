#pragma once

#include <LovyanGFX.hpp>

/** Checks the embedded VLW fonts; falls back to a scaled bitmap font if they don't load. */
bool displayFontInit();
bool displayFontIsSmooth();

/**
 * Set gfx's font so fontHeight() == height_px (line height, ascent + descent).
 * Uses the smallest embedded VLW at least that tall and scales it down, so text
 * stays sharp. Reloads only when gfx doesn't already hold that VLW.
 */
void displayFontApply(lgfx::LGFXBase& gfx, int height_px);
