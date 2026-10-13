#pragma once

#include <LovyanGFX.hpp>

namespace ui::landmark {

/**
 * Copies the landmark lines (shores, islands, rivers, border) into the frame, which must
 * have just been cleared to the background colour. The lines only change with the range
 * and the radar location, so they're drawn once into a PSRAM layer and only the rows they
 * cover are copied each frame. Draws nothing when the overlay is off, there's no generated
 * data or none near the location, or the layer can't be allocated.
 */
void drawLandmarks(LGFX_Sprite& frame);

}  // namespace ui::landmark
