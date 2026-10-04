#include "ui/radar_display.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "ui/radar_geometry.h"
#include "services/clock.h"
#include "ui/radar_range.h"
#include "ui/radar_shapes.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"
#include "ui/tag_layout.h"

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorAlert = 0xFE40;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
uint16_t kColorAltGround = 0x9CD3;
uint16_t kColorAltUnknown = 0xFFFF;

}  // namespace radar

namespace {

using radar::clipPointToOuterRing;
using radar::distSqFromCenter;
using radar::latLonToScreen;
using radar::offsetKmFromCenter;

/** Scale label is a little shorter than the cardinal letters. */
constexpr int kScaleLabelHeightPx =
    radar::kCardinalLabelHeightPx - radar::kScaleBelowCardinalPx;

void initPalette() {
  radar::kColorBackground = canvas.color565(radar::kBgR, radar::kBgG, radar::kBgB);
  radar::kColorGrid = canvas.color565(radar::kGridR, radar::kGridG, radar::kGridB);
  radar::kColorLabel = canvas.color565(255, 255, 255);
  radar::kColorCenter = canvas.color565(255, 255, 255);
  radar::kColorAlert = canvas.color565(radar::kAlertR, radar::kAlertG, radar::kAlertB);
  radar::kColorTagType =
      canvas.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
  radar::kColorRunway =
      canvas.color565(radar::kRunwayR, radar::kRunwayG, radar::kRunwayB);
  radar::kColorRunwayLabel = canvas.color565(radar::kRunwayLabelR, radar::kRunwayLabelG,
                                          radar::kRunwayLabelB);
  radar::kColorAltGround =
      canvas.color565(radar::kAltGroundR, radar::kAltGroundG, radar::kAltGroundB);
  radar::kColorAltUnknown =
      canvas.color565(radar::kAltUnknownR, radar::kAltUnknownG, radar::kAltUnknownB);
}

struct Rgb {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

Rgb mixRgb(const Rgb& a, const Rgb& b, float t) {
  const auto mix = [t](uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(lroundf(x + (y - x) * t));
  };
  return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
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
float iconBearing(const services::adsb::Aircraft& plane) {
  return plane.has_track ? plane.track_deg : plane.nose_deg;
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

/** Rim marker from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingMarkerFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  // Inset by the marker radius too, so the whole marker stays inside the round screen.
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx -
                    radar::kBeyondRingMarkerRadiusPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

/** Notched arrow centred on (x, y), pointing along track_deg; a dot if there's no track to show. */
void drawBeyondRingMarker(int x, int y, float track_deg, bool show_arrow, uint16_t color) {
  if (show_arrow) {
    radar::drawRimArrow(x, y, track_deg, color);
  } else {
    canvas.fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx, color);
  }
}

/** Screen length of the distance flown in the current range's track_horizon_s. */
int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }
  constexpr float kKmPerKnotSecond = 1.852f / 3600.0f;
  const radar::RangePreset& range = radar::rangeCurrent();
  const float px = gs_knots * kKmPerKnotSecond * range.track_horizon_s *
                   radar::kGridOuterRadius / range.outer_km;
  return static_cast<int>(px + 0.5f);
}

/** End of the speed vector, clipped to the outer ring; false if there's nothing to draw. */
bool speedVectorEnd(int cx, int cy, float track_deg, float gs_knots, int* ex, int* ey) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return false;
  }

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  *ex = cx + static_cast<int>(lroundf(sinf(rad) * len));
  *ey = cy - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(cx, cy, ex, ey);
  return *ex != cx || *ey != cy;
}

void applyTagStyle() {
  displayFontApply(canvas, radar::kAircraftTagLabelHeightPx);
}

/** A tag's non-empty lines (callsign, type, altitude) and their widths; tag font applied. */
struct TagText {
  const char* lines[3];
  uint16_t colors[3];
  int widths[3];
  uint8_t count;
};

