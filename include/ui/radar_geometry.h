#pragma once

namespace ui::radar {

/** Screen rectangle: top-left corner and size, in px. */
struct ScreenRect {
  int left;
  int top;
  int w;
  int h;
};

/**
 * Lat/lon to screen projection shared by the aircraft and runway layers.
 * Equirectangular: dx = Δlon·kKmPerDeg·cos(center_lat), dy = Δlat·kKmPerDeg.
 */

/** Offset of (lat, lon) from the radar center in km (x east, y north) and its distance. */
void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km, float* dist_km);

/** Screen position of an offset from the radar center in km (x east, y north); north is up. */
void kmOffsetToScreen(float dx_km, float dy_km, int* out_x, int* out_y);

/** As kmOffsetToScreen, unrounded, for drawing that moves by fractions of a pixel. */
void kmOffsetToScreenF(float dx_km, float dy_km, float* out_x, float* out_y);

/** Screen position of (lat, lon) at the current range; north is up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y);

int distSqFromCenter(int x, int y);

/** Pulls (x1, y1) back toward (x0, y0) until it lies inside the outer grid ring. */
void clipPointToOuterRing(int x0, int y0, int* x1, int* y1);

/** As clipPointToOuterRing, unrounded: (x1, y1) lands exactly on the ring. (x0, y0) must be inside it. */
void clipPointToOuterRingF(float x0, float y0, float* x1, float* y1);

}  // namespace ui::radar
