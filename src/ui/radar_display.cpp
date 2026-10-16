#include "ui/radar_display.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <new>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "ui/radar_geometry.h"
#include "ui/canvas_pixels.h"
#include "ui/radar_frame.h"
#include "ui/radar_range.h"
#include "ui/radar_shapes.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"
#include "ui/tag_layout.h"

namespace ui {

namespace {

using radar::distSqFromCenter;
using radar::kmOffsetToScreenF;
using radar::mixRgb;
using radar::offsetKmFromCenter;
using radar::Rgb;

constexpr float kKmPerKnotSecond = 1.852f / 3600.0f;
constexpr float kDegToRad = 0.01745329252f;

/** Parts of a frame, timed separately for the serial log (phase D1 profiling). */
enum class Phase : uint8_t {
  Clear,  // includes redrawing the landmark layer after a zoom (logged on its own)
  Rings,
  Crosshairs,
  Runways,
  Home,
  Cardinals,
  Scale,
  ClockLayout,
  Obstacles,
  Classify,
  Rim,
  Vectors,
  Icons,
  TagPlace,
  TagDraw,
  Problem,
  Clock,
  Count,
};

constexpr const char* kPhaseNames[] = {
    "clear",    "rings", "cross",   "runways", "home",      "cardinal",
    "scale",    "clk_layout", "obst", "classify", "rim",    "vectors",
    "icons",    "tag_place",  "tag_draw", "problem", "clock",
};
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) ==
                  static_cast<size_t>(Phase::Count),
              "one name per phase");

uint32_t s_phase_us[static_cast<size_t>(Phase::Count)] = {};

/** Adds the time until the end of its scope to one phase; a phase can be timed in pieces. */
class PhaseScope {
 public:
  explicit PhaseScope(Phase phase) : phase_(phase), start_(esp_timer_get_time()) {}
  ~PhaseScope() {
    s_phase_us[static_cast<size_t>(phase_)] +=
        static_cast<uint32_t>(esp_timer_get_time() - start_);
  }

 private:
  Phase phase_;
  int64_t start_;
};

void logPhases() {
  char line[320];
  int len = snprintf(line, sizeof(line), "Radar phases (ms):");
  for (size_t i = 0; i < static_cast<size_t>(Phase::Count); ++i) {
    if (s_phase_us[i] == 0 || len >= static_cast<int>(sizeof(line))) {
      continue;
    }
    len += snprintf(line + len, sizeof(line) - len, " %s %.1f", kPhaseNames[i],
                    s_phase_us[i] / 1000.0f);
  }
  Serial.println(line);
}

/**
 * Anti-aliased line of half-width half_w, flat-ended, written straight into the canvas.
 * drawWideLine tests every pixel in a box around the line and reads each one it touches
 * back through LovyanGFX (~1.4 ms for a speed vector). This walks the major axis and, at
 * each step, fills the span across the line: covered pixels solid, the edges blended by
 * how much of them the span covers. The ends take fractional coordinates too, so a moving
 * vector slides instead of stepping a pixel at a time.
 */
