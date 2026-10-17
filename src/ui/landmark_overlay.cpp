#include "ui/landmark_overlay.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "config.h"
#include "data/landmarks.h"
#include "services/radar_location.h"
#include "ui/radar_frame.h"
#include "ui/radar_geometry.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::landmark {
namespace {

using data::landmarks::Kind;

constexpr int kW = config::kDisplayWidth;
constexpr int kH = config::kDisplayHeight;
/** Copied spans start and end on 16 px (one 32-byte cache line). */
constexpr int kAlignPx = 16;
constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;
/**
 * Points closer than this to the last drawn one are skipped, so zoomed-out ranges don't
 * pay for detail they can't show (OSM shores have a point every few metres). Rivers skip
 * more at wide ranges (kRiverFarMinSegmentPx).
 */
constexpr float kMinSegmentPx = 1.5f;

/**
 * What the layer was drawn for; any change redraws it. Keyed by distance rather than preset
 * index, so a range both modes share (25 km) reuses the layer. The background is part of it
 * because each mode has its own and the lines are blended onto it.
 */
struct LayerKey {
  float outer_km;
  double lat;
  double lon;
  uint16_t background;
  bool operator==(const LayerKey& o) const {
    return outer_km == o.outer_km && lat == o.lat && lon == o.lon &&
           background == o.background;
  }
};

/**
 * Columns [left, right) of a buffer row that may hold landmark pixels; empty if left >= right.
 * Outside its span every row of the layer is background.
 */
struct RowSpan {
  int16_t left;
  int16_t right;
};

LGFX_Sprite s_layer;
/** The background the layer is filled with outside the lines. */
uint16_t s_layer_background = 0;
bool s_alloc_failed = false;
bool s_layer_valid = false;
LayerKey s_key{};
RowSpan s_spans[kH];
/** The layer holds lines for s_key; false while nothing is in view. */
bool s_has_lines = false;

struct LineStyle {
  float half_width;
  uint16_t color;
  bool dashed;
  float min_segment_px;
};

/** How far the current range is into the river fade: 0 up close, 1 at kRiverFadeFarKm. */
float riverFadeAmount() {
  const float t = (radar::rangeCurrent().outer_km - radar::kRiverFadeNearKm) /
                  (radar::kRiverFadeFarKm - radar::kRiverFadeNearKm);
  return std::clamp(t, 0.0f, 1.0f);
}

LineStyle styleFor(Kind kind) {
  switch (kind) {
    case Kind::River: {
      const float t = riverFadeAmount();
      const radar::Rgb rgb = radar::mixRgb({radar::kRiverR, radar::kRiverG, radar::kRiverB},
                                           radar::backgroundRgb(), t * radar::kRiverFarFade);
      return {radar::kRiverLineHalfWidth, s_layer.color565(rgb.r, rgb.g, rgb.b), false,
              kMinSegmentPx + t * (radar::kRiverFarMinSegmentPx - kMinSegmentPx)};
    }
    case Kind::Island:
      return {radar::kIslandLineHalfWidth,
              s_layer.color565(radar::kShoreR, radar::kShoreG, radar::kShoreB), false,
              kMinSegmentPx};
    case Kind::Shore:
      return {radar::kShoreLineHalfWidth,
              s_layer.color565(radar::kShoreR, radar::kShoreG, radar::kShoreB), false,
              kMinSegmentPx};
    case Kind::Border:
    default:
      return {radar::kBorderLineHalfWidth,
              s_layer.color565(radar::kBorderR, radar::kBorderG, radar::kBorderB), true,
              kMinSegmentPx};
  }
}

bool allocateLayer(LGFX_Sprite& frame) {
  if (s_layer.getBuffer() != nullptr) {
    return true;
  }
  if (s_alloc_failed) {
    return false;
  }
  s_layer.setPsram(true);
  s_layer.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
  if (s_layer.createSprite(kW, kH) == nullptr) {
    Serial.println("Landmarks: no PSRAM for the layer; overlay off");
    s_alloc_failed = true;
    return false;
  }
  // Same rotation as the frame, so buffer rows line up for the copy.
  s_layer.setRotation(frame.getRotation());
  s_layer.fillScreen(radar::kColorBackground);
  s_layer_background = radar::kColorBackground;
  for (RowSpan& span : s_spans) {
    span = {kW, 0};
  }
  return true;
}

/** Widens a buffer row's span to cover screen columns [x_left, x_right] on screen row y. */
void markScreenRow(int y, float x_left, float x_right) {
  if (y < 0 || y >= kH) {
    return;
  }
  int left = std::max(0, static_cast<int>(floorf(x_left)));
  int right = std::min(kW, static_cast<int>(ceilf(x_right)) + 1);
  if (left >= right) {
    return;
  }
  int row = y;
  if (config::kDisplayRotate180) {
    // Rotation 2 turns the buffer 180 degrees (see canvas_pixels.h).
    row = kH - 1 - y;
    const int mirrored_left = kW - right;
    right = kW - left;
    left = mirrored_left;
  }
  RowSpan& span = s_spans[row];
  span.left = static_cast<int16_t>(std::min<int>(span.left, left));
  span.right = static_cast<int16_t>(std::max<int>(span.right, right));
}

/**
 * Marks the rows a drawWideLine of half-width hw covers. For each row it takes the part of
 * the segment within the pen's reach of that row, so a long diagonal doesn't mark its whole
 * bounding box.
 */
void markSegment(float x0, float y0, float x1, float y1, float hw) {
  const float pad = hw + 1.5f;  // the pen, its anti-aliased edge, and rounding
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  const int top = static_cast<int>(floorf(std::min(y0, y1) - pad));
  const int bottom = static_cast<int>(ceilf(std::max(y0, y1) + pad));
  for (int y = std::max(0, top); y <= std::min(kH - 1, bottom); ++y) {
    float xa = x0;
    float xb = x1;
    if (fabsf(dy) > 1e-3f) {
      const float t0 = std::clamp((y - pad - y0) / dy, 0.0f, 1.0f);
      const float t1 = std::clamp((y + pad - y0) / dy, 0.0f, 1.0f);
      xa = x0 + dx * t0;
      xb = x0 + dx * t1;
    }
    markScreenRow(y, std::min(xa, xb) - pad, std::max(xa, xb) + pad);
  }
}

/**
 * Trims the segment to the part inside the outer grid ring. False if none of it is inside.
 */
bool clipToOuterRing(float* x0, float* y0, float* x1, float* y1) {
  const float r = static_cast<float>(radar::kGridOuterRadius);
  const float ax = *x0 - radar::kCenterX;
  const float ay = *y0 - radar::kCenterY;
  const float dx = *x1 - *x0;
  const float dy = *y1 - *y0;
  const float a = dx * dx + dy * dy;
  const float c = ax * ax + ay * ay - r * r;
  if (a < 1e-6f) {
    return c <= 0.0f;
  }
  // |a0 + t*d| = r; the segment is t in [0, 1].
  const float b = ax * dx + ay * dy;
  const float disc = b * b - a * c;
  if (disc <= 0.0f) {
    return false;
  }
  const float root = sqrtf(disc);
  const float t_in = std::max(0.0f, (-b - root) / a);
  const float t_out = std::min(1.0f, (-b + root) / a);
  if (t_in >= t_out) {
    return false;
  }
  const float sx = *x0;
  const float sy = *y0;
  *x0 = sx + dx * t_in;
  *y0 = sy + dy * t_in;
  *x1 = sx + dx * t_out;
  *y1 = sy + dy * t_out;
  return true;
}

void drawClipped(float x0, float y0, float x1, float y1, const LineStyle& style) {
  if (clipToOuterRing(&x0, &y0, &x1, &y1)) {
    s_layer.drawWideLine(x0, y0, x1, y1, style.half_width, style.color);
    markSegment(x0, y0, x1, y1, style.half_width);
  }
}

/**
 * Dashes along one segment. `phase` is the distance into the current dash-gap period and
 * carries over to the next segment, so dashes run on round corners. drawWideLine adds a
 * round cap of half_width at each end, so each dash's centre line is shortened by that.
 */
void drawDashed(float x0, float y0, float x1, float y1, const LineStyle& style,
                float* phase) {
  const float len = hypotf(x1 - x0, y1 - y0);
  if (len < 1e-3f) {
    return;
  }
  const float ux = (x1 - x0) / len;
  const float uy = (y1 - y0) / len;
  const float period = radar::kBorderDashPx + radar::kBorderGapPx;
  float s = 0.0f;
  while (s < len) {
    const bool in_dash = *phase < radar::kBorderDashPx;
    const float to_edge = (in_dash ? radar::kBorderDashPx : period) - *phase;
    const float step = std::min(to_edge, len - s);
    if (in_dash) {
      // Shorten the part of the dash that's in this segment by the caps at the dash's own ends.
      const float dash_start = s - *phase;
      const float a = std::max(s, dash_start + style.half_width);
      const float b = std::min(s + step, dash_start + radar::kBorderDashPx - style.half_width);
      if (b > a) {
        drawClipped(x0 + ux * a, y0 + uy * a, x0 + ux * b, y0 + uy * b, style);
      }
    }
    s += step;
    *phase = fmodf(*phase + step, period);
  }
}

/** The box the screen covers (past the outer ring, to the edge), in dataset units (1e-5 degree). */
struct ViewBox {
  int32_t min_lat;
  int32_t max_lat;
  int32_t min_lon;
  int32_t max_lon;
};

ViewBox currentViewBox() {
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  const float half_km = radar::screenHalfWidthKm(radar::rangeCurrent());
  const float dlat_deg = half_km / kKmPerDeg;
  const float dlon_deg = half_km / (kKmPerDeg * cosf(static_cast<float>(lat) * kDegToRad));
  const auto units = [](double deg) {
    return static_cast<int32_t>(lround(deg * data::landmarks::kUnitsPerDeg));
  };
  return {units(lat - dlat_deg), units(lat + dlat_deg), units(lon - dlon_deg),
          units(lon + dlon_deg)};
}

bool chunkInView(const data::landmarks::Chunk& chunk, const ViewBox& view) {
  return chunk.max_lat >= view.min_lat && chunk.min_lat <= view.max_lat &&
         chunk.max_lon >= view.min_lon && chunk.min_lon <= view.max_lon;
}

bool anyChunkInView(const ViewBox& view) {
  for (size_t i = 0; i < data::landmarks::kChunkCount; ++i) {
    if (chunkInView(data::landmarks::kChunks[i], view)) {
      return true;
    }
  }
  return false;
}

/** Draws the chunk's segments, merging sub-pixel ones; *end_lat / *end_lon get its last point. */
void drawChunk(const data::landmarks::Chunk& chunk, const LineStyle& style, float* phase,
               int32_t* end_lat, int32_t* end_lon) {
  constexpr float kDegPerUnit = 1.0f / data::landmarks::kUnitsPerDeg;
  int32_t lat = chunk.lat0;
  int32_t lon = chunk.lon0;
  float x0 = 0.0f;
  float y0 = 0.0f;
  const auto project = [](int32_t lat_u, int32_t lon_u, float* x, float* y) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    radar::offsetKmFromCenter(lat_u * kDegPerUnit, lon_u * kDegPerUnit, &dx_km, &dy_km,
                              &dist_km);
    radar::kmOffsetToScreenF(dx_km, dy_km, x, y);
  };
  project(lat, lon, &x0, &y0);
  for (uint16_t i = 1; i < chunk.point_count; ++i) {
    const data::landmarks::Delta& d = data::landmarks::kDeltas[chunk.first_delta + i - 1];
    lat += d.dlat;
    lon += d.dlon;
    float x1 = 0.0f;
    float y1 = 0.0f;
    project(lat, lon, &x1, &y1);
    const bool last = i + 1 == chunk.point_count;
    if (!last && fabsf(x1 - x0) < style.min_segment_px &&
        fabsf(y1 - y0) < style.min_segment_px) {
      continue;
    }
    if (style.dashed) {
      drawDashed(x0, y0, x1, y1, style, phase);
    } else {
      drawClipped(x0, y0, x1, y1, style);
    }
    x0 = x1;
    y0 = y1;
  }
  *end_lat = lat;
  *end_lon = lon;
}

