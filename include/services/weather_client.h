#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace services::weather {

/**
 * Environment Canada's GeoMet radar composite (RADAR_1KM_RRAI: rain rate, 1 km cells, a new
 * time step every 6 min, covering Canada and the US). A task on core 0 keeps the last
 * kWeatherHistoryFrames time steps as PNGs in PSRAM, fetched at one fixed extent around the
 * radar centre, so range changes and the replay need no network. It only fetches; the
 * display decodes on core 1, so no bulk PSRAM work runs beside the panel's refill.
 */

/** Width and height of every image (px). The display scales it up to the screen. */
constexpr int kImagePx = 360;

/** What the last attempt to update the history found. */
enum class State : uint8_t {
  Pending,      // nothing fetched yet for this location
  Ok,
  NoCoverage,   // the centre is outside the layer's coverage
  Error,        // the last request failed; see Status::error
};

struct Status {
  State state = State::Pending;
  /** Short reason for State::Error, e.g. "HTTP 503" or "not a PNG". */
  char error[40] = "";
  /** Time steps held, oldest first; their valid times are frameTime(). */
  size_t frame_count = 0;
  /** Valid time of the newest frame (UTC epoch seconds); 0 if none. */
  time_t newest_time = 0;
};

/**
 * Starts the fetch task (core 0, stack in PSRAM). It checks for a new time step every
 * config::kWeatherCheckIntervalMs while enabled and a request is set. Call once from setup().
 */
bool startFetchTask();

/**
 * Where to fetch: the radar centre, and half the width of the square image in km. A change
 * empties the history and fetches right away. Cheap to call often.
 */
void setRequest(double center_lat, double center_lon, float half_width_km);

/** Fetch only while enabled (Wi-Fi up, weather mode). Turning it on checks right away. */
void setEnabled(bool enabled);

/** Goes up each time the history or the status changes: time to redraw. */
uint32_t publishCount();

Status status();

/** Valid time (UTC epoch seconds) of frame i, oldest first; 0 if there's no such frame. */
time_t frameTime(size_t i);

/**
 * Identifies frame i's image, oldest first; 0 if there's no such frame. Unlike its time, it
 * changes when a new location brings the same time step, so a decoded copy can be matched.
 */
uint32_t frameId(size_t i);

/**
 * Copies frame i's PNG (oldest first) into out, growing it as needed (PSRAM). Returns its
 * size, or 0 if there's no such frame or no memory. *capacity tracks out's size.
 */
size_t copyFramePng(size_t i, uint8_t** out, size_t* capacity);

}  // namespace services::weather