void drawThickLine(float x0, float y0, float x1, float y1, float half_w, uint16_t color) {
  uint16_t* fb = static_cast<uint16_t*>(canvas.getBuffer());
  if (fb == nullptr) {
    canvas.drawWideLine(x0, y0, x1, y1, half_w, color);
    return;
  }
  const bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
  // u runs along the major axis, v across it.
  float u0 = steep ? y0 : x0;
  float v0 = steep ? x0 : y0;
  float u1 = steep ? y1 : x1;
  float v1 = steep ? x1 : y1;
  if (u0 > u1) {
    std::swap(u0, u1);
    std::swap(v0, v1);
  }
  const float du = u1 - u0;
  const float dv = v1 - v0;
  if (du < 0.01f) {  // a single point
    return;
  }
  const float slope = dv / du;
  // Measured along v, the line is wider than half_w by 1 / cos of its angle to u.
  const float half_span = half_w * sqrtf(du * du + dv * dv) / du;

  // Pixel u covers [u - 0.5, u + 0.5]; the end pixels count only the part inside [u0, u1].
  const int u_first = static_cast<int>(floorf(u0 + 0.5f));
  const int u_last = static_cast<int>(ceilf(u1 - 0.5f));
  for (int u = u_first; u <= u_last; ++u) {
    const float along = std::min(u1, u + 0.5f) - std::max(u0, u - 0.5f);
    if (along <= 0.004f) {
      continue;
    }
    const float vc = v0 + slope * (static_cast<float>(u) - u0);
    const float lo = vc - half_span;
    const float hi = vc + half_span;
    // Pixel v covers [v - 0.5, v + 0.5].
    const int v_first = static_cast<int>(floorf(lo + 0.5f));
    const int v_last = static_cast<int>(ceilf(hi - 0.5f));
    for (int v = v_first; v <= v_last; ++v) {
      const float cover =
          std::min(along, 1.0f) * (std::min(hi, v + 0.5f) - std::max(lo, v - 0.5f));
      if (cover <= 0.004f) {
        continue;
      }
      const int x = steep ? v : u;
      const int y = steep ? u : v;
      if (x < 0 || x >= kFbW || y < 0 || y >= kFbH) {
        continue;
      }
      uint16_t& px = fb[fbIndex(x, y)];
      px = cover >= 0.996f ? color
                           : blend565(px, color, static_cast<uint32_t>(cover * 255.0f + 0.5f));
    }
  }
}

/** Sunset to night sky; see kAltitudeColorStops. */
Rgb altitudeRgb(const services::adsb::Aircraft& plane) {
  using services::adsb::AltState;
  if (plane.alt_state == AltState::Ground) {
    return {radar::kAltGroundR, radar::kAltGroundG, radar::kAltGroundB};
  }
  if (plane.alt_state != AltState::Airborne) {
    return {radar::kAltUnknownR, radar::kAltUnknownG, radar::kAltUnknownB};
  }

  constexpr const radar::AltitudeColorStop* kStops = radar::kAltitudeColorStops;
  constexpr size_t kCount =
      sizeof(radar::kAltitudeColorStops) / sizeof(radar::kAltitudeColorStops[0]);
  const auto rgbOf = [](const radar::AltitudeColorStop& s) { return Rgb{s.r, s.g, s.b}; };
  const int32_t alt = plane.alt_ft;
  if (alt <= kStops[0].alt_ft) {
    return rgbOf(kStops[0]);
  }
  for (size_t i = 1; i < kCount; ++i) {
    if (alt < kStops[i].alt_ft) {
      const float t = static_cast<float>(alt - kStops[i - 1].alt_ft) /
                      static_cast<float>(kStops[i].alt_ft - kStops[i - 1].alt_ft);
      return mixRgb(rgbOf(kStops[i - 1]), rgbOf(kStops[i]), t);
    }
  }
  return rgbOf(kStops[kCount - 1]);
}

/** Icon colour, and the dimmer version of it for the speed vector. */
struct AircraftColors {
  uint16_t icon;
  uint16_t track;
};

AircraftColors aircraftColors(const services::adsb::Aircraft& plane) {
  const Rgb rgb = altitudeRgb(plane);
  const Rgb bg{radar::kBgR, radar::kBgG, radar::kBgB};
  const Rgb dim = mixRgb(rgb, bg, radar::kAircraftTrackDimming);
  return {canvas.color565(rgb.r, rgb.g, rgb.b), canvas.color565(dim.r, dim.g, dim.b)};
}