/** Paints the old spans back to background and empties them, so the layer is all background. */
void clearSpans() {
  uint16_t* buf = static_cast<uint16_t*>(s_layer.getBuffer());
  for (int y = 0; y < kH; ++y) {
    RowSpan& span = s_spans[y];
    if (span.left < span.right) {
      uint16_t* row = buf + static_cast<size_t>(y) * kW;
      std::fill(row + span.left, row + span.right, radar::kColorBackground);
    }
    span = {kW, 0};
  }
}

/** Rounds the spans out to 16 px (one 32-byte cache line); true if any row has lines. */
bool alignSpans() {
  bool any = false;
  for (RowSpan& span : s_spans) {
    if (span.left < span.right) {
      span.left = static_cast<int16_t>(span.left / kAlignPx * kAlignPx);
      span.right = static_cast<int16_t>(
          std::min(kW, (span.right + kAlignPx - 1) / kAlignPx * kAlignPx));
      any = true;
    }
  }
  return any;
}

void redrawLayer(const ViewBox& view) {
  const unsigned long t0 = millis();
  clearSpans();

  size_t drawn = 0;
  float phase = 0.0f;
  int32_t prev_end_lat = INT32_MIN;
  int32_t prev_end_lon = INT32_MIN;
  // Chunks are sorted by kind, which is also the drawing order.
  for (size_t i = 0; i < data::landmarks::kChunkCount; ++i) {
    const data::landmarks::Chunk& chunk = data::landmarks::kChunks[i];
    if (!chunkInView(chunk, view)) {
      prev_end_lat = INT32_MIN;
      continue;
    }
    // A chunk that continues the previous one keeps its dash phase.
    if (chunk.lat0 != prev_end_lat || chunk.lon0 != prev_end_lon) {
      phase = 0.0f;
    }
    drawChunk(chunk, styleFor(chunk.kind), &phase, &prev_end_lat, &prev_end_lon);
    ++drawn;
  }

  s_has_lines = alignSpans();
  Serial.printf("Landmarks: drew %u chunks in %lu ms\n", static_cast<unsigned>(drawn),
                millis() - t0);
}

}  // namespace

