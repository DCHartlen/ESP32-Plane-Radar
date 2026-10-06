#include "ui/radar_geometry.h"

#include <cmath>

#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"

namespace ui::radar {

namespace {

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

}  // namespace

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km, float* dist_km) {
  // Longitude degrees shrink toward the poles; scale by cos(latitude) so
  // east-west distance isn't overstated away from the equator.
  const float center_lat_rad = static_cast<float>(services::location::lat()) * kDegToRad;
  *dx_km = static_cast<float>(lon - services::location::lon()) * kKmPerDeg *
           cosf(center_lat_rad);
  *dy_km = static_cast<float>(lat - services::location::lat()) * kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

void kmOffsetToScreen(float dx_km, float dy_km, int* out_x, int* out_y) {
  const float px_per_km = static_cast<float>(kGridOuterRadius) / rangeCurrent().outer_km;
  *out_x = kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  kmOffsetToScreen(dx_km, dy_km, out_x, out_y);
}

int distSqFromCenter(int x, int y) {
  const int dx = x - kCenterX;
  const int dy = y - kCenterY;
  return dx * dx + dy * dy;
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  // Step back from the far end in 1% increments (5% left visible gaps at 720 px).
  constexpr int kSteps = 100;
  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  for (int step = 1; step < kSteps; ++step) {
    const float t = 1.0f - static_cast<float>(step) / kSteps;
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
  }
  *x1 = x0;
  *y1 = y0;
}

}  // namespace ui::radar
