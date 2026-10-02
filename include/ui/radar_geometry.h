#pragma once

namespace ui::radar {

/**
 * Lat/lon to screen projection shared by the aircraft and runway layers.
 * Equirectangular: dx = Δlon·kKmPerDeg·cos(center_lat), dy = Δlat·kKmPerDeg.
 */

/** Offset of (lat, lon) from the radar center in km (x east, y north) and its distance. */
void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km, float* dist_km);

/** Screen position of (lat, lon) at the current range; north is up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y);

int distSqFromCenter(int x, int y);

/** Pulls (x1, y1) back toward (x0, y0) until it lies inside the outer grid ring. */
void clipPointToOuterRing(int x0, int y0, int* x1, int* y1);

}  // namespace ui::radar
