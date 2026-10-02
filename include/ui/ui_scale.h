#pragma once

#include "config.h"

namespace ui {

/** Element sizes are written in pixels of the original 240 px screen. */
constexpr float kDesignSize = 240.0f;
/** 1.0 = original proportions (about 3× the old physical size); ~0.33 = old physical size. */
constexpr float kUiDensity = 0.67f;
constexpr float kElementScale =
    static_cast<float>(config::kDisplayWidth) / kDesignSize * kUiDensity;

/** A design-pixel size scaled to the screen, rounded; never collapses a nonzero size to 0. */
constexpr int px(float design_px) {
  const float v = design_px * kElementScale;
  const int r = static_cast<int>(v < 0.0f ? v - 0.5f : v + 0.5f);
  if (design_px > 0.0f && r < 1) {
    return 1;
  }
  if (design_px < 0.0f && r > -1) {
    return -1;
  }
  return r;
}

/** Unrounded scaled size, for line half-widths. */
constexpr float pxF(float design_px) { return design_px * kElementScale; }

}  // namespace ui