void prepareLandmarks(LGFX_Sprite& frame) {
  if (!radar::showLandmarks()) {
    return;
  }
  const LayerKey key{radar::rangeCurrent().outer_km, services::location::lat(),
                     services::location::lon(), radar::kColorBackground};
  if (s_layer_valid && key == s_key) {
    return;
  }
  // With nothing in view (no generated data, or far from every site), skip the layer and
  // its 1 MB of PSRAM.
  const ViewBox view = currentViewBox();
  s_has_lines = false;
  if (anyChunkInView(view) && allocateLayer(frame)) {
    if (s_layer_background != radar::kColorBackground) {
      // The other mode's background: lines blend onto what's under them, and new spans can
      // land anywhere.
      s_layer.fillScreen(radar::kColorBackground);
      s_layer_background = radar::kColorBackground;
    }
    redrawLayer(view);
  }
  s_key = key;
  s_layer_valid = true;
}

const uint16_t* landmarkRow(int y, int* left, int* right) {
  if (!s_has_lines || !radar::showLandmarks() || y < 0 || y >= kH) {
    return nullptr;
  }
  const RowSpan span = s_spans[y];
  if (span.left >= span.right) {
    return nullptr;
  }
  *left = span.left;
  *right = span.right;
  return static_cast<const uint16_t*>(s_layer.getBuffer()) + static_cast<size_t>(y) * kW;
}

}  // namespace ui::landmark
