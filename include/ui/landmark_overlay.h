#pragma once

#include <LovyanGFX.hpp>

#include <cstdint>

namespace ui::landmark {

/**
 * Brings the landmark layer (shores, islands, rivers, border) up to date with the range and
 * radar location. The lines only change with those, so they're drawn once into a PSRAM
 * layer the size of the frame, and each row records the span of columns its lines cover.
 * Call before the frame is cleared. Leaves no lines when the overlay is off, there's no
 * generated data or none near the location, or the layer can't be allocated.
 */
void prepareLandmarks(LGFX_Sprite& frame);

/**
 * Buffer row y of the landmark layer, with the columns [*left, *right) that may hold lines
 * (multiples of 16 px; everything else in the row is background), or nullptr if the row has
 * none. The frame clear copies these spans instead of writing background there.
 */
const uint16_t* landmarkRow(int y, int* left, int* right);

}  // namespace ui::landmark
