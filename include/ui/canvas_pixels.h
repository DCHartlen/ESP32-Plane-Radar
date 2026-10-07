#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

namespace ui {

// Direct framebuffer access for hot paths. LovyanGFX's per-pixel calls cost ~1 µs each;
// these write the canvas buffer (RGB565, native byte order) themselves.
constexpr int kFbW = config::kDisplayWidth;
constexpr int kFbH = config::kDisplayHeight;

/** Buffer index of canvas pixel (x, y), which must be on screen; rotation 2 turns it 180°. */
inline size_t fbIndex(int x, int y) {
  return config::kDisplayRotate180 ? static_cast<size_t>(kFbH - 1 - y) * kFbW + (kFbW - 1 - x)
                                   : static_cast<size_t>(y) * kFbW + x;
}

/** color over dst at alpha 0–255. */
inline uint16_t blend565(uint32_t dst, uint32_t color, uint32_t alpha) {
  const uint32_t keep = 255 - alpha;
  const uint32_t r = ((dst >> 11) * keep + (color >> 11) * alpha + 127) / 255;
  const uint32_t g = (((dst >> 5) & 0x3F) * keep + ((color >> 5) & 0x3F) * alpha + 127) / 255;
  const uint32_t b = ((dst & 0x1F) * keep + (color & 0x1F) * alpha + 127) / 255;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

}  // namespace ui