/** Icons point along the ground track (matching the speed vector), else the nose heading. */
float iconBearing(const services::adsb::Aircraft& plane, float shown_track_deg) {
  return plane.has_track ? shown_track_deg : plane.nose_deg;
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

/**
 * Rim marker from true bearing; always on screen edge (even if target is 50+ km away).
 * Unrounded, so a marker sliding round the rim doesn't step.
 */
bool beyondRingMarkerFromKm(float dx_km, float dy_km, float dist_km, float* out_x,
                            float* out_y) {
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  // Inset by the marker radius too, so the whole marker stays inside the round screen.
  const float rim_r = static_cast<float>(radar::kCenterX - radar::kBeyondRingScreenMarginPx -
                                         radar::kBeyondRingMarkerRadiusPx);
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = radar::kCenterX + sinf(angle_rad) * rim_r;
  *out_y = radar::kCenterY - cosf(angle_rad) * rim_r;
  return true;
}

/** Notched arrow centred on (x, y), pointing along track_deg; a dot if there's no track to show. */
void drawBeyondRingMarker(float x, float y, float track_deg, bool show_arrow, uint16_t color) {
  if (show_arrow) {
    radar::drawRimArrow(x, y, track_deg, color);
  } else {
    radar::drawRimDot(x, y, color);
  }
}

/** Screen length of the distance flown in the current range's track_horizon_s. */
float speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0.0f;
  }
  const radar::RangePreset& range = radar::rangeCurrent();
  return gs_knots * kKmPerKnotSecond * range.track_horizon_s * radar::kGridOuterRadius /
         range.outer_km;
}

/**
 * End of the speed vector from (cx, cy), clipped to the outer ring; false if there's nothing
 * to draw. Unrounded, like the icon it starts from.
 */
bool speedVectorEnd(float cx, float cy, float track_deg, float gs_knots, float* ex,
                    float* ey) {
  const float len = speedLineLengthPx(gs_knots);
  if (len < 0.5f) {
    return false;
  }

  const float rad = track_deg * kDegToRad;
  *ex = cx + sinf(rad) * len;
  *ey = cy - cosf(rad) * len;
  radar::clipPointToOuterRingF(cx, cy, ex, ey);
  return fabsf(*ex - cx) >= 0.5f || fabsf(*ey - cy) >= 0.5f;
}

void applyTagStyle() {
  displayFontApply(canvas, radar::kAircraftTagLabelHeightPx);
}

enum class ClimbArrow : uint8_t { None, Up, Down };

/** Up or down when the aircraft is airborne and climbing or descending faster than kClimbArrowMinFpm. */
ClimbArrow climbArrowFor(const services::adsb::Aircraft& plane) {
  if (plane.alt_state != services::adsb::AltState::Airborne || !plane.has_vrate) {
    return ClimbArrow::None;
  }
  if (plane.vrate_fpm > radar::kClimbArrowMinFpm) {
    return ClimbArrow::Up;
  }
  if (plane.vrate_fpm < -radar::kClimbArrowMinFpm) {
    return ClimbArrow::Down;
  }
  return ClimbArrow::None;
}

/**
 * A tag's non-empty lines (callsign, type, altitude) and their widths; tag font applied.
 * The altitude line may end in a climb arrow, which its width includes.
 */
struct TagText {
  const char* lines[3];
  uint16_t colors[3];
  int widths[3];
  ClimbArrow arrows[3];
  uint8_t count;
};

constexpr int kClimbArrowExtentPx = radar::kClimbArrowGapPx + radar::kClimbArrowWidthPx;

TagText tagText(const services::adsb::Aircraft& plane, uint16_t alt_color) {
  TagText text{};
  const auto add = [&text](const char* line, uint16_t color, ClimbArrow arrow) {
    if (line[0] == '\0') {
      return;
    }
    text.lines[text.count] = line;
    text.colors[text.count] = color;
    text.arrows[text.count] = arrow;
    text.widths[text.count] =
        canvas.textWidth(line) + (arrow != ClimbArrow::None ? kClimbArrowExtentPx : 0);
    ++text.count;
  };
  add(plane.callsign, radar::kColorLabel, ClimbArrow::None);
  add(plane.type, radar::kColorTagType, ClimbArrow::None);
  add(plane.alt, alt_color, climbArrowFor(plane));
  return text;
}

int tagWidth(const TagText& text, uint8_t lines) {
  int w = 0;
  for (uint8_t i = 0; i < lines; ++i) {
    w = std::max(w, text.widths[i]);
  }
  return w;
}

/**
 * The first `lines` lines of the tag in its placed box, lined up toward the icon. Each line
 * is placed by its full width, so a climb arrow stays at the end of the altitude even when
 * the tag is right-aligned.
 */
