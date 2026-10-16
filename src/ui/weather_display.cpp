#include "ui/weather_display.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock.h"
#include "services/weather_client.h"
#include "ui/canvas_pixels.h"
#include "ui/radar_frame.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"

namespace ui {

namespace {

using services::weather::kImagePx;

/** Decoded pixels left at this value are transparent. GeoMet's rain palette has no pure black. */
constexpr uint16_t kTransparentKey = 0x0000;

// The image covers the screen at the widest preset at 2x; narrower presets crop its centre.
constexpr int kBaseScale = config::kDisplayWidth / kImagePx;
static_assert(config::kDisplayWidth == kBaseScale * kImagePx &&
                  config::kDisplayHeight == config::kDisplayWidth,
              "the scaled copy assumes a square screen a whole multiple of the image");

/** A 360 px image in PSRAM, in the canvas's pixel format, and which frame it holds. */
struct Image {
  LGFX_Sprite sprite;
  uint32_t id = 0;  // services::weather::frameId(); 0 = nothing decoded
  time_t time = 0;
  bool failed = false;
};

Image s_live;
Image s_replay;
/** Scratch copy of the PNG being decoded (PSRAM), so the fetch task isn't held up. */
uint8_t* s_png = nullptr;
size_t s_png_capacity = 0;
unsigned long s_last_decode_ms = 0;

/** Replay: the frames to show (oldest first), the next one, and when the last went up. */
struct Replay {
  bool active = false;
  uint32_t ids[config::kWeatherHistoryFrames] = {};
  time_t times[config::kWeatherHistoryFrames] = {};
  size_t count = 0;
  size_t next = 0;
  unsigned long shown_ms = 0;
};
Replay s_replay_state;

bool ensureSprite(LGFX_Sprite& sprite) {
  if (sprite.getBuffer() != nullptr) {
    return true;
  }
  sprite.setPsram(true);
  // Same pixel format as canvas, so the copy is a plain 16-bit store.
  sprite.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
  return sprite.createSprite(kImagePx, kImagePx) != nullptr;
}

/** Index of the frame with this id, or -1. */
int frameIndexOf(uint32_t id) {
  const services::weather::Status st = services::weather::status();
  for (size_t i = 0; i < st.frame_count; ++i) {
    if (services::weather::frameId(i) == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

/**
 * Decodes frame `index` into `image` (on this task, core 1: the panel's refill interrupt runs
 * here too, so the decode never competes with it for PSRAM). False if it isn't there or
 * doesn't decode.
 */
bool decodeFrame(size_t index, Image& image) {
  const uint32_t id = services::weather::frameId(index);
  if (id == 0) {
    return false;
  }
  if (image.id == id && !image.failed) {
    return true;
  }
  if (!ensureSprite(image.sprite)) {
    Serial.println("Weather: no PSRAM for the radar image");
    return false;
  }
  const size_t len = services::weather::copyFramePng(index, &s_png, &s_png_capacity);
  if (len == 0) {
    return false;
  }
  const unsigned long t0 = millis();
  image.sprite.fillSprite(kTransparentKey);
  const bool ok = image.sprite.drawPng(s_png, len, 0, 0);
  s_last_decode_ms = millis() - t0;
  image.id = id;
  image.time = services::weather::frameTime(index);
  image.failed = !ok;
  if (!ok) {
    Serial.println("Weather: PNG decode failed");
  }
  return ok;
}

/** Screen pixels per image pixel at the current range: 2 at the widest, then 4, 8. */
int currentScale() {
  const float widest =
      radar::kWeatherRangePresets[radar::kWeatherRangePresetCount - 1].ring3_km;
  const int zoom = static_cast<int>(lroundf(widest / radar::rangeCurrent().ring3_km));
  return kBaseScale * std::max(1, zoom);
}

/**
 * Copies the centre of `image` into canvas, each image pixel a scale x scale block, skipping
 * transparent ones so the background and landmarks show through.
 */
void copyScaled(const LGFX_Sprite& image, int scale) {
  const auto* src = static_cast<const uint16_t*>(image.getBuffer());
  auto* fb = static_cast<uint16_t*>(canvas.getBuffer());
  if (src == nullptr || fb == nullptr) {
    return;
  }
  const int span = config::kDisplayWidth / scale;  // image pixels across the screen
  const int first = (kImagePx - span) / 2;
  // Rotation 2 turns the buffer 180 degrees (canvas_pixels.h): a screen row runs backwards.
  const int step = config::kDisplayRotate180 ? -1 : 1;
  for (int iy = 0; iy < span; ++iy) {
    const uint16_t* row = src + static_cast<size_t>(first + iy) * kImagePx + first;
    for (int ix = 0; ix < span; ++ix) {
      const uint16_t p = row[ix];
      if (p == kTransparentKey) {
        continue;
      }
      const int x = ix * scale;
      for (int dy = 0; dy < scale; ++dy) {
        uint16_t* out = fb + fbIndex(x, iy * scale + dy);
        for (int dx = 0; dx < scale; ++dx, out += step) {
          *out = p;
        }
      }
    }
  }
}

/** The age label's line height: the scale label's size, smaller than the tags. */
constexpr int kLabelHeightPx = radar::kCardinalLabelHeightPx - radar::kScaleBelowCardinalPx;

/** A line of outlined text centred on (kCenterX, center_y). */
void drawLabel(const char* text, uint16_t color, int center_y) {
  radar::drawOutlinedText(text, kLabelHeightPx, radar::kCenterX, center_y,
                          textdatum_t::middle_center, color);
}

/**
 * The label sits just under the clock on the south spoke. The clock's digits reach about
 * 0.32 line heights below its centre, plus the outline (radar_frame's layoutClock).
 */
int labelCenterY() {
  const int clock_center_y =
      radar::kCenterY + static_cast<int>(radar::kGridOuterRadius * radar::kClockCenterRingFraction);
  const int clock_bottom =
      clock_center_y + radar::kClockLabelHeightPx * 32 / 100 + radar::kClockOutlinePx;
  return clock_bottom + px(2) + kLabelHeightPx / 2;
}

/** Local HH:MM of a UTC time, or HH:MMZ before the clock is synced. */
void formatImageTime(time_t t, char* out, size_t len) {
  struct tm tm_local;
  if (services::clock::timeValid()) {
    localtime_r(&t, &tm_local);
    strftime(out, len, "%H:%M", &tm_local);
  } else {
    gmtime_r(&t, &tm_local);
    strftime(out, len, "%H:%MZ", &tm_local);
  }
}

/** "Radar 6 min ago" (alert colour once stale), or the image's time before SNTP. */
void drawAgeLabel(time_t image_time) {
  char text[40];
  uint16_t color = radar::kColorLabel;
  if (services::clock::timeValid()) {
    const long age_s = std::max(0L, static_cast<long>(time(nullptr) - image_time));
    const long age_min = age_s / 60;
    if (age_min < 120) {
      snprintf(text, sizeof(text), "Radar %ld min ago", age_min);
    } else {
      snprintf(text, sizeof(text), "Radar %ld h ago", age_min / 60);
    }
    if (static_cast<unsigned long>(age_s) * 1000UL > config::kWeatherStaleAfterMs) {
      color = radar::kColorAlert;
    }
  } else {
    char hhmm[12];
    formatImageTime(image_time, hhmm, sizeof(hhmm));
    snprintf(text, sizeof(text), "Radar %s", hhmm);
  }
  drawLabel(text, color, labelCenterY());
}

/** "Replay 20:12  -42 min" for a replay frame; the newest one says "latest". */
void drawReplayLabel(time_t frame_time, time_t newest_time) {
  char hhmm[12];
  formatImageTime(frame_time, hhmm, sizeof(hhmm));
  char text[48];
  const long behind_min = static_cast<long>(newest_time - frame_time) / 60;
  if (behind_min <= 0) {
    snprintf(text, sizeof(text), "Replay %s  latest", hhmm);
  } else {
    snprintf(text, sizeof(text), "Replay %s  -%ld min", hhmm, behind_min);
  }
  drawLabel(text, radar::kColorLabel, labelCenterY());
}

/** Why there's no image: waiting, no coverage, or the last error. */
void drawStatusMessage(const services::weather::Status& st) {
  using services::weather::State;
  switch (st.state) {
    case State::NoCoverage:
      radar::drawMessageBox("No radar coverage", "Weather radar covers North America",
                            radar::messageBoxCenterY());
      break;
    case State::Error:
      radar::drawMessageBox("Radar unavailable", st.error, radar::messageBoxCenterY());
      break;
    case State::Pending:
    case State::Ok:
    default:
      radar::drawMessageBox("Waiting for radar", "", radar::messageBoxCenterY());
      break;
  }
}

/**
 * One full frame: background and landmarks, the radar image (`image`, or none), the grid and
 * runways over it, then the label and the clock.
 */
void drawFrame(const Image* image, time_t label_time, time_t newest_time, bool replay,
               const services::weather::Status& st, bool log, const char* what) {
  const unsigned long t0 = millis();
  radar::applyPalette();
  radar::clearFrame();
  const unsigned long t_clear = millis();
  if (image != nullptr) {
    copyScaled(image->sprite, currentScale());
  }
  const unsigned long t_copy = millis();
  radar::drawRings();
  radar::drawCrosshairs();
  runway::drawLargeAirportRunways(canvas);
  radar::drawHome();
  radar::drawCardinalLabels();
  radar::drawScaleLabel();
  if (image == nullptr) {
    drawStatusMessage(st);
  } else if (replay) {
    drawReplayLabel(label_time, newest_time);
  } else {
    drawAgeLabel(label_time);
  }
  radar::ClockLayout clock{};
  if (radar::layoutClock(&clock)) {
    radar::drawClock(clock);
  }
  canvas.setTextDatum(textdatum_t::top_left);
  const unsigned long t1 = millis();
  displayPresent();
  if (log) {
    Serial.printf("Weather %s frame: draw %lu ms (clear %lu, copy %lu), present %lu ms\n", what,
                  t1 - t0, t_clear - t0, t_copy - t_clear, millis() - t1);
  }
}

}  // namespace

void weatherDisplayDraw(bool log) {
  const services::weather::Status st = services::weather::status();
  const Image* image = nullptr;
  if (st.frame_count > 0) {
    const uint32_t had = s_live.id;
    if (decodeFrame(st.frame_count - 1, s_live)) {
      image = &s_live;
      if (log && s_live.id != had) {
        Serial.printf("Weather: decoded the newest image in %lu ms; %u held\n",
                      s_last_decode_ms, static_cast<unsigned>(st.frame_count));
      }
    }
  }
  drawFrame(image, image != nullptr ? image->time : 0, st.newest_time, false, st, log, "live");
}

bool weatherDisplayReplayStart() {
  const services::weather::Status st = services::weather::status();
  if (st.frame_count < 2) {
    return false;
  }
  Replay& r = s_replay_state;
  r = Replay{};
  for (size_t i = 0; i < st.frame_count && i < config::kWeatherHistoryFrames; ++i) {
    r.ids[r.count] = services::weather::frameId(i);
    r.times[r.count] = services::weather::frameTime(i);
    ++r.count;
  }
  r.active = true;
  r.shown_ms = millis() - config::kWeatherReplayFrameMs;  // the first frame goes up now
  Serial.printf("Weather: replaying %u images\n", static_cast<unsigned>(r.count));
  return true;
}

bool weatherDisplayReplaying() { return s_replay_state.active; }

void weatherDisplayReplayStop() { s_replay_state.active = false; }

void weatherDisplayReplayStep() {
  Replay& r = s_replay_state;
  if (!r.active) {
    return;
  }
  const bool at_end = r.next >= r.count;
  const unsigned long hold =
      at_end ? config::kWeatherReplayLastFrameMs : config::kWeatherReplayFrameMs;
  if (millis() - r.shown_ms < hold) {
    return;
  }
  if (at_end) {
    r.active = false;
    weatherDisplayDraw(false);
    return;
  }
  r.shown_ms = millis();
  const size_t k = r.next++;
  const int index = frameIndexOf(r.ids[k]);  // a new image may have shifted the list
  if (index < 0 || !decodeFrame(static_cast<size_t>(index), s_replay)) {
    return;  // gone or broken: skip it
  }
  const services::weather::Status st = services::weather::status();
  drawFrame(&s_replay, r.times[k], r.times[r.count - 1], true, st, false, "replay");
}

}  // namespace ui
