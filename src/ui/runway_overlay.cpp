#include "ui/runway_overlay.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "data/large_airports.h"
#include "hardware/display_font.h"
#include "ui/radar_geometry.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::runway {
namespace {

using radar::clipPointToOuterRing;
using radar::distSqFromCenter;
using radar::latLonToScreen;
using radar::offsetKmFromCenter;

constexpr size_t kMaxAirportLabels = 32;
constexpr size_t kMaxRunwaySegments = 128;

/** Parallel runways closer than this (centerline to centerline) draw as one. */
constexpr float kMergeMaxSeparationPx = radar::kRunwayLineWidthPx * 2.5f;
/** "Parallel" means headings within about 4 degrees. */
constexpr float kMergeMaxSin = 0.07f;

/** Screen-space bounds of an airport's drawn (clipped) runways. */
struct Box {
  int min_x;
  int min_y;
  int max_x;
  int max_y;
};

/** A runway's on-screen segment, clipped to the outer ring. */
struct Segment {
  float x0;
  float y0;
  float x1;
  float y1;
  uint16_t airport_idx;
  bool merged;  // folded into another segment; not drawn
};

bool s_in_range[data::large_airports::kAirportCount];
bool s_label_pending[data::large_airports::kAirportCount];
Box s_runway_box[data::large_airports::kAirportCount];
Segment s_segments[kMaxRunwaySegments];

float e7ToDeg(int32_t e7) { return static_cast<float>(e7) * 1e-7f; }

bool segmentIntersectsDisc(int x0, int y0, int x1, int y1) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kGridOuterRadius;
  const int r_sq = r * r;

  if (distSqFromCenter(x0, y0) <= r_sq || distSqFromCenter(x1, y1) <= r_sq) {
    return true;
  }

  // 64-bit: at 720 px, b² alone reaches ~2e10 and overflows int32.
  const int64_t dx = x1 - x0;
  const int64_t dy = y1 - y0;
  const int64_t fx = x0 - cx;
  const int64_t fy = y0 - cy;
  const int64_t a = dx * dx + dy * dy;
  if (a == 0) {
    return false;
  }
  const int64_t b = 2 * (fx * dx + fy * dy);
  const int64_t c = fx * fx + fy * fy - r_sq;
  const int64_t disc = b * b - 4 * a * c;
  if (disc < 0) {
    return false;
  }
  const double sqrt_disc = sqrt(static_cast<double>(disc));
  const double inv2a = 1.0 / (2.0 * static_cast<double>(a));
  const double t0 = (-static_cast<double>(b) - sqrt_disc) * inv2a;
  const double t1 = (-static_cast<double>(b) + sqrt_disc) * inv2a;
  return (t0 >= 0.0 && t0 <= 1.0) || (t1 >= 0.0 && t1 <= 1.0);
}

void expandBox(Box* box, int x, int y) {
  if (x < box->min_x) box->min_x = x;
  if (x > box->max_x) box->max_x = x;
  if (y < box->min_y) box->min_y = y;
  if (y > box->max_y) box->max_y = y;
}

bool rectInsideOuterRing(int left, int top, int w, int h) {
  const int r_sq = radar::kGridOuterRadius * radar::kGridOuterRadius;
  const int right = left + w - 1;
  const int bottom = top + h - 1;
  return distSqFromCenter(left, top) <= r_sq &&
         distSqFromCenter(right, top) <= r_sq &&
         distSqFromCenter(left, bottom) <= r_sq &&
         distSqFromCenter(right, bottom) <= r_sq;
}

/**
 * Places a w×h label just outside the runway box, preferring the side that
 * faces away from the screen center, then the two perpendicular sides, then
 * the inward side. Falls back to the outward side if none fits in the ring.
 */
