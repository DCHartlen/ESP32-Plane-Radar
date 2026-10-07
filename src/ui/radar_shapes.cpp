#include "ui/radar_shapes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "hardware/display.h"
#include "ui/canvas_pixels.h"
#include "ui/radar_theme.h"
#include "ui/ui_scale.h"

namespace ui::radar {

namespace {

using services::adsb::categoryCode;

/**
 * Shape coordinates are in design px (before px()): fwd along the bearing, right to its
 * right. Every shape is symmetric, so only the right half is listed; each triangle and
 * circle is drawn again with right negated. A triangle that crosses the centreline (the
 * rotor bar) mirrors into a second, crossing one.
 */
struct Point {
  float fwd;
  float right;
};

struct Tri {
  Point a;
  Point b;
  Point c;
};

struct Circle {
  Point center;
  float radius;
};

struct Shape {
  const Tri* tris;
  size_t tri_count;
  const Circle* circles;
  size_t circle_count;
};

template <typename T, size_t N>
constexpr size_t countOf(const T (&)[N]) {
  return N;
}

// Radii here (before kAircraftIconScale) must stay within the matching kIconRadius*
// constant in radar_theme.h.

/** Today's plain triangle, for unknown categories. Radius 12. */
constexpr Tri kGenericTris[] = {
    {{12.0f, 0.0f}, {-4.5f, 6.0f}, {-4.5f, 0.0f}},
};

/**
 * Wide-body twin (ICAO code E: 777, 787, A350): span about equal to the length, wide
 * fuselage, deep wing root with a trailing-edge kink, an engine pod under each wing.
 * Radius ~14.1 (wing tip and tailplane tip).
 */
constexpr Tri kJetTris[] = {
    // Blunt nose, fuselage, tail cone.
    {{13.0f, 0.0f}, {12.0f, 1.4f}, {12.0f, 0.0f}},
    {{12.0f, 0.0f}, {12.0f, 1.4f}, {10.5f, 2.2f}},
    {{12.0f, 0.0f}, {10.5f, 2.2f}, {10.5f, 0.0f}},
    {{10.5f, 0.0f}, {10.5f, 2.2f}, {-9.0f, 2.2f}},
    {{10.5f, 0.0f}, {-9.0f, 2.2f}, {-9.0f, 0.0f}},
    {{-9.0f, 0.0f}, {-9.0f, 2.2f}, {-12.5f, 0.7f}},
    {{-9.0f, 0.0f}, {-12.5f, 0.7f}, {-12.5f, 0.0f}},
    // Wing, fanned from the leading-edge root: tip, then the trailing-edge kink.
    {{4.5f, 2.2f}, {-4.5f, 12.5f}, {-6.5f, 12.5f}},
    {{4.5f, 2.2f}, {-6.5f, 12.5f}, {-3.5f, 5.5f}},
    {{4.5f, 2.2f}, {-3.5f, 5.5f}, {-3.0f, 2.2f}},
    // Engine pod, sticking out ahead of the leading edge.
    {{4.0f, 4.8f}, {4.0f, 6.2f}, {0.5f, 6.2f}},
    {{4.0f, 4.8f}, {0.5f, 6.2f}, {0.5f, 4.8f}},
    // Tailplane.
    {{-8.5f, 1.8f}, {-11.8f, 5.8f}, {-12.8f, 5.8f}},
    {{-8.5f, 1.8f}, {-12.8f, 5.8f}, {-11.5f, 1.8f}},
};

/**
 * Business jet / rear-engine regional: slim fuselage, small swept wing with nothing on
 * it, engine pods on the rear fuselage, T-tail at the very back. Radius ~13.7 (tailplane tip).
 */
constexpr Tri kBizJetTris[] = {
    // Pointed nose, fuselage, tail cone.
    {{12.0f, 0.0f}, {9.5f, 1.5f}, {9.5f, 0.0f}},
    {{9.5f, 0.0f}, {9.5f, 1.5f}, {-8.0f, 1.5f}},
    {{9.5f, 0.0f}, {-8.0f, 1.5f}, {-8.0f, 0.0f}},
    {{-8.0f, 0.0f}, {-8.0f, 1.5f}, {-11.5f, 0.5f}},
    {{-8.0f, 0.0f}, {-11.5f, 0.5f}, {-11.5f, 0.0f}},
    // Wing: 10 half-span, swept, clean.
    {{2.5f, 1.5f}, {-3.0f, 10.0f}, {-4.5f, 10.0f}},
    {{2.5f, 1.5f}, {-4.5f, 10.0f}, {-2.5f, 1.5f}},
    // Engine pod against the rear fuselage.
    {{-4.5f, 1.5f}, {-4.5f, 3.4f}, {-8.0f, 3.4f}},
    {{-4.5f, 1.5f}, {-8.0f, 3.4f}, {-8.0f, 1.5f}},
    // T-tail: the tailplane sits on top of the swept fin, so it reaches past the tail cone.
    {{-9.5f, 0.5f}, {-11.8f, 5.0f}, {-12.8f, 5.0f}},
    {{-9.5f, 0.5f}, {-12.8f, 5.0f}, {-11.5f, 0.5f}},
};

/**
 * Light aircraft: stubby. Thick fuselage, short broad wing (span about the length).
 * Radius ~11.6 (wing tip).
 */
constexpr Tri kLightTris[] = {
    {{10.0f, 0.0f}, {8.5f, 2.0f}, {8.5f, 0.0f}},
    {{8.5f, 0.0f}, {8.5f, 2.0f}, {-9.0f, 0.9f}},
    {{8.5f, 0.0f}, {-9.0f, 0.9f}, {-9.0f, 0.0f}},
    // Wing: 4 deep, 10.5 half-span.
    {{5.0f, 2.0f}, {5.0f, 10.5f}, {1.0f, 10.5f}},
    {{5.0f, 2.0f}, {1.0f, 10.5f}, {1.0f, 2.0f}},
    // Tailplane.
    {{-6.0f, 0.9f}, {-6.5f, 4.5f}, {-9.0f, 4.5f}},
    {{-6.0f, 0.9f}, {-9.0f, 4.5f}, {-9.0f, 0.9f}},
};

/**
 * Glider: the opposite of the light aircraft. Hairline fuselage and a very long, thin,
 * tapered wing (span about 1.5x the length). Radius ~14.1 (wing tip).
 */
constexpr Tri kGliderTris[] = {
    {{9.0f, 0.0f}, {7.5f, 0.9f}, {7.5f, 0.0f}},
    {{7.5f, 0.0f}, {7.5f, 0.9f}, {-9.0f, 0.35f}},
    {{7.5f, 0.0f}, {-9.0f, 0.35f}, {-9.0f, 0.0f}},
    // Wing: 1.9 deep at the root, 1.1 at the tip, 14 half-span.
    {{2.6f, 0.9f}, {2.0f, 14.0f}, {0.9f, 14.0f}},
    {{2.6f, 0.9f}, {0.9f, 14.0f}, {0.7f, 0.9f}},
    // Small T-tail.
    {{-7.8f, 0.35f}, {-8.0f, 3.5f}, {-9.0f, 3.5f}},
    {{-7.8f, 0.35f}, {-9.0f, 3.5f}, {-9.0f, 0.35f}},
};

/** Cabin (circle), tail boom, tail rotor, and one rotor blade that mirrors into an X. Radius 13. */
constexpr Tri kHelicopterTris[] = {
    // Tail boom.
    {{-1.0f, 0.0f}, {-1.0f, 1.2f}, {-11.0f, 0.6f}},
    {{-1.0f, 0.0f}, {-11.0f, 0.6f}, {-11.0f, 0.0f}},
    // Tail rotor.
    {{-10.0f, 0.0f}, {-10.0f, 3.5f}, {-12.0f, 3.5f}},
    {{-10.0f, 0.0f}, {-12.0f, 3.5f}, {-12.0f, 0.0f}},
    // Main rotor blade: hub (2.5, 0), 11 long each way at 45 degrees, 1.8 wide.
    {{9.64f, 8.42f}, {10.92f, 7.14f}, {-4.64f, -8.42f}},
    {{9.64f, 8.42f}, {-4.64f, -8.42f}, {-5.92f, -7.14f}},
};
constexpr Circle kHelicopterCircles[] = {
    {{2.5f, 0.0f}, 4.5f},
};

/** Envelope, neck and basket; drawn upright. Radius 10. */
constexpr Tri kBalloonTris[] = {
    {{-1.5f, 0.0f}, {-1.5f, 5.0f}, {-6.0f, 0.0f}},
    {{-6.0f, 0.0f}, {-6.0f, 2.2f}, {-9.0f, 2.2f}},
    {{-6.0f, 0.0f}, {-9.0f, 2.2f}, {-9.0f, 0.0f}},
};
constexpr Circle kBalloonCircles[] = {
    {{2.5f, 0.0f}, 7.0f},
};

/** Rim arrow: tip, back corner, notch. Radius ~6.0, inside kBeyondRingMarkerRadiusPx. */
constexpr Tri kRimArrowTris[] = {
    {{6.0f, 0.0f}, {-4.0f, 4.5f}, {-2.0f, 0.0f}},
};

/** Climb/descend arrow: 6 tall, 7 wide (kClimbArrowWidthPx), centred. */
constexpr Tri kClimbArrowTris[] = {
    {{3.0f, 0.0f}, {-3.0f, 3.5f}, {-3.0f, 0.0f}},
};

/** House: roof and body, plus a door cut out in the background colour. Radius 5.5. */
constexpr Tri kHouseTris[] = {
    {{5.5f, 0.0f}, {1.0f, 5.0f}, {1.0f, 0.0f}},
    {{1.0f, 0.0f}, {1.0f, 3.5f}, {-4.5f, 3.5f}},
    {{1.0f, 0.0f}, {-4.5f, 3.5f}, {-4.5f, 0.0f}},
};
constexpr Tri kHouseDoorTris[] = {
    {{-1.0f, 0.0f}, {-1.0f, 1.1f}, {-4.5f, 1.1f}},
    {{-1.0f, 0.0f}, {-4.5f, 1.1f}, {-4.5f, 0.0f}},
};

constexpr Shape kGeneric{kGenericTris, countOf(kGenericTris), nullptr, 0};
constexpr Shape kJet{kJetTris, countOf(kJetTris), nullptr, 0};
constexpr Shape kBizJet{kBizJetTris, countOf(kBizJetTris), nullptr, 0};
constexpr Shape kLight{kLightTris, countOf(kLightTris), nullptr, 0};
constexpr Shape kGlider{kGliderTris, countOf(kGliderTris), nullptr, 0};
constexpr Shape kHelicopter{kHelicopterTris, countOf(kHelicopterTris), kHelicopterCircles, 1};
constexpr Shape kBalloon{kBalloonTris, countOf(kBalloonTris), kBalloonCircles, 1};
constexpr Shape kRimArrow{kRimArrowTris, countOf(kRimArrowTris), nullptr, 0};
constexpr Shape kClimbArrow{kClimbArrowTris, countOf(kClimbArrowTris), nullptr, 0};
constexpr Shape kHouse{kHouseTris, countOf(kHouseTris), nullptr, 0};
constexpr Shape kHouseDoor{kHouseDoorTris, countOf(kHouseDoorTris), nullptr, 0};

/** Rim dot for beyond-ring traffic with no track: kBeyondRingDotRadiusPx. */
constexpr Circle kRimDotCircles[] = {
    {{0.0f, 0.0f}, 4.0f},
};
constexpr Shape kRimDot{nullptr, 0, kRimDotCircles, countOf(kRimDotCircles)};

/**
 * Shapes are drawn anti-aliased at fractional positions, so an icon moving a fraction of a
 * pixel per frame shifts its edge shading instead of jumping a whole pixel now and then.
 * Each pixel is sampled on a 4x4 grid. A row of pixels keeps one bit per sample, so
 * overlapping triangles and circles merge before blending and their shared edges leave no
 * seams. Pixel p covers [p - 0.5, p + 0.5], as in drawThickLine().
 */
constexpr int kSamplesPerAxis = 4;
constexpr uint32_t kSampleCount = kSamplesPerAxis * kSamplesPerAxis;
/** Both halves of the largest shape (the jet, 2 x 14) fit. */
constexpr size_t kMaxShapeTris = 32;
constexpr size_t kMaxShapeCircles = 2;
/** Widest shape in px; anything past it is cut off. The jet is about 2 x 23. */
constexpr int kMaxShapeWidthPx = 96;

/** A triangle edge, stepped down the screen: x at y_top, and how x moves per px of y. */
struct Edge {
  float y_top;
  float y_bottom;
  float x_top;
  float dx_dy;
};

struct ScreenTri {
  Edge edges[3];
  float y_min;
  float y_max;
};

struct ScreenCircle {
  float x;
  float y;
  float r;
};

struct ScreenPoint {
  float x;
  float y;
};

Edge makeEdge(const ScreenPoint& a, const ScreenPoint& b) {
  const ScreenPoint& top = a.y <= b.y ? a : b;
  const ScreenPoint& bottom = a.y <= b.y ? b : a;
  const float dy = bottom.y - top.y;
  return {top.y, bottom.y, top.x, dy > 0.0f ? (bottom.x - top.x) / dy : 0.0f};
}

/** ceilf() without the library call, which is most of the cost of a span. */
inline int ceilToInt(float v) {
  const int i = static_cast<int>(v);  // truncates toward zero
  return i + (v > static_cast<float>(i) ? 1 : 0);
}

/** In sample row j of a pixel row starting at screen x = left, sets the samples in [x_lo, x_hi). */
inline void setSampleSpan(uint16_t* row, int left, int width, int j, float x_lo, float x_hi) {
  static_assert(kSamplesPerAxis == 4, "the nibble masks below assume 4 samples per axis");
  // Sample column k (counted from screen x = 0) is centred at (k + 0.5) / 4 - 0.5.
  const int origin = left * kSamplesPerAxis;
  const int k_first = std::max(ceilToInt(x_lo * 4.0f + 1.5f) - origin, 0);
  const int k_last = std::min(ceilToInt(x_hi * 4.0f + 1.5f) - 1 - origin, width * 4 - 1);
  if (k_first > k_last) {
    return;
  }
  const int shift = j * kSamplesPerAxis;
  const int p_first = k_first >> 2;
  const int p_last = k_last >> 2;
  const uint32_t head = (0xFu << (k_first & 3)) & 0xFu;  // samples from k_first on
  const uint32_t tail = 0xFu >> (3 - (k_last & 3));     // samples up to k_last
  if (p_first == p_last) {
    row[p_first] = static_cast<uint16_t>(row[p_first] | ((head & tail) << shift));
    return;
  }
  row[p_first] = static_cast<uint16_t>(row[p_first] | (head << shift));
  const uint16_t full = static_cast<uint16_t>(0xFu << shift);
  for (int p = p_first + 1; p < p_last; ++p) {
    row[p] = static_cast<uint16_t>(row[p] | full);
  }
  row[p_last] = static_cast<uint16_t>(row[p_last] | (tail << shift));
}

/** color over dst at coverage 1-15 sixteenths; shifts instead of the divides in blend565(). */
inline uint16_t blendCoverage(uint32_t dst, uint32_t color, uint32_t covered) {
  const uint32_t keep = kSampleCount - covered;
  const uint32_t r = ((dst >> 11) * keep + (color >> 11) * covered + 8) >> 4;
  const uint32_t g = (((dst >> 5) & 0x3F) * keep + ((color >> 5) & 0x3F) * covered + 8) >> 4;
  const uint32_t b = ((dst & 0x1F) * keep + (color & 0x1F) * covered + 8) >> 4;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void fillShape(float x, float y, float bearing_deg, float scale, const Shape& shape,
               uint16_t color) {
  uint16_t* fb = static_cast<uint16_t*>(canvas.getBuffer());
  if (fb == nullptr) {
    return;  // the canvas always has a framebuffer once the display is up
  }

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = bearing_deg * kDegToRad;
  const float s = kElementScale * scale;
  // Screen y grows downward: forward is (sin, -cos), right of forward is (cos, sin).
  const float fx = sinf(rad) * s;
  const float fy = -cosf(rad) * s;
  const float rx = cosf(rad) * s;
  const float ry = sinf(rad) * s;
  const auto map = [&](const Point& p, float mirror) {
    const float right = p.right * mirror;
    return ScreenPoint{x + fx * p.fwd + rx * right, y + fy * p.fwd + ry * right};
  };

  ScreenTri tris[kMaxShapeTris];
  ScreenCircle circles[kMaxShapeCircles];
  size_t tri_count = 0;
  size_t circle_count = 0;
  float min_x = x;
  float max_x = x;
  float min_y = y;
  float max_y = y;
  for (const float mirror : {1.0f, -1.0f}) {
    for (size_t i = 0; i < shape.tri_count && tri_count < kMaxShapeTris; ++i) {
      const Tri& t = shape.tris[i];
      const ScreenPoint a = map(t.a, mirror);
      const ScreenPoint b = map(t.b, mirror);
      const ScreenPoint c = map(t.c, mirror);
      ScreenTri& out = tris[tri_count++];
      out.edges[0] = makeEdge(a, b);
      out.edges[1] = makeEdge(b, c);
      out.edges[2] = makeEdge(c, a);
      out.y_min = std::min({a.y, b.y, c.y});
      out.y_max = std::max({a.y, b.y, c.y});
      min_x = std::min({min_x, a.x, b.x, c.x});
      max_x = std::max({max_x, a.x, b.x, c.x});
      min_y = std::min(min_y, out.y_min);
      max_y = std::max(max_y, out.y_max);
    }
  }
  // Circles are drawn once each; every one in these shapes sits on the centreline.
  for (size_t i = 0; i < shape.circle_count && circle_count < kMaxShapeCircles; ++i) {
    const Circle& c = shape.circles[i];
    const ScreenPoint center = map(c.center, 1.0f);
    const float r = c.radius * s;
    circles[circle_count++] = {center.x, center.y, r};
    min_x = std::min(min_x, center.x - r);
    max_x = std::max(max_x, center.x + r);
    min_y = std::min(min_y, center.y - r);
    max_y = std::max(max_y, center.y + r);
  }

  const int left = std::max(0, static_cast<int>(floorf(min_x + 0.5f)));
  const int right = std::min(kFbW - 1, static_cast<int>(ceilf(max_x - 0.5f)));
  const int top = std::max(0, static_cast<int>(floorf(min_y + 0.5f)));
  const int bottom = std::min(kFbH - 1, static_cast<int>(ceilf(max_y - 0.5f)));
  const int width = std::min(right - left + 1, kMaxShapeWidthPx);
  if (width <= 0) {
    return;
  }

  // Sample rows sit 1/8, 3/8, 5/8 and 7/8 of the way down each pixel row.
  constexpr float kFirstSampleRow = 0.5f / kSamplesPerAxis - 0.5f;
  constexpr float kLastSampleRow = 0.5f - 0.5f / kSamplesPerAxis;
  constexpr float kSampleRowStep = 1.0f / kSamplesPerAxis;
  /** Set bits in each 4-bit value, for counting a pixel's covered samples. */
  constexpr uint8_t kNibbleBits[16] = {0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4};

  uint16_t row[kMaxShapeWidthPx];
  for (int py = top; py <= bottom; ++py) {
    memset(row, 0, sizeof(row[0]) * width);
    bool any = false;
    const float sy_first = py + kFirstSampleRow;
    const float sy_last = py + kLastSampleRow;
    for (size_t t = 0; t < tri_count; ++t) {
      const ScreenTri& tri = tris[t];
      if (sy_last < tri.y_min || sy_first >= tri.y_max) {
        continue;  // no sample row of this pixel row crosses it
      }
      for (int j = 0; j < kSamplesPerAxis; ++j) {
        const float sy = sy_first + j * kSampleRowStep;
        if (sy < tri.y_min || sy >= tri.y_max) {
          continue;
        }
        float lo = 1e9f;
        float hi = -1e9f;
        for (const Edge& e : tri.edges) {
          // Half-open in y, so a sample row through a vertex counts it once.
          if (sy >= e.y_top && sy < e.y_bottom) {
            const float ex = e.x_top + (sy - e.y_top) * e.dx_dy;
            lo = std::min(lo, ex);
            hi = std::max(hi, ex);
          }
        }
        if (lo < hi) {
          setSampleSpan(row, left, width, j, lo, hi);
          any = true;
        }
      }
    }
    for (size_t c = 0; c < circle_count; ++c) {
      const ScreenCircle& circle = circles[c];
      if (sy_last < circle.y - circle.r || sy_first > circle.y + circle.r) {
        continue;
      }
      for (int j = 0; j < kSamplesPerAxis; ++j) {
        const float dy = sy_first + j * kSampleRowStep - circle.y;
        const float h_sq = circle.r * circle.r - dy * dy;
        if (h_sq > 0.0f) {
          const float h = sqrtf(h_sq);
          setSampleSpan(row, left, width, j, circle.x - h, circle.x + h);
          any = true;
        }
      }
    }
    if (!any) {
      continue;
    }
    for (int i = 0; i < width; ++i) {
      const uint32_t bits = row[i];
      if (bits == 0) {
        continue;
      }
      const uint32_t covered = kNibbleBits[bits & 0xF] + kNibbleBits[(bits >> 4) & 0xF] +
                               kNibbleBits[(bits >> 8) & 0xF] + kNibbleBits[bits >> 12];
      uint16_t& px = fb[fbIndex(left + i, py)];
      px = covered >= kSampleCount ? color : blendCoverage(px, color, covered);
    }
  }
}

const Shape& shapeFor(IconShape icon) {
  switch (icon) {
    case IconShape::Jet:
      return kJet;
    case IconShape::BizJet:
      return kBizJet;
    case IconShape::Light:
      return kLight;
    case IconShape::Glider:
      return kGlider;
    case IconShape::Helicopter:
      return kHelicopter;
    case IconShape::Balloon:
      return kBalloon;
    case IconShape::Generic:
      break;
  }
  return kGeneric;
}

/**
 * ICAO type prefixes of jets with rear-mounted engines: business jets, CRJs, ERJs,
 * Fokker 70/100, 717 and the MD-80/DC-9 family. Matched only against jet-weight
 * categories (or a missing one), so light aircraft codes that happen to share a prefix
 * never get here.
 */
constexpr const char* kRearEngineTypePrefixes[] = {
    "CRJ",  "E135", "E145", "E35L", "E45X", "F70",  "F100", "B712", "MD8",  "MD9",
    "DC9",  "C50",  "C51",  "C52",  "C55",  "C56",  "C65",  "C68",  "C70",  "C75",
    "CL30", "CL35", "CL60", "GL5T", "GL6T", "GL7T", "GLEX", "GLF",  "G150", "G200",
    "G280", "GALX", "ASTR", "WW24", "LJ",   "H25",  "HA4T", "E50P", "E55P", "PRM1",
    "BE40", "MU30", "FA",   "F2TH", "F900", "EA50", "SF50", "HDJT", "J328",
};

bool isRearEngineType(const char* type) {
  for (const char* prefix : kRearEngineTypePrefixes) {
    if (strncmp(type, prefix, strlen(prefix)) == 0) {
      return true;
    }
  }
  return false;
}

IconShape shapeFromType(const char* type) {
  if (strcmp(type, "BALL") == 0) {
    return IconShape::Balloon;
  }
  if (strcmp(type, "GLID") == 0) {
    return IconShape::Glider;
  }
  if (strcmp(type, "ULAC") == 0) {
    return IconShape::Light;
  }
  if (isRearEngineType(type)) {
    return IconShape::BizJet;
  }
  return IconShape::Generic;
}

}  // namespace

IconShape iconShapeFor(const services::adsb::Aircraft& plane) {
  switch (plane.category) {
    case categoryCode('A', 1):  // light, < 15 500 lb
    case categoryCode('B', 4):  // ultralight
      return IconShape::Light;
    case categoryCode('A', 2):  // small: business jets, but also turboprops like the Q400
    case categoryCode('A', 3):  // large
    case categoryCode('A', 4):  // B757-class wake
    case categoryCode('A', 5):  // heavy
    case categoryCode('A', 6):  // high performance
      // Weight class can't tell engine placement, so the type decides; the airliner icon
      // (wing engines, turboprops too) is the default.
      return isRearEngineType(plane.type) ? IconShape::BizJet : IconShape::Jet;
    case categoryCode('A', 7):
      return IconShape::Helicopter;
    case categoryCode('B', 1):
      return IconShape::Glider;
    case categoryCode('B', 2):  // lighter than air
      return IconShape::Balloon;
    default:
      return shapeFromType(plane.type);
  }
}

int iconRadiusPx(IconShape shape) {
  switch (shape) {
    case IconShape::Jet:
      return kIconRadiusJetPx;
    case IconShape::BizJet:
      return kIconRadiusBizJetPx;
    case IconShape::Light:
      return kIconRadiusLightPx;
    case IconShape::Glider:
      return kIconRadiusGliderPx;
    case IconShape::Helicopter:
      return kIconRadiusHelicopterPx;
    case IconShape::Balloon:
      return kIconRadiusBalloonPx;
    case IconShape::Generic:
      break;
  }
  return kIconRadiusGenericPx;
}

void drawAircraftIcon(float x, float y, float bearing_deg, IconShape shape, uint16_t color) {
  if (shape == IconShape::Balloon) {
    bearing_deg = 0.0f;
  }
  fillShape(x, y, bearing_deg, kAircraftIconScale, shapeFor(shape), color);
}

void drawRimArrow(float x, float y, float bearing_deg, uint16_t color) {
  fillShape(x, y, bearing_deg, 1.0f, kRimArrow, color);
}

void drawRimDot(float x, float y, uint16_t color) {
  fillShape(x, y, 0.0f, 1.0f, kRimDot, color);
}

void drawClimbArrow(int x, int y, bool climbing, uint16_t color) {
  fillShape(static_cast<float>(x), static_cast<float>(y), climbing ? 0.0f : 180.0f, 1.0f,
            kClimbArrow, color);
}

void drawHomeMarker(int x, int y, uint16_t color, uint16_t door_color) {
  const float fx = static_cast<float>(x);
  const float fy = static_cast<float>(y);
  fillShape(fx, fy, 0.0f, kHomeMarkerScale, kHouse, color);
  fillShape(fx, fy, 0.0f, kHomeMarkerScale, kHouseDoor, door_color);
}

}  // namespace ui::radar