TagText tagText(const services::adsb::Aircraft& plane, uint16_t alt_color) {
  TagText text{};
  const auto add = [&text](const char* line, uint16_t color) {
    if (line[0] == '\0') {
      return;
    }
    text.lines[text.count] = line;
    text.colors[text.count] = color;
    text.widths[text.count] = canvas.textWidth(line);
    ++text.count;
  };
  add(plane.callsign, radar::kColorLabel);
  add(plane.type, radar::kColorTagType);
  add(plane.alt, alt_color);
  return text;
}

int tagWidth(const TagText& text, uint8_t lines) {
  int w = 0;
  for (uint8_t i = 0; i < lines; ++i) {
    w = std::max(w, text.widths[i]);
  }
  return w;
}

/** The first `lines` lines of the tag in its placed box, lined up toward the icon. */
void drawTag(const TagText& text, uint8_t lines, const tags::ScreenRect& box, tags::Slot slot,
             int line_h) {
  int x = box.left;
  switch (tags::slotAlign(slot)) {
    case tags::Align::Left:
      canvas.setTextDatum(textdatum_t::top_left);
      break;
    case tags::Align::Center:
      canvas.setTextDatum(textdatum_t::top_center);
      x = box.left + box.w / 2;
      break;
    case tags::Align::Right:
      canvas.setTextDatum(textdatum_t::top_right);
      x = box.left + box.w;
      break;
  }
  for (uint8_t i = 0; i < lines; ++i) {
    canvas.setTextColor(text.colors[i], radar::kColorBackground);
    canvas.drawString(text.lines[i], x, box.top + i * line_h);
  }
}

/** Tag outcomes of the last frame, for the serial log. */
struct TagStats {
  uint8_t full;
  uint8_t short_only;
  uint8_t hidden;
};
TagStats s_tag_stats{};

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
  AircraftColors colors{};
  radar::IconShape shape = radar::IconShape::Generic;
};

struct BeyondRingDrawItem {
  int x = 0;
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

void drawAircraft() {

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondRingDrawItem rim[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t rim_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      items[draw_count].colors = aircraftColors(planes[i]);
      items[draw_count].shape = radar::iconShapeFor(planes[i]);
      ++draw_count;
      continue;
    }

    int rim_x = 0;
    int rim_y = 0;
    if (!beyondRingMarkerFromLatLon(planes[i].lat, planes[i].lon, &rim_x,
                                    &rim_y)) {
      continue;
    }
    rim[rim_count].x = rim_x;
    rim[rim_count].y = rim_y;
    rim[rim_count].dist_sq = distSqFromCenter(rim_x, rim_y);
    rim[rim_count].track_deg = planes[i].track_deg;
    rim[rim_count].show_arrow = planes[i].has_track && planes[i].gs_knots > 0.0f;
    rim[rim_count].color = aircraftColors(planes[i]).icon;
    ++rim_count;
  }

  sortBeyondRingFarFirst(rim, rim_count);
  for (size_t d = 0; d < rim_count; ++d) {
    drawBeyondRingMarker(rim[d].x, rim[d].y, rim[d].track_deg, rim[d].show_arrow,
                         rim[d].color);
    tags::addCircle(rim[d].x, rim[d].y, radar::kBeyondRingMarkerRadiusPx);
  }

  // Icons far-first, so near aircraft paint on top. Each icon and speed vector is an
  // obstacle for the tags; the draw index is the icon's owner id.
  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    int ex = 0;
    int ey = 0;
    // Drawn before the icon, so the part under it is hidden.
    if (planes[i].has_track &&
        speedVectorEnd(x, y, planes[i].track_deg, planes[i].gs_knots, &ex, &ey)) {
      canvas.drawWideLine(x, y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                          items[d].colors.track);
      tags::addSegment(x, y, ex, ey, radar::kAircraftTrackLineHalfWidth);
    }
    radar::drawAircraftIcon(x, y, iconBearing(planes[i]), items[d].shape,
                            items[d].colors.icon);
    tags::addCircle(x, y, radar::iconRadiusPx(items[d].shape), static_cast<int>(d));
  }

  // Tags nearest-first, so the aircraft that matter most get the best slots. The nearest
  // max_full_tags get every line; the rest, or any that don't fit, try the first line alone.
  applyTagStyle();
  const int line_h = canvas.fontHeight();
  const uint8_t max_full = radar::rangeCurrent().max_full_tags;
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
    req.icon_radius = radar::iconRadiusPx(items[d].shape);
    req.moving = plane.has_track && plane.gs_knots > 0.0f;
    req.track_deg = plane.track_deg;

    tags::ScreenRect box{};
    tags::Slot slot = tags::Slot::Right;
    uint8_t lines = s_tag_stats.full < max_full ? text.count : 1;
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
    drawTag(text, lines, box, slot, line_h);
  }
}