void drawTag(const TagText& text, uint8_t lines, const tags::ScreenRect& box, tags::Slot slot,
             int line_h) {
  const tags::Align align = tags::slotAlign(slot);
  canvas.setTextDatum(textdatum_t::top_left);
  for (uint8_t i = 0; i < lines; ++i) {
    int x = box.left;
    if (align == tags::Align::Center) {
      x = box.left + (box.w - text.widths[i]) / 2;
    } else if (align == tags::Align::Right) {
      x = box.left + box.w - text.widths[i];
    }
    const int y = box.top + i * line_h;
    canvas.setTextColor(text.colors[i], radar::kColorBackground);
    canvas.drawString(text.lines[i], x, y);
    if (text.arrows[i] != ClimbArrow::None) {
      // Vertically centred on the line, which is about where the digits sit.
      const int arrow_x = x + text.widths[i] - radar::kClimbArrowWidthPx / 2;
      radar::drawClimbArrow(arrow_x, y + line_h / 2, text.arrows[i] == ClimbArrow::Up,
                            text.colors[i]);
    }
  }
}

/** How many lines tagText() gives the aircraft. */
uint8_t tagLineCount(const services::adsb::Aircraft& plane) {
  return static_cast<uint8_t>((plane.callsign[0] != '\0') + (plane.type[0] != '\0') +
                              (plane.alt[0] != '\0'));
}

/**
 * An aircraft that had a full tag keeps it while it is at most this many places past the
 * max_full_tags nearest, so tags don't grow and shrink as aircraft pass each other.
 */
constexpr size_t kFullTagKeepRanks = 2;

/** Tag outcomes of the last frame, for the serial log. */
struct TagStats {
  uint8_t full;
  uint8_t short_only;
  uint8_t hidden;
};
TagStats s_tag_stats{};

/** This frame's copy of the fetch task's results (the list is in PSRAM, ~5 KB). */
services::adsb::Aircraft* s_planes = nullptr;
services::adsb::Snapshot s_snapshot{};

/**
 * Dead reckoning: the aircraft's offset from the center in km, moved along its track by the
 * age of its position (seen_pos plus the time since the server's "now"), capped at
 * kDeadReckonMaxSec.
 * Returns true if it's still moving (has a track and speed, and isn't at the cap yet).
 */
bool deadReckonedOffsetKm(const services::adsb::Aircraft& plane, unsigned long now_ms,
                          float* dx_km, float* dy_km, float* dist_km) {
  offsetKmFromCenter(plane.lat, plane.lon, dx_km, dy_km, dist_km);
  if (!plane.has_track || plane.gs_knots <= 0.0f || s_snapshot.fetched_ms == 0) {
    return false;
  }
  // positions_ms is the server's "now", so the age includes how old its data was on arrival.
  const float since_now_s = static_cast<float>(now_ms - s_snapshot.positions_ms) / 1000.0f;
  const float raw_age_s = std::max(plane.seen_pos_s, 0.0f) + since_now_s;
  const float age_s = std::min(raw_age_s, config::kDeadReckonMaxSec);
  const float km = plane.gs_knots * kKmPerKnotSecond * age_s;
  const float rad = plane.track_deg * kDegToRad;
  *dx_km += sinf(rad) * km;
  *dy_km += cosf(rad) * km;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
  return raw_age_s < config::kDeadReckonMaxSec;
}

/** Set by drawAircraft(): some aircraft on screen is still being moved by dead reckoning. */
bool s_animating = false;

/**
 * The track drawn for one aircraft (icon, speed vector, rim arrow), keyed by hex. Reported
 * tracks wobble 1-3 degrees between fetches even in straight flight, which flicks the long
 * speed vector sideways every fetch; this eases toward each new value instead.
 */
struct ShownTrack {
  char hex[sizeof(services::adsb::Aircraft::hex)];
  float track_deg;
  unsigned long updated_ms;
};
ShownTrack s_shown_tracks[services::adsb::kMaxAircraft] = {};

/** d wrapped into [-180, 180). */
float wrapDeg180(float d) { return d - 360.0f * floorf((d + 180.0f) / 360.0f); }

/**
 * The plane's track eased toward the reported one with time constant kTrackSmoothingSec. A
 * plane not drawn recently takes the reported track as is, reusing the stalest slot. Only the
 * drawing uses this; dead reckoning moves along the reported track, which predicts the next
 * fix to a few metres.
 */
