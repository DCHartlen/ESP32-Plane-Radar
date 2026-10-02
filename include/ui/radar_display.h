#pragma once

namespace ui {

/** Render the full frame (grid, runways, labels, aircraft) into `canvas` and present it. */
void radarDisplayDraw();

/** Called after a fetch with new aircraft. Same as radarDisplayDraw(): every frame is full. */
void radarDisplayRefreshAircraft();

}  // namespace ui