void applyCardinalStyle() {
  displayFontApply(canvas, radar::kCardinalLabelHeightPx);
}

void applyScaleStyle() { displayFontApply(canvas, kScaleLabelHeightPx); }

struct CardinalLabel {
  const char* text;
  int x;
  int y;
  textdatum_t datum;
};

constexpr CardinalLabel kCardinalLabels[] = {
    {"N", radar::kCenterX, radar::kCardinalNorthOffsetY, textdatum_t::top_center},
    {"S", radar::kCenterX, radar::kSize - 1 + radar::kCardinalSouthOffsetY,
     textdatum_t::bottom_center},
    {"W", radar::kCardinalSideInsetPx, radar::kCenterY, textdatum_t::middle_left},
    {"E", radar::kSize - 1 - radar::kCardinalSideInsetPx, radar::kCenterY,
     textdatum_t::middle_right},
};

/** Box of a cardinal letter, for the datums kCardinalLabels uses; cardinal font applied. */
tags::ScreenRect cardinalLabelRect(const CardinalLabel& label) {
  const int w = canvas.textWidth(label.text);
  const int h = canvas.fontHeight();
  switch (label.datum) {
    case textdatum_t::top_center: return {label.x - w / 2, label.y, w, h};
    case textdatum_t::bottom_center: return {label.x - w / 2, label.y - h, w, h};
    case textdatum_t::middle_left: return {label.x, label.y - h / 2, w, h};
    case textdatum_t::middle_right: return {label.x - w, label.y - h / 2, w, h};
    default: return {label.x, label.y, w, h};
  }
}

/** Scale label's background box, right edge padded past the middle_right anchor (x, y). */
tags::ScreenRect scaleLabelRect(const char* text, int x, int y) {
  applyScaleStyle();
  const int tw = canvas.textWidth(text);
  const int th = canvas.fontHeight();
  constexpr int kPadX = px(3);
  constexpr int kPadY = px(2);
  return {x - tw - kPadX, y - th / 2 - kPadY, tw + kPadX * 2, th + kPadY * 2};
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  const tags::ScreenRect box = scaleLabelRect(text, x, y);
  canvas.fillRect(box.left, box.top, box.w, box.h, radar::kColorBackground);
  canvas.setTextDatum(textdatum_t::middle_right);
  canvas.setTextColor(radar::kColorGrid, radar::kColorBackground);
  canvas.drawString(text, x, y);
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  // One filled annulus: stacked 1 px circles leave pinholes once the stroke is thick.
  const int thickness =
      std::max(1, static_cast<int>(lroundf(radar::kGridStrokeHalfWidth * 2.0f)));
  canvas.fillArc(cx, cy, std::max(0, r - thickness + 1), r, 0.0f, 360.0f, color);
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  canvas.drawWideLine(cx, cy - radius, cx, cy + radius,
                      radar::kGridStrokeHalfWidth, color);
  canvas.drawWideLine(cx - radius, cy, cx + radius, cy,
                      radar::kGridStrokeHalfWidth, color);
}

void drawHomeMarker(int cx, int cy) {
  radar::drawHomeMarker(cx, cy, radar::kColorCenter, radar::kColorBackground);
}