float shownTrackDeg(const services::adsb::Aircraft& plane, unsigned long now_ms) {
  if (!plane.has_track || plane.hex[0] == '\0') {
    return plane.track_deg;
  }
  ShownTrack* slot = nullptr;
  ShownTrack* stalest = &s_shown_tracks[0];
  for (ShownTrack& entry : s_shown_tracks) {
    if (strcmp(entry.hex, plane.hex) == 0) {
      slot = &entry;
      break;
    }
    if (entry.hex[0] == '\0') {
      stalest = &entry;  // an empty slot beats any used one
    } else if (stalest->hex[0] != '\0' &&
               now_ms - entry.updated_ms > now_ms - stalest->updated_ms) {
      stalest = &entry;
    }
  }
  if (slot == nullptr) {
    slot = stalest;
    strncpy(slot->hex, plane.hex, sizeof(slot->hex));
    slot->hex[sizeof(slot->hex) - 1] = '\0';
    slot->track_deg = plane.track_deg;
    slot->updated_ms = now_ms;
    return slot->track_deg;
  }
  const float dt_s = static_cast<float>(now_ms - slot->updated_ms) / 1000.0f;
  const float step = 1.0f - expf(-dt_s / config::kTrackSmoothingSec);
  float track = slot->track_deg + wrapDeg180(plane.track_deg - slot->track_deg) * step;
  track -= 360.0f * floorf(track / 360.0f);
  slot->track_deg = track;
  slot->updated_ms = now_ms;
  return track;
}

void takeSnapshot() {
  if (s_planes == nullptr) {
    s_planes = static_cast<services::adsb::Aircraft*>(heap_caps_malloc(
        sizeof(services::adsb::Aircraft) * services::adsb::kMaxAircraft, MALLOC_CAP_SPIRAM));
  }
  if (s_planes == nullptr) {
    s_snapshot = services::adsb::Snapshot{};  // nothing to draw, shows "Waiting for data"
    return;
  }
  services::adsb::aircraftSnapshot(s_planes, services::adsb::kMaxAircraft, &s_snapshot);
}

struct AircraftDrawItem {
  size_t index = 0;
  float fx = 0.0f;  // unrounded, for drawing
  float fy = 0.0f;
  int x = 0;  // rounded, for the tag layout
  int y = 0;
  int dist_sq = 0;
  float track_deg = 0.0f;  // smoothed, see shownTrackDeg()
  AircraftColors colors{};
  radar::IconShape shape = radar::IconShape::Generic;
};

