#pragma once

namespace ui {

/**
 * Weather mode's frame: the newest radar image from services::weather under the grid, the
 * runways and the clock, with the image's age above the clock. The image is decoded here, on
 * the loop task (core 1), whenever a newer one arrives. `log` prints the frame timing.
 */
void weatherDisplayDraw(bool log = true);

/**
 * Starts the replay of the held radar images, oldest first; false (and nothing changes) with
 * fewer than two. Each step of it is drawn by weatherDisplayReplayStep().
 */
bool weatherDisplayReplayStart();

/** A replay is running. */
bool weatherDisplayReplaying();

/** Draws the next replay frame once the current one has been up long enough; then the live one. */
void weatherDisplayReplayStep();

/** Stops a replay without drawing (e.g. on a mode switch). */
void weatherDisplayReplayStop();

}  // namespace ui
