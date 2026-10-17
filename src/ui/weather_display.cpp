#include "ui/weather_display.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock.h"
#include "services/weather_client.h"
#include "ui/canvas_pixels.h"
#include "ui/landmark_overlay.h"
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

#ifdef WEATHER_FAKE_RAIN
/**
 * Test builds only (env qualia_fake_rain): paints synthetic rain over a decoded frame, so
 * the copy and the landmark blend can be timed in a storm on a dry day. Overlapping round
 * cells drift a little each time step, so the replay moves. WEATHER_FAKE_RAIN=2 also fills
 * every dry pixel with the lightest colour: rain on the whole screen, the upper bound.
 */
void paintFakeRain(LGFX_Sprite& sprite, time_t frame_time) {
  // Roughly GeoMet's rain-rate ramp, light to heavy (never pure black: that's transparent).
  static const uint8_t kRamp[][3] = {
      {152, 203, 254}, {0, 153, 254}, {0, 254, 102}, {0, 203, 0},   {0, 153, 0},
      {0, 102, 0},     {254, 254, 0}, {254, 203, 0}, {254, 153, 0}, {254, 102, 0},
      {254, 0, 0},     {254, 2, 152}, {153, 51, 204}, {102, 0, 153},
  };
  constexpr int kRampSize = sizeof(kRamp) / sizeof(kRamp[0]);
  constexpr int kCells = 16;
  constexpr float kDryBelow = 0.15f;
  uint16_t colors[kRampSize];
  for (int i = 0; i < kRampSize; ++i) {
    colors[i] = sprite.color565(kRamp[i][0], kRamp[i][1], kRamp[i][2]);
  }
  // Fixed cells (same seed every frame), moved along their own drift by the time step.
  struct Cell {
    float x, y, r2, amp;
  };
  Cell cells[kCells];
  uint32_t seed = 12345;
  const auto next = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 16777216.0f;  // [0, 1)
  };
  const float step = static_cast<float>(frame_time / 360 % 1000);
  for (Cell& c : cells) {
    const float r = 25.0f + 70.0f * next();
    c.x = fmodf(kImagePx * next() + step * (4.0f * next() - 1.0f) + 10 * kImagePx, kImagePx);
    c.y = fmodf(kImagePx * next() + step * (4.0f * next() - 1.0f) + 10 * kImagePx, kImagePx);
    c.r2 = r * r;
    c.amp = 0.4f + 0.9f * next();
  }
  auto* buf = static_cast<uint16_t*>(sprite.getBuffer());
  for (int y = 0; y < kImagePx; ++y) {
    for (int x = 0; x < kImagePx; ++x) {
      float v = 0.0f;
      for (const Cell& c : cells) {
        const float dx = x - c.x;
        const float dy = y - c.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < c.r2) {
          v += c.amp * (1.0f - d2 / c.r2);
        }
      }
      uint16_t* p = buf + static_cast<size_t>(y) * kImagePx + x;
      if (v < kDryBelow) {
        if (WEATHER_FAKE_RAIN == 2 && *p == kTransparentKey) {
          *p = colors[0];
        }
        continue;
      }
      const int i = std::min(kRampSize - 1, static_cast<int>((v - kDryBelow) / 1.3f * kRampSize));
      *p = colors[i];
    }
  }
}
#endif

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
#ifdef WEATHER_FAKE_RAIN
  if (ok) {
    paintFakeRain(image.sprite, services::weather::frameTime(index));
  }
#endif
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
 * transparent ones so the background and landmarks show through. Where rain covers a
 * landmark line, the line is blended back over it (kLandmarkOverRainAlpha): a second pass
 * over each row's landmark span only, so rows without lines cost nothing extra.
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
  const uint16_t background = radar::kColorBackground;
  for (int iy = 0; iy < span; ++iy) {
    const uint16_t* row = src + static_cast<size_t>(first + iy) * kImagePx + first;
    // The row's first and last rain pixel; the blend pass looks only between them.
    int rain_first = 0;
    while (rain_first < span && row[rain_first] == kTransparentKey) {
      ++rain_first;
    }
    if (rain_first == span) {
      continue;  // dry row
    }
    int rain_last = span - 1;
    while (row[rain_last] == kTransparentKey) {
      --rain_last;
    }
    // Those as buffer columns [rain_left, rain_right).
    int rain_left = rain_first * scale;
    int rain_right = (rain_last + 1) * scale;
    if (config::kDisplayRotate180) {
      const int left = kFbW - rain_right;
      rain_right = kFbW - rain_left;
      rain_left = left;
    }
    for (int dy = 0; dy < scale; ++dy) {
      const int y = iy * scale + dy;
      for (int ix = rain_first; ix <= rain_last; ++ix) {
        const uint16_t p = row[ix];
        if (p == kTransparentKey) {
          continue;
        }
        uint16_t* out = fb + fbIndex(ix * scale, y);
        for (int dx = 0; dx < scale; ++dx, out += step) {
          *out = p;
        }
      }
      // The clear copied the layer's span into this row, so a pixel there that no longer
      // matches the layer, where the layer has a line, is rain over that line. Both are
      // buffer rows in the same column order.
      const int buffer_row = config::kDisplayRotate180 ? kFbH - 1 - y : y;
      int lines_left = 0;
      int lines_right = 0;
      const uint16_t* lines = landmark::landmarkRow(buffer_row, &lines_left, &lines_right);
      if (lines == nullptr) {
        continue;
      }
      uint16_t* const out_row = fb + static_cast<size_t>(buffer_row) * kFbW;
      const int to = std::min(lines_right, rain_right);
      for (int col = std::max(lines_left, rain_left); col < to; ++col) {
        const uint16_t line = lines[col];
        const uint16_t under = out_row[col];
        if (line != background && under != line) {
          out_row[col] = blend565(under, line, radar::kLandmarkOverRainAlpha);
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

/**
 * What the live frame shows from the weather task: the newest image, or the reason there is
 * none. Older images arriving (the backfill after boot) don't change it.
 */
struct LiveKey {
  uint32_t newest_id = 0;
  services::weather::State state = services::weather::State::Pending;
  char error[sizeof(services::weather::Status::error)] = "";
  bool operator==(const LiveKey& o) const {
    return newest_id == o.newest_id &&
           (newest_id != 0 || (state == o.state && strcmp(error, o.error) == 0));
  }
};

LiveKey liveKeyOf(const services::weather::Status& st) {
  LiveKey key;
  key.newest_id = st.frame_count > 0 ? services::weather::frameId(st.frame_count - 1) : 0;
  key.state = st.state;
  snprintf(key.error, sizeof(key.error), "%s", st.error);
  return key;
}

/** The key of the live frame last drawn; nothing drawn yet never matches. */
LiveKey s_drawn_key;
bool s_drawn_any = false;

}  // namespace

bool weatherDisplayLiveChanged() {
  return !s_drawn_any || !(liveKeyOf(services::weather::status()) == s_drawn_key);
}

void weatherDisplayDraw(bool log) {
  const services::weather::Status st = services::weather::status();
  s_drawn_key = liveKeyOf(st);
  s_drawn_any = true;
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