void placeLabelBesideBox(const Box& box, int w, int h, int* left, int* top) {
  const int gap = radar::kRunwayLabelGapPx;
  const int bx = (box.min_x + box.max_x) / 2;
  const int by = (box.min_y + box.max_y) / 2;
  const int dx = bx - radar::kCenterX;
  const int dy = by - radar::kCenterY;

  // Sides: 0 = right, 1 = below, 2 = left, 3 = above.
  auto rectForSide = [&](int side, int* l, int* t) {
    switch (side) {
      case 0: *l = box.max_x + gap;     *t = by - h / 2;          break;
      case 1: *l = bx - w / 2;          *t = box.max_y + gap;     break;
      case 2: *l = box.min_x - gap - w; *t = by - h / 2;          break;
      default: *l = bx - w / 2;         *t = box.min_y - gap - h; break;
    }
  };

  const bool horizontal = abs(dx) > abs(dy);
  const int outward = horizontal ? (dx >= 0 ? 0 : 2) : (dy >= 0 ? 1 : 3);
  const int side_a = horizontal ? (dy >= 0 ? 1 : 3) : (dx >= 0 ? 0 : 2);
  const int order[] = {outward, side_a, (side_a + 2) % 4, (outward + 2) % 4};

  for (int side : order) {
    rectForSide(side, left, top);
    if (rectInsideOuterRing(*left, *top, w, h)) {
      return;
    }
  }
  rectForSide(outward, left, top);
}

void drawAirportLabel(lgfx::LGFXBase& gfx, const char* ident, const Box& box) {
  constexpr int kPadX = px(2);
  constexpr int kPadY = px(1);
  const int w = gfx.textWidth(ident) + kPadX * 2;
  const int h = gfx.fontHeight() + kPadY * 2;

  int left = 0;
  int top = 0;
  placeLabelBesideBox(box, w, h, &left, &top);

  gfx.fillRect(left, top, w, h, radar::kColorBackground);
  gfx.setTextDatum(textdatum_t::top_left);
  gfx.setTextColor(radar::kColorRunwayLabel, radar::kColorBackground);
  gfx.drawString(ident, left + kPadX, top + kPadY);
}

bool runwaySegment(const data::large_airports::Runway& rw, Segment* seg) {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  latLonToScreen(e7ToDeg(rw.le_lat_e7), e7ToDeg(rw.le_lon_e7), &x0, &y0);
  latLonToScreen(e7ToDeg(rw.he_lat_e7), e7ToDeg(rw.he_lon_e7), &x1, &y1);

  if (!segmentIntersectsDisc(x0, y0, x1, y1)) {
    return false;
  }

  clipPointToOuterRing(x0, y0, &x1, &y1);
  clipPointToOuterRing(x1, y1, &x0, &y0);
  *seg = {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1),
          static_cast<float>(y1), rw.airport_idx, false};
  return true;
}

/**
 * Folds `other` into `seg` when the two are near-parallel and so close on
 * screen that their strokes would blur together (e.g. CYYZ 06L/06R, 300 m
 * apart). The result spans both along the runway axis, on their midline.
 */
bool mergeIfClose(Segment* seg, const Segment& other) {
  const float dx = seg->x1 - seg->x0;
  const float dy = seg->y1 - seg->y0;
  const float len = sqrtf(dx * dx + dy * dy);
  const float odx = other.x1 - other.x0;
  const float ody = other.y1 - other.y0;
  const float olen = sqrtf(odx * odx + ody * ody);
  if (len < 1.0f || olen < 1.0f) {
    return false;
  }
  const float ux = dx / len;
  const float uy = dy / len;
  if (fabsf(ux * ody - uy * odx) / olen > kMergeMaxSin) {
    return false;
  }

  // Perpendicular offset of the other segment's midpoint from this one's axis.
  const float nx = -uy;
  const float ny = ux;
  const float mx = (other.x0 + other.x1) * 0.5f - seg->x0;
  const float my = (other.y0 + other.y1) * 0.5f - seg->y0;
  const float offset = mx * nx + my * ny;
  if (fabsf(offset) > kMergeMaxSeparationPx) {
    return false;
  }

  // Positions along this segment's axis; require the two to overlap.
  const float t0 = (other.x0 - seg->x0) * ux + (other.y0 - seg->y0) * uy;
  const float t1 = (other.x1 - seg->x0) * ux + (other.y1 - seg->y0) * uy;
  const float lo = fminf(0.0f, fminf(t0, t1));
  const float hi = fmaxf(len, fmaxf(t0, t1));
  if (fmaxf(t0, t1) < 0.0f || fminf(t0, t1) > len) {
    return false;
  }

  const float ox = seg->x0 + nx * offset * 0.5f;
  const float oy = seg->y0 + ny * offset * 0.5f;
  int x0 = static_cast<int>(lroundf(ox + ux * lo));
  int y0 = static_cast<int>(lroundf(oy + uy * lo));
  int x1 = static_cast<int>(lroundf(ox + ux * hi));
  int y1 = static_cast<int>(lroundf(oy + uy * hi));
  clipPointToOuterRing(x0, y0, &x1, &y1);
  clipPointToOuterRing(x1, y1, &x0, &y0);
  seg->x0 = static_cast<float>(x0);
  seg->y0 = static_cast<float>(y0);
  seg->x1 = static_cast<float>(x1);
  seg->y1 = static_cast<float>(y1);
  return true;
}