struct BeyondRingDrawItem {
  float fx = 0.0f;  // unrounded, for drawing
  float fy = 0.0f;
  int x = 0;  // rounded, for the tag layout
  int y = 0;
  int dist_sq = 0;
  float track_deg = 0.0f;
  bool show_arrow = false;  // has a track and is moving
  uint16_t color = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondRingFarFirst(BeyondRingDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondRingDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

/**
 * drawAircraft()'s per-frame lists. At ~4.3 KB they were over half of the 8 KB loop task
 * stack, which overflowed (a panic) once deep drawing calls stacked on top. Kept in PSRAM.
 */
struct DrawLists {
  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondRingDrawItem rim[services::adsb::kMaxAircraft];
};
DrawLists* s_draw_lists = nullptr;

void drawAircraft() {
  const size_t n = s_snapshot.count;
  const services::adsb::Aircraft* planes = s_planes;

  if (s_draw_lists == nullptr) {
    void* mem = heap_caps_malloc(sizeof(DrawLists), MALLOC_CAP_SPIRAM);
    if (mem == nullptr) {
      return;
    }
    s_draw_lists = new (mem) DrawLists();
  }
  AircraftDrawItem* items = s_draw_lists->items;
  BeyondRingDrawItem* rim = s_draw_lists->rim;
  size_t draw_count = 0;
  size_t rim_count = 0;

  {
  PhaseScope timer(Phase::Classify);
  const unsigned long now_ms = millis();
  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    // Extrapolated, so aircraft cross the ring between fetches instead of jumping.
    const bool moving = deadReckonedOffsetKm(planes[i], now_ms, &dx_km, &dy_km, &dist_km);
    const float shown_track = shownTrackDeg(planes[i], now_ms);

    if (isInsideOuterRingKm(dist_km)) {
      float fx = 0.0f;
      float fy = 0.0f;
      kmOffsetToScreenF(dx_km, dy_km, &fx, &fy);
      const int x = static_cast<int>(lroundf(fx));
      const int y = static_cast<int>(lroundf(fy));
      items[draw_count].index = i;
      items[draw_count].fx = fx;
      items[draw_count].fy = fy;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      items[draw_count].track_deg = shown_track;
      items[draw_count].colors = aircraftColors(planes[i]);
      items[draw_count].shape = radar::iconShapeFor(planes[i]);
      ++draw_count;
      // Only in-ring aircraft count: a rim marker's bearing barely changes between frames.
      s_animating |= moving;
      continue;
    }

    float rim_fx = 0.0f;
    float rim_fy = 0.0f;
    if (!beyondRingMarkerFromKm(dx_km, dy_km, dist_km, &rim_fx, &rim_fy)) {
      continue;
    }
    const int rim_x = static_cast<int>(lroundf(rim_fx));
    const int rim_y = static_cast<int>(lroundf(rim_fy));
    rim[rim_count].fx = rim_fx;
    rim[rim_count].fy = rim_fy;
    rim[rim_count].x = rim_x;
    rim[rim_count].y = rim_y;
    rim[rim_count].dist_sq = distSqFromCenter(rim_x, rim_y);
    rim[rim_count].track_deg = shown_track;
    rim[rim_count].show_arrow = planes[i].has_track && planes[i].gs_knots > 0.0f;
    rim[rim_count].color = aircraftColors(planes[i]).icon;
    ++rim_count;
  }
  sortBeyondRingFarFirst(rim, rim_count);
  // Icons far-first, so near aircraft paint on top.
  sortDrawItemsFarFirst(items, draw_count);
  }

  {
  PhaseScope timer(Phase::Rim);
  for (size_t d = 0; d < rim_count; ++d) {
    drawBeyondRingMarker(rim[d].fx, rim[d].fy, rim[d].track_deg, rim[d].show_arrow,
                         rim[d].color);
    tags::addCircle(rim[d].x, rim[d].y, radar::kBeyondRingMarkerRadiusPx);
  }
  }

  // Each icon and speed vector is an obstacle for the tags; the draw index is the icon's
  // owner id.
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    const float fx = items[d].fx;
    const float fy = items[d].fy;
    float ex = 0.0f;
    float ey = 0.0f;
    // Drawn before the icon, so the part under it is hidden.
    if (planes[i].has_track &&
        speedVectorEnd(fx, fy, items[d].track_deg, planes[i].gs_knots, &ex, &ey)) {
      PhaseScope timer(Phase::Vectors);
      drawThickLine(fx, fy, ex, ey, radar::kAircraftTrackLineHalfWidth, items[d].colors.track);
      tags::addSegment(x, y, static_cast<int>(lroundf(ex)), static_cast<int>(lroundf(ey)),
                       radar::kAircraftTrackLineHalfWidth);
    }
    PhaseScope timer(Phase::Icons);
    radar::drawAircraftIcon(fx, fy, iconBearing(planes[i], items[d].track_deg), items[d].shape,
                            items[d].colors.icon);
    tags::addCircle(x, y, radar::iconRadiusPx(items[d].shape), static_cast<int>(d));
  }

