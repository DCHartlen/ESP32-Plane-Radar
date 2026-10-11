#include "ui/landmark_overlay.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "config.h"
#include "data/landmarks.h"
#include "services/radar_location.h"
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

/** What the layer was drawn for; any change redraws it. */
struct LayerKey {
  uint8_t range_index;
  double lat;
  double lon;
  bool operator==(const LayerKey& o) const {
    return range_index == o.range_index && lat == o.lat && lon == o.lon;
  }
};

/** Columns [left, right) of a buffer row that hold landmark pixels; left == right if none. */
struct RowSpan {
  uint16_t left;
  uint16_t right;
};

LGFX_Sprite s_layer;
bool s_alloc_failed = false;
bool s_layer_valid = false;
LayerKey s_key{};
RowSpan s_spans[kH];
bool s_any_rows = false;

struct LineStyle {
  float half_width;
  uint16_t color;
  bool dashed;
};

LineStyle styleFor(Kind kind) {
  switch (kind) {
    case Kind::River:
      return {radar::kRiverLineHalfWidth,
              s_layer.color565(radar::kRiverR, radar::kRiverG, radar::kRiverB), false};
    case Kind::Island:
      return {radar::kIslandLineHalfWidth,
              s_layer.color565(radar::kShoreR, radar::kShoreG, radar::kShoreB), false};
    case Kind::Shore:
      return {radar::kShoreLineHalfWidth,
              s_layer.color565(radar::kShoreR, radar::kShoreG, radar::kShoreB), false};
    case Kind::Border:
    default:
      return {radar::kBorderLineHalfWidth,
              s_layer.color565(radar::kBorderR, radar::kBorderG, radar::kBorderB), true};
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
  return true;
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

/** True if the chunk's box overlaps the box (1e-5 degree units) the outer ring covers. */
bool chunkInView(const data::landmarks::Chunk& chunk, int32_t min_lat, int32_t max_lat,
                 int32_t min_lon, int32_t max_lon) {
  return chunk.max_lat >= min_lat && chunk.min_lat <= max_lat && chunk.max_lon >= min_lon &&
         chunk.min_lon <= max_lon;
}

/** Draws the chunk's segments; *end_lat / *end_lon get its last point. */
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

/** Finds each buffer row's span of non-background pixels. */
void findRowSpans(uint16_t background) {
  const uint16_t* buf = static_cast<const uint16_t*>(s_layer.getBuffer());
  s_any_rows = false;
  for (int y = 0; y < kH; ++y) {
    const uint16_t* row = buf + static_cast<size_t>(y) * kW;
    int left = 0;
    while (left < kW && row[left] == background) {
      ++left;
    }
    if (left == kW) {
      s_spans[y] = {0, 0};
      continue;
    }
    int right = kW;
    while (row[right - 1] == background) {
      --right;
    }
    left = left / kAlignPx * kAlignPx;
    right = std::min(kW, (right + kAlignPx - 1) / kAlignPx * kAlignPx);
    s_spans[y] = {static_cast<uint16_t>(left), static_cast<uint16_t>(right)};
    s_any_rows = true;
  }
}

void redrawLayer() {
  const unsigned long t0 = millis();
  s_layer.fillScreen(radar::kColorBackground);

  // The box the outer ring covers, in dataset units, to skip chunks off screen.
  const double lat = services::location::lat();
  const double lon = services::location::lon();
  const float outer_km = radar::rangeCurrent().outer_km;
  const float dlat_deg = outer_km / kKmPerDeg;
  const float dlon_deg = outer_km / (kKmPerDeg * cosf(static_cast<float>(lat) * kDegToRad));
  const auto units = [](double deg) {
    return static_cast<int32_t>(lround(deg * data::landmarks::kUnitsPerDeg));
  };
  const int32_t min_lat = units(lat - dlat_deg);
  const int32_t max_lat = units(lat + dlat_deg);
  const int32_t min_lon = units(lon - dlon_deg);
  const int32_t max_lon = units(lon + dlon_deg);

  size_t drawn = 0;
  float phase = 0.0f;
  int32_t prev_end_lat = INT32_MIN;
  int32_t prev_end_lon = INT32_MIN;
  // Chunks are sorted by kind, which is also the drawing order.
  for (size_t i = 0; i < data::landmarks::kChunkCount; ++i) {
    const data::landmarks::Chunk& chunk = data::landmarks::kChunks[i];
    if (!chunkInView(chunk, min_lat, max_lat, min_lon, max_lon)) {
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

  findRowSpans(radar::kColorBackground);
  Serial.printf("Landmarks: drew %u chunks in %lu ms\n", static_cast<unsigned>(drawn),
                millis() - t0);
}

bool locationInRegion(double lat, double lon) {
  // Margin of the widest fetch radius, so lines just inside the region still show.
  constexpr double kMarginDeg = 1.0;
  return lat >= data::landmarks::kRegionSouth - kMarginDeg &&
         lat <= data::landmarks::kRegionNorth + kMarginDeg &&
         lon >= data::landmarks::kRegionWest - kMarginDeg &&
         lon <= data::landmarks::kRegionEast + kMarginDeg;
}

}  // namespace

void drawLandmarks(LGFX_Sprite& frame) {
  if (!radar::showLandmarks()) {
    return;
  }
  const LayerKey key{radar::rangeIndex(), services::location::lat(),
                     services::location::lon()};
  if (!locationInRegion(key.lat, key.lon) || !allocateLayer(frame)) {
    return;
  }
  if (!s_layer_valid || !(key == s_key)) {
    redrawLayer();
    s_key = key;
    s_layer_valid = true;
  }
  if (!s_any_rows) {
    return;
  }

  uint16_t* dst = static_cast<uint16_t*>(frame.getBuffer());
  const uint16_t* src = static_cast<const uint16_t*>(s_layer.getBuffer());
  if (dst == nullptr) {
    return;
  }
  for (int y = 0; y < kH; ++y) {
    const RowSpan span = s_spans[y];
    if (span.right > span.left) {
      const size_t offset = static_cast<size_t>(y) * kW + span.left;
      memcpy(dst + offset, src + offset, (span.right - span.left) * sizeof(uint16_t));
    }
  }
}

}  // namespace ui::landmark
