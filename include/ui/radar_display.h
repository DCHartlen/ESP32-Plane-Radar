#pragma once

namespace ui {

/**
 * Render the full frame (grid, runways, labels, aircraft) into `canvas` and present it.
 * `log` prints the frame and phase timing lines; pass false for the in-between animation
 * frames so the serial log isn't flooded at 4 Hz.
 */
void radarDisplayDraw(bool log = true);

/**
 * The last frame showed an aircraft that dead reckoning is still moving, so a redraw would
 * change the picture. False when the data was stale or nothing is moving (or all are capped).
 */
bool radarDisplayAnimating();

}  // namespace ui
