#include "ui/tag_layout.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "services/adsb_client.h"
#include "ui/radar_theme.h"

namespace ui::tags {
namespace {

constexpr size_t kMaxAircraft = services::adsb::kMaxAircraft;
/** Fixed labels plus one per placed tag. */
constexpr size_t kMaxRects = kMaxAircraft + 16;
constexpr size_t kMaxSoftRects = 32;
/** Icons, rim markers and the home marker. */
constexpr size_t kMaxCircles = kMaxAircraft * 2 + 4;
constexpr size_t kMaxSegments = kMaxAircraft;
/** Forget an aircraft's slot after this many frames without seeing it. */
constexpr uint32_t kForgetAfterFrames = 3;
/**
 * A tag stays where it was until its icon has moved more than this far (physical px, either
 * axis) from the remembered anchor. Not a design size, so not scaled with px().
 */
constexpr float kTagHoldPx = 1.0f;

/** Slot costs; the cheapest slot that fits wins. */
constexpr int kOrderStepCost = 10;     // per step down the preference order
constexpr int kVectorSideCost = 60;    // slot straight along the track; scaled by the cosine
constexpr int kKeepSlotBonus = 100;    // last frame's slot
/** A better slot must win this many frames in a row (at 4 Hz, ~1 s) before the tag moves. */
constexpr uint8_t kSwitchAfterFrames = 4;
constexpr int kSoftOverlapCost = 200;  // covering an airport label at all...
constexpr int kSoftOverlapAreaDiv = 64;  // ...plus 1 per this many px² covered

constexpr Slot kOrder[] = {Slot::Right,     Slot::Left,      Slot::UpperRight, Slot::LowerRight,
                           Slot::UpperLeft, Slot::LowerLeft, Slot::Above,      Slot::Below};
constexpr int kSlotCount = sizeof(kOrder) / sizeof(kOrder[0]);
constexpr float kDiag = 0.70710678f;

struct Circle {
  int x;
  int y;
  int r;
  int owner;
};

struct Segment {
  float x0;
  float y0;
  float x1;
  float y1;
  float half_width;
};

struct Memory {
  char hex[7];
  Slot slot;
  int anchor_x;  // icon centre the tag was placed around
  int anchor_y;
  Slot pending_slot;       // a better slot the tag isn't in yet...
  uint8_t pending_frames;  // ...and how many frames in a row it has won
  bool full;
  uint32_t last_frame;
};

/** ~6.5 KB, kept in PSRAM: internal RAM is short and these are only touched once a frame. */
struct Storage {
  ScreenRect rects[kMaxRects];
  ScreenRect soft_rects[kMaxSoftRects];
  Circle circles[kMaxCircles];
  Segment segments[kMaxSegments];
  Memory memory[kMaxAircraft];
};

Storage* s_store = nullptr;  // allocated by the first beginFrame(); null = no tags
size_t s_rect_count = 0;
size_t s_soft_rect_count = 0;
size_t s_circle_count = 0;
size_t s_segment_count = 0;
uint32_t s_frame = 0;

Slot mirrored(Slot slot) {
  switch (slot) {
    case Slot::Right: return Slot::Left;
    case Slot::Left: return Slot::Right;
    case Slot::UpperRight: return Slot::UpperLeft;
    case Slot::LowerRight: return Slot::LowerLeft;
    case Slot::UpperLeft: return Slot::UpperRight;
    case Slot::LowerLeft: return Slot::LowerRight;
    default: return slot;
  }
}

/** Unit vector from the icon toward the slot, screen axes (y down). */
void slotDirection(Slot slot, float* dx, float* dy) {
  switch (slot) {
    case Slot::Right: *dx = 1.0f; *dy = 0.0f; break;
    case Slot::Left: *dx = -1.0f; *dy = 0.0f; break;
    case Slot::UpperRight: *dx = kDiag; *dy = -kDiag; break;
    case Slot::LowerRight: *dx = kDiag; *dy = kDiag; break;
    case Slot::UpperLeft: *dx = -kDiag; *dy = -kDiag; break;
    case Slot::LowerLeft: *dx = -kDiag; *dy = kDiag; break;
    case Slot::Above: *dx = 0.0f; *dy = -1.0f; break;
    case Slot::Below: *dx = 0.0f; *dy = 1.0f; break;
  }
}

/** The tag box for a slot, clear of the icon's radius by the label gap. */
ScreenRect slotRect(const TagRequest& req, Slot slot) {
  const int w = req.w;
  const int h = req.h;
  const int side = req.icon_radius + radar::kAircraftLabelGapPx;
  // A diagonal box's near corner sits on the icon's radius, plus the gap.
  const int diag = static_cast<int>(lroundf(req.icon_radius * kDiag)) +
                   radar::kAircraftLabelGapPx;
  const int x = req.x;
  const int y = req.y;
  switch (slot) {
    case Slot::Right: return {x + side, y - h / 2, w, h};
    case Slot::Left: return {x - side - w, y - h / 2, w, h};
    case Slot::UpperRight: return {x + diag, y - diag - h, w, h};
    case Slot::LowerRight: return {x + diag, y + diag, w, h};
    case Slot::UpperLeft: return {x - diag - w, y - diag - h, w, h};
    case Slot::LowerLeft: return {x - diag - w, y + diag, w, h};
    case Slot::Above: return {x - w / 2, y - side - h, w, h};
    case Slot::Below: return {x - w / 2, y + side, w, h};
  }
  return {x, y, w, h};
}

bool insideScreen(const ScreenRect& r) {
  const int r_sq = radar::kTagScreenRadiusPx * radar::kTagScreenRadiusPx;
  const int right = r.left + r.w - 1;
  const int bottom = r.top + r.h - 1;
  return radar::distSqFromCenter(r.left, r.top) <= r_sq &&
         radar::distSqFromCenter(right, r.top) <= r_sq &&
         radar::distSqFromCenter(r.left, bottom) <= r_sq &&
         radar::distSqFromCenter(right, bottom) <= r_sq;
}

int overlapArea(const ScreenRect& a, const ScreenRect& b) {
  const int w = std::min(a.left + a.w, b.left + b.w) - std::max(a.left, b.left);
  const int h = std::min(a.top + a.h, b.top + b.h) - std::max(a.top, b.top);
  return (w > 0 && h > 0) ? w * h : 0;
}

bool circleHitsRect(const Circle& c, const ScreenRect& r) {
  const int nx = std::max(r.left, std::min(c.x, r.left + r.w - 1));
  const int ny = std::max(r.top, std::min(c.y, r.top + r.h - 1));
  const int dx = c.x - nx;
  const int dy = c.y - ny;
  return dx * dx + dy * dy < c.r * c.r;
}

/** Liang–Barsky clip of the segment against the rect grown by the line's half-width. */
bool segmentHitsRect(const Segment& s, const ScreenRect& r) {
  const float x_min = r.left - s.half_width;
  const float x_max = r.left + r.w + s.half_width;
  const float y_min = r.top - s.half_width;
  const float y_max = r.top + r.h + s.half_width;
  const float dx = s.x1 - s.x0;
  const float dy = s.y1 - s.y0;
  const float p[4] = {-dx, dx, -dy, dy};
  const float q[4] = {s.x0 - x_min, x_max - s.x0, s.y0 - y_min, y_max - s.y0};
  float t0 = 0.0f;
  float t1 = 1.0f;
  for (int i = 0; i < 4; ++i) {
    if (p[i] == 0.0f) {
      if (q[i] < 0.0f) {
        return false;
      }
      continue;
    }
    const float t = q[i] / p[i];
    if (p[i] < 0.0f) {
      if (t > t1) {
        return false;
      }
      t0 = std::max(t0, t);
    } else {
      if (t < t0) {
        return false;
      }
      t1 = std::min(t1, t);
    }
  }
  return true;
}

bool hitsHardObstacle(const ScreenRect& r, int owner) {
  for (size_t i = 0; i < s_rect_count; ++i) {
    if (overlapArea(r, s_store->rects[i]) > 0) {
      return true;
    }
  }
  for (size_t i = 0; i < s_circle_count; ++i) {
    const Circle& c = s_store->circles[i];
    if (c.owner != owner && circleHitsRect(c, r)) {
      return true;
    }
  }
  for (size_t i = 0; i < s_segment_count; ++i) {
    if (segmentHitsRect(s_store->segments[i], r)) {
      return true;
    }
  }
  return false;
}

int softCost(const ScreenRect& r) {
  int cost = 0;
  for (size_t i = 0; i < s_soft_rect_count; ++i) {
    const int area = overlapArea(r, s_store->soft_rects[i]);
    if (area > 0) {
      cost += kSoftOverlapCost + area / kSoftOverlapAreaDiv;
    }
  }
  return cost;
}

Memory* findMemory(const char* hex) {
  if (hex == nullptr || hex[0] == '\0') {
    return nullptr;
  }
  for (Memory& m : s_store->memory) {
    if (m.hex[0] != '\0' && strcmp(m.hex, hex) == 0) {
      return &m;
    }
  }
  return nullptr;
}

void remember(const char* hex, Memory* existing, Slot slot, int anchor_x, int anchor_y,
              bool full) {
  Memory* m = existing;
  if (m == nullptr) {
    if (hex == nullptr || hex[0] == '\0') {
      return;
    }
    // A free entry, else the one seen longest ago.
    m = &s_store->memory[0];
    for (Memory& candidate : s_store->memory) {
      if (candidate.hex[0] == '\0') {
        m = &candidate;
        break;
      }
      if (candidate.last_frame < m->last_frame) {
        m = &candidate;
      }
    }
    strncpy(m->hex, hex, sizeof(m->hex) - 1);
    m->hex[sizeof(m->hex) - 1] = '\0';
    m->pending_frames = 0;
  }
  m->slot = slot;
  m->anchor_x = anchor_x;
  m->anchor_y = anchor_y;
  m->full = full;
  m->last_frame = s_frame;
}

}  // namespace

Align slotAlign(Slot slot) {
  switch (slot) {
    case Slot::Left:
    case Slot::UpperLeft:
    case Slot::LowerLeft:
      return Align::Right;
    case Slot::Above:
    case Slot::Below:
      return Align::Center;
    default:
      return Align::Left;
  }
}

bool wasFull(const char* hex) {
  if (s_store == nullptr) {
    return false;
  }
  const Memory* m = findMemory(hex);
  return m != nullptr && m->full;
}

void beginFrame() {
  if (s_store == nullptr) {
    s_store = static_cast<Storage*>(
        heap_caps_calloc(1, sizeof(Storage), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_store == nullptr) {
      Serial.println("tags: no PSRAM for the layout, tags hidden");
      return;
    }
  }
  ++s_frame;
  s_rect_count = 0;
  s_soft_rect_count = 0;
  s_circle_count = 0;
  s_segment_count = 0;
  for (Memory& m : s_store->memory) {
    if (m.hex[0] != '\0' && s_frame - m.last_frame > kForgetAfterFrames) {
      m.hex[0] = '\0';
    }
  }
}

void addRect(const ScreenRect& rect) {
  if (s_store != nullptr && s_rect_count < kMaxRects) {
    s_store->rects[s_rect_count++] = rect;
  }
}

void addCircle(int x, int y, int radius, int owner) {
  if (s_store != nullptr && s_circle_count < kMaxCircles) {
    s_store->circles[s_circle_count++] = {x, y, radius, owner};
  }
}

void addSegment(int x0, int y0, int x1, int y1, float half_width) {
  if (s_store != nullptr && s_segment_count < kMaxSegments) {
    s_store->segments[s_segment_count++] = {static_cast<float>(x0), static_cast<float>(y0),
                                            static_cast<float>(x1), static_cast<float>(y1),
                                            half_width};
  }
}

void addSoftRect(const ScreenRect& rect) {
  if (s_store != nullptr && s_soft_rect_count < kMaxSoftRects) {
    s_store->soft_rects[s_soft_rect_count++] = rect;
  }
}

bool place(const TagRequest& request, ScreenRect* out, Slot* slot) {
  if (s_store == nullptr) {
    return false;
  }
  Memory* memory = findMemory(request.hex);
  // Hold last frame's anchor while the icon is within kTagHoldPx of it.
  TagRequest req = request;
  if (memory != nullptr && fabsf(req.fx - memory->anchor_x) <= kTagHoldPx &&
      fabsf(req.fy - memory->anchor_y) <= kTagHoldPx) {
    req.x = memory->anchor_x;
    req.y = memory->anchor_y;
  }
  // The preferred side faces the centre.
  const bool mirror = req.x >= radar::kCenterX;

  float track_dx = 0.0f;
  float track_dy = 0.0f;
  if (req.moving) {
    constexpr float kDegToRad = 0.01745329252f;
    track_dx = sinf(req.track_deg * kDegToRad);
    track_dy = -cosf(req.track_deg * kDegToRad);
  }

  int best_cost = INT_MAX;
  Slot best_slot = Slot::Right;
  ScreenRect best_rect{};
  bool kept_fits = false;  // last frame's slot is still free
  ScreenRect kept_rect{};
  for (int rank = 0; rank < kSlotCount; ++rank) {
    const Slot candidate = mirror ? mirrored(kOrder[rank]) : kOrder[rank];
    const ScreenRect rect = slotRect(req, candidate);
    if (!insideScreen(rect) || hitsHardObstacle(rect, req.owner)) {
      continue;
    }

    int cost = rank * kOrderStepCost + softCost(rect);
    if (req.moving) {
      float dx = 0.0f;
      float dy = 0.0f;
      slotDirection(candidate, &dx, &dy);
      const float along = dx * track_dx + dy * track_dy;
      if (along > 0.0f) {
        cost += static_cast<int>(lroundf(kVectorSideCost * along));
      }
    }
    if (memory != nullptr && memory->slot == candidate) {
      cost -= kKeepSlotBonus;
      kept_fits = true;
      kept_rect = rect;
    }
    if (cost < best_cost) {
      best_cost = cost;
      best_slot = candidate;
      best_rect = rect;
    }
  }

  if (best_cost == INT_MAX) {
    return false;
  }
  // Stay in a slot that still fits until the same better slot has won kSwitchAfterFrames in
  // a row. A blocked slot is left at once.
  if (memory != nullptr) {
    if (kept_fits && best_slot != memory->slot) {
      if (memory->pending_frames > 0 && memory->pending_slot == best_slot) {
        ++memory->pending_frames;
      } else {
        memory->pending_slot = best_slot;
        memory->pending_frames = 1;
      }
      if (memory->pending_frames < kSwitchAfterFrames) {
        best_slot = memory->slot;
        best_rect = kept_rect;
      } else {
        memory->pending_frames = 0;
      }
    } else {
      memory->pending_frames = 0;
    }
  }
  addRect(best_rect);
  remember(req.hex, memory, best_slot, req.x, req.y, req.full);
  *out = best_rect;
  *slot = best_slot;
  return true;
}

}  // namespace ui::tags