  // Which aircraft get full tags, decided before placing so the choice doesn't depend on
  // what fits: first last frame's full tags still within kFullTagKeepRanks of the cutoff,
  // then the nearest others up to max_full_tags. items[] is far-first, so the nearest-first
  // rank r is items[draw_count - 1 - r].
  const int64_t tag_place_start = esp_timer_get_time();
  const uint8_t max_full = radar::rangeCurrent().max_full_tags;
  bool want_full[services::adsb::kMaxAircraft] = {};
  size_t full_count = 0;
  const size_t keep_ranks = std::min(draw_count, max_full + kFullTagKeepRanks);
  for (size_t r = 0; r < keep_ranks && full_count < max_full; ++r) {
    const size_t d = draw_count - 1 - r;
    const services::adsb::Aircraft& plane = planes[items[d].index];
    if (tagLineCount(plane) > 1 && tags::wasFull(plane.hex)) {
      want_full[d] = true;
      ++full_count;
    }
  }
  for (size_t r = 0; r < draw_count && full_count < max_full; ++r) {
    const size_t d = draw_count - 1 - r;
    if (!want_full[d] && tagLineCount(planes[items[d].index]) > 1) {
      want_full[d] = true;
      ++full_count;
    }
  }

  // Tags nearest-first, so the aircraft that matter most get the best slots. A full tag that
  // doesn't fit tries the first line alone; its place in the full set isn't passed on.
  applyTagStyle();
  const int line_h = canvas.fontHeight();
  s_tag_stats = {};
  for (size_t d = draw_count; d-- > 0;) {
    const services::adsb::Aircraft& plane = planes[items[d].index];
    const TagText text = tagText(plane, items[d].colors.icon);
    if (text.count == 0) {
      continue;
    }

    tags::TagRequest req{};
    req.hex = plane.hex;
    req.owner = static_cast<int>(d);
    req.x = items[d].x;
    req.y = items[d].y;
    req.fx = items[d].fx;
    req.fy = items[d].fy;
    req.icon_radius = radar::iconRadiusPx(items[d].shape);
    req.moving = plane.has_track && plane.gs_knots > 0.0f;
    req.track_deg = items[d].track_deg;
    req.full = want_full[d];

    tags::ScreenRect box{};
    tags::Slot slot = tags::Slot::Right;
    uint8_t lines = want_full[d] ? text.count : 1;
    req.w = tagWidth(text, lines);
    req.h = line_h * lines;
    bool placed = tags::place(req, &box, &slot);
    if (!placed && lines > 1) {
      lines = 1;
      req.w = tagWidth(text, lines);
      req.h = line_h;
      placed = tags::place(req, &box, &slot);
    }

    if (!placed) {
      ++s_tag_stats.hidden;
      continue;
    }
    if (lines > 1) {
      ++s_tag_stats.full;
    } else {
      ++s_tag_stats.short_only;
    }
    PhaseScope timer(Phase::TagDraw);
    drawTag(text, lines, box, slot, line_h);
  }
  // Text measuring and slot search: everything in the tag pass except drawing.
  const uint32_t tag_us = static_cast<uint32_t>(esp_timer_get_time() - tag_place_start);
  s_phase_us[static_cast<size_t>(Phase::TagPlace)] +=
      tag_us - std::min(tag_us, s_phase_us[static_cast<size_t>(Phase::TagDraw)]);
}

/** Fixed things tags must not cover: the home marker, labels, the clock; airport labels softly. */
void addFixedTagObstacles(const tags::ScreenRect* clock_box) {
  tags::addCircle(radar::kCenterX, radar::kCenterY, radar::kHomeMarkerRadiusPx);
  for (size_t i = 0; i < radar::cardinalLabelCount(); ++i) {
    tags::addRect(radar::cardinalLabelBox(i));
  }
  tags::addRect(radar::scaleLabelBox());
  if (clock_box != nullptr) {
    tags::addRect(*clock_box);
  }
  for (size_t i = 0; i < runway::airportLabelCount(); ++i) {
    tags::addSoftRect(runway::airportLabelBox(i));
  }
}