void drawCardinalLabels() {
  applyCardinalStyle();
  canvas.setTextColor(radar::kColorLabel, radar::kColorBackground);
  for (const CardinalLabel& label : kCardinalLabels) {
    canvas.setTextDatum(label.datum);
    canvas.drawString(label.text, label.x, label.y);
  }
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

/** Fixed things tags must not cover: the home marker, labels, the clock; airport labels softly. */
void addFixedTagObstacles(const tags::ScreenRect* clock_box) {
  tags::addCircle(radar::kCenterX, radar::kCenterY, radar::kHomeMarkerRadiusPx);

  applyCardinalStyle();
  for (const CardinalLabel& label : kCardinalLabels) {
    tags::addRect(cardinalLabelRect(label));
  }

  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  tags::addRect(scaleLabelRect(
      scale_label, scaleLabelAnchorX(radar::kCenterX, radar::kGridOuterRadius),
      radar::kCenterY));

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
  const int code = services::adsb::lastErrorCode();
  const char* title = "";
  detail[0] = '\0';
  switch (services::adsb::lastStatus()) {
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
      snprintf(detail, detail_len, "%s", services::adsb::lastErrorDetail());
      break;
  }
  snprintf(headline, headline_len, "%s", title);
}

/** Shown instead of aircraft when the data is stale, between the first and second rings. */
void drawFetchProblem() {
  char headline[32];
  char detail[40];
  describeFetchProblem(headline, sizeof(headline), detail, sizeof(detail));

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY + radar::kGridOuterRadius * 3 / 8;
  constexpr int kPadX = px(4);
  constexpr int kPadY = px(2);

  displayFontApply(canvas, radar::kCardinalLabelHeightPx);
  const int headline_w = canvas.textWidth(headline);
  const int headline_h = canvas.fontHeight();
  int detail_w = 0;
  int detail_h = 0;
  if (detail[0] != '\0') {
    displayFontApply(canvas, radar::kAircraftTagLabelHeightPx);
    detail_w = canvas.textWidth(detail);
    detail_h = canvas.fontHeight();
  }

  const int box_w = std::max(headline_w, detail_w) + kPadX * 2;
  const int box_h = headline_h + detail_h + kPadY * 2;
  const int top = cy - box_h / 2;
  canvas.fillRect(cx - box_w / 2, top, box_w, box_h, radar::kColorBackground);

  canvas.setTextDatum(textdatum_t::top_center);
  displayFontApply(canvas, radar::kCardinalLabelHeightPx);
  canvas.setTextColor(radar::kColorAlert, radar::kColorBackground);
  canvas.drawString(headline, cx, top + kPadY);
  if (detail[0] != '\0') {
    displayFontApply(canvas, radar::kAircraftTagLabelHeightPx);
    canvas.setTextColor(radar::kColorLabel, radar::kColorBackground);
    canvas.drawString(detail, cx, top + kPadY + headline_h);
  }
}

/**
 * White text over a black outline, on a transparent background. The outline is the text
 * drawn at 12 points around a circle; 8 left notches at the corners once it got thicker.
 */
void drawOutlinedString(const char* text, int x, int y) {
  constexpr int kSteps = 12;
  constexpr float kStepRad = 6.2831853f / kSteps;
  constexpr float o = radar::kClockOutlinePx;
  canvas.setTextColor(config::kColorBlack);
  for (int i = 0; i < kSteps; ++i) {
    const int dx = static_cast<int>(lroundf(cosf(i * kStepRad) * o));
    const int dy = static_cast<int>(lroundf(sinf(i * kStepRad) * o));
    canvas.drawString(text, x + dx, y + dy);
  }
  canvas.setTextColor(radar::kColorLabel);
  canvas.drawString(text, x, y);
}

struct ClockLayout {
  char hhmm[8];
  char suffix[4];
  int left_x;
  int baseline_y;
  int time_w;
  int gap;
  int suffix_w;
  tags::ScreenRect box;  // digits and outline, for tag placement
};

/** Measures the clock on the south spoke; false until the time is synced. */
bool layoutClock(ClockLayout* clock) {
  if (!services::clock::formatTime(clock->hhmm, sizeof(clock->hhmm), clock->suffix,
                                   sizeof(clock->suffix))) {
    return false;
  }

  displayFontApply(canvas, radar::kAircraftTagLabelHeightPx);
  clock->suffix_w = clock->suffix[0] != '\0' ? canvas.textWidth(clock->suffix) : 0;
  displayFontApply(canvas, radar::kClockLabelHeightPx);
  clock->time_w = canvas.textWidth(clock->hhmm);
  clock->gap = clock->suffix_w > 0 ? radar::kClockSuffixGapPx : 0;

  // Both parts share a baseline. Digits are about half the line height tall, so a
  // baseline ~0.27 line heights below the centre puts them on it.
  const int center_y = radar::kCenterY +
                       static_cast<int>(radar::kGridOuterRadius * radar::kClockCenterRingFraction);
  clock->baseline_y = center_y + radar::kClockLabelHeightPx * 27 / 100;
  const int total_w = clock->time_w + clock->gap + clock->suffix_w;
  clock->left_x = radar::kCenterX - total_w / 2;

  // The digits span about ±0.3 line heights around center_y; add the outline all round.
  const int half_h = radar::kClockLabelHeightPx * 32 / 100 + radar::kClockOutlinePx;
  clock->box = {clock->left_x - radar::kClockOutlinePx, center_y - half_h,
                total_w + radar::kClockOutlinePx * 2, half_h * 2};
  return true;
}

/** Local time on the south spoke, drawn last so it sits above aircraft and tags. */
void drawClock(const ClockLayout& clock) {
  canvas.setTextDatum(textdatum_t::baseline_left);
  displayFontApply(canvas, radar::kClockLabelHeightPx);
  drawOutlinedString(clock.hhmm, clock.left_x, clock.baseline_y);
  if (clock.suffix_w > 0) {
    displayFontApply(canvas, radar::kAircraftTagLabelHeightPx);
    drawOutlinedString(clock.suffix, clock.left_x + clock.time_w + clock.gap,
                       clock.baseline_y);
  }
}

void drawStaticGrid() {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  canvas.fillScreen(radar::kColorBackground);
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  runway::drawLargeAirportRunways(canvas);
  drawHomeMarker(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
}

}  // namespace

// Draw the grid and aircraft into the back framebuffer, then swap it on screen,
// so labels never show an erase/redraw gap.
void radarDisplayDraw() {
  const unsigned long t0 = millis();
  initPalette();
  tags::beginFrame();
  drawStaticGrid();
  ClockLayout clock{};
  const bool clock_shown = layoutClock(&clock);
  // Old positions look live, so once the data goes stale show why instead.
  const bool fresh = services::adsb::aircraftFresh();
  if (fresh) {
    addFixedTagObstacles(clock_shown ? &clock.box : nullptr);
    drawAircraft();
  } else {
    drawFetchProblem();
  }
  if (clock_shown) {
    drawClock(clock);
  }
  canvas.setTextDatum(textdatum_t::top_left);
  const unsigned long t1 = millis();
  displayPresent();
  if (fresh) {
    Serial.printf("Radar frame: draw %lu ms, present %lu ms, %u aircraft, tags %u full, "
                  "%u short, %u hidden\n",
                  t1 - t0, millis() - t1,
                  static_cast<unsigned>(services::adsb::aircraftCount()),
                  static_cast<unsigned>(s_tag_stats.full),
                  static_cast<unsigned>(s_tag_stats.short_only),
                  static_cast<unsigned>(s_tag_stats.hidden));
  } else {
    Serial.printf("Radar frame: draw %lu ms, present %lu ms, %u aircraft (stale, hidden)\n",
                  t1 - t0, millis() - t1,
                  static_cast<unsigned>(services::adsb::aircraftCount()));
  }
}

void radarDisplayRefreshAircraft() { radarDisplayDraw(); }

}  // namespace ui