void mergeCloseParallels(Segment* segs, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (segs[i].merged) {
      continue;
    }
    for (size_t j = i + 1; j < count; ++j) {
      if (!segs[j].merged && segs[j].airport_idx == segs[i].airport_idx &&
          mergeIfClose(&segs[i], segs[j])) {
        segs[j].merged = true;
      }
    }
  }
}

}  // namespace

void drawLargeAirportRunways(lgfx::LGFXBase& gfx) {
  if (!radar::showRunways()) {
    return;
  }
  const float radius_km = radar::fetchRadiusKm();

  uint16_t label_airports[kMaxAirportLabels];
  size_t label_count = 0;

  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    s_in_range[i] = false;
    s_label_pending[i] = false;
  }

  size_t seg_count = 0;
  for (size_t i = 0; i < data::large_airports::kRunwayCount &&
                     seg_count < kMaxRunwaySegments;
       ++i) {
    const auto& rw = data::large_airports::kRunways[i];
    const uint16_t ap_idx = rw.airport_idx;
    if (!s_in_range[ap_idx]) {
      const auto& ap = data::large_airports::kAirports[ap_idx];
      float dx_km = 0.0f;
      float dy_km = 0.0f;
      float dist_km = 0.0f;
      offsetKmFromCenter(e7ToDeg(ap.lat_e7), e7ToDeg(ap.lon_e7), &dx_km, &dy_km,
                         &dist_km);
      s_in_range[ap_idx] = (dist_km <= radius_km);
    }
    if (s_in_range[ap_idx] && runwaySegment(rw, &s_segments[seg_count])) {
      ++seg_count;
    }
  }

  mergeCloseParallels(s_segments, seg_count);

  for (size_t i = 0; i < seg_count; ++i) {
    const Segment& seg = s_segments[i];
    if (seg.merged) {
      continue;
    }
    const uint16_t ap_idx = seg.airport_idx;
    gfx.drawWideLine(static_cast<int>(seg.x0), static_cast<int>(seg.y0),
                     static_cast<int>(seg.x1), static_cast<int>(seg.y1),
                     radar::kRunwayLineHalfWidth, radar::kColorRunway);
    if (!s_label_pending[ap_idx]) {
      s_runway_box[ap_idx] = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};
    }
    expandBox(&s_runway_box[ap_idx], static_cast<int>(seg.x0),
              static_cast<int>(seg.y0));
    expandBox(&s_runway_box[ap_idx], static_cast<int>(seg.x1),
              static_cast<int>(seg.y1));
    if (!s_label_pending[ap_idx] && label_count < kMaxAirportLabels) {
      s_label_pending[ap_idx] = true;
      label_airports[label_count++] = ap_idx;
    }
  }

  if (label_count == 0) {
    return;
  }

  displayFontApply(gfx, radar::kRunwayLabelHeightPx);
  for (size_t i = 0; i < label_count; ++i) {
    const uint16_t ap_idx = label_airports[i];
    drawAirportLabel(gfx, data::large_airports::kAirports[ap_idx].ident,
                     s_runway_box[ap_idx]);
  }
}

}  // namespace ui::runway