/** Headline and detail for the empty radar, from how the last fetch went. */
void describeFetchProblem(char* headline, size_t headline_len, char* detail, size_t detail_len) {
  using services::adsb::FetchStatus;
  const int code = s_snapshot.error_code;
  const char* title = "";
  detail[0] = '\0';
  switch (s_snapshot.status) {
    case FetchStatus::Pending:
      title = "Waiting for data";
      break;
    case FetchStatus::Ok:  // last fetch was good, but too long ago
      title = "Data out of date";
      break;
    case FetchStatus::NoConnection:
      title = "No internet";
      snprintf(detail, detail_len, "Can't reach adsb.fi");
      break;
    case FetchStatus::Timeout:
      title = "adsb.fi not responding";
      snprintf(detail, detail_len, "Request timed out");
      break;
    case FetchStatus::ConnectionLost:
      title = "Connection lost";
      if (code != 0) {
        snprintf(detail, detail_len, "Error %d", code);
      }
      break;
    case FetchStatus::RateLimited:
      title = "adsb.fi rate limit";
      snprintf(detail, detail_len, "HTTP %d", code);
      break;
    case FetchStatus::ServerError:
      title = "adsb.fi unavailable";
      snprintf(detail, detail_len, "HTTP %d", code);
      break;
    case FetchStatus::HttpError:
      title = "adsb.fi error";
      snprintf(detail, detail_len, "HTTP %d", code);
      break;
    case FetchStatus::BadResponse:
      title = "Bad data from adsb.fi";
      snprintf(detail, detail_len, "%s", s_snapshot.error_detail);
      break;
  }
  snprintf(headline, headline_len, "%s", title);
}

/** Shown instead of aircraft when the data is stale, between the first and second rings. */
void drawFetchProblem() {
  char headline[32];
  char detail[40];
  describeFetchProblem(headline, sizeof(headline), detail, sizeof(detail));
  radar::drawMessageBox(headline, detail, radar::messageBoxCenterY());
}

void drawStaticGrid() {
  {
    PhaseScope timer(Phase::Clear);
    radar::clearFrame();
  }
  {
    PhaseScope timer(Phase::Rings);
    radar::drawRings();
  }
  {
    PhaseScope timer(Phase::Crosshairs);
    radar::drawCrosshairs();
  }
  {
    PhaseScope timer(Phase::Runways);
    runway::drawLargeAirportRunways(canvas);
  }
  {
    PhaseScope timer(Phase::Home);
    radar::drawHome();
  }
  {
    PhaseScope timer(Phase::Cardinals);
    radar::drawCardinalLabels();
  }
  PhaseScope timer(Phase::Scale);
  radar::drawScaleLabel();
}

}  // namespace

// Draw the grid and aircraft into the back framebuffer, then swap it on screen,
// so labels never show an erase/redraw gap.
void radarDisplayDraw(bool log) {
  const unsigned long t0 = millis();
  std::fill(std::begin(s_phase_us), std::end(s_phase_us), 0);
  s_animating = false;
  radar::applyPalette();
  tags::beginFrame();
  drawStaticGrid();
  radar::ClockLayout clock{};
  bool clock_shown = false;
  {
    PhaseScope timer(Phase::ClockLayout);
    clock_shown = radar::layoutClock(&clock);
  }
  takeSnapshot();
  // Old positions look live, so once the data goes stale show why instead.
  const bool fresh = s_snapshot.fresh;
  if (fresh) {
    {
      PhaseScope timer(Phase::Obstacles);
      addFixedTagObstacles(clock_shown ? &clock.box : nullptr);
    }
    drawAircraft();
  } else {
    PhaseScope timer(Phase::Problem);
    drawFetchProblem();
  }
  if (clock_shown) {
    PhaseScope timer(Phase::Clock);
    radar::drawClock(clock);
  }
  canvas.setTextDatum(textdatum_t::top_left);
  const unsigned long t1 = millis();
  displayPresent();
  if (!log) {
    return;
  }
  if (fresh) {
    Serial.printf("Radar frame: draw %lu ms, present %lu ms, %u aircraft, tags %u full, "
                  "%u short, %u hidden\n",
                  t1 - t0, millis() - t1, static_cast<unsigned>(s_snapshot.count),
                  static_cast<unsigned>(s_tag_stats.full),
                  static_cast<unsigned>(s_tag_stats.short_only),
                  static_cast<unsigned>(s_tag_stats.hidden));
  } else {
    Serial.printf("Radar frame: draw %lu ms, present %lu ms, %u aircraft (stale, hidden)\n",
                  t1 - t0, millis() - t1, static_cast<unsigned>(s_snapshot.count));
  }
  logPhases();
}

bool radarDisplayAnimating() { return s_animating; }

}  // namespace ui
