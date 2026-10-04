#include "ui/radar_shapes.h"

#include <cmath>
#include <cstring>

#include "hardware/display.h"
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

template <size_t N>
constexpr size_t countOf(const Tri (&)[N]) {
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
constexpr Shape kHouse{kHouseTris, countOf(kHouseTris), nullptr, 0};
constexpr Shape kHouseDoor{kHouseDoorTris, countOf(kHouseDoorTris), nullptr, 0};

/** Maps design (fwd, right) to screen px for one bearing. */
class Placer {
 public:
  Placer(int x, int y, float bearing_deg, float scale) : x_(x), y_(y) {
    constexpr float kDegToRad = 0.01745329252f;
    const float rad = bearing_deg * kDegToRad;
    const float s = kElementScale * scale;
    // Screen y grows downward: forward is (sin, -cos), right of forward is (cos, sin).
    fx_ = sinf(rad) * s;
    fy_ = -cosf(rad) * s;
    rx_ = cosf(rad) * s;
    ry_ = sinf(rad) * s;
  }

  void map(const Point& p, float mirror, int* out_x, int* out_y) const {
    const float right = p.right * mirror;
    *out_x = x_ + static_cast<int>(lroundf(fx_ * p.fwd + rx_ * right));
    *out_y = y_ + static_cast<int>(lroundf(fy_ * p.fwd + ry_ * right));
  }

 private:
  int x_;
  int y_;
  float fx_;
  float fy_;
  float rx_;
  float ry_;
};

void fillShape(int x, int y, float bearing_deg, float scale, const Shape& shape,
               uint16_t color) {
  const Placer placer(x, y, bearing_deg, scale);
  for (const float mirror : {1.0f, -1.0f}) {
    for (size_t i = 0; i < shape.tri_count; ++i) {
      const Tri& t = shape.tris[i];
      int ax, ay, bx, by, cx, cy;
      placer.map(t.a, mirror, &ax, &ay);
      placer.map(t.b, mirror, &bx, &by);
      placer.map(t.c, mirror, &cx, &cy);
      canvas.fillTriangle(ax, ay, bx, by, cx, cy, color);
    }
  }
  // Circles are drawn once each; every one in these shapes sits on the centreline.
  for (size_t i = 0; i < shape.circle_count; ++i) {
    const Circle& c = shape.circles[i];
    int cx, cy;
    placer.map(c.center, 1.0f, &cx, &cy);
    canvas.fillSmoothCircle(cx, cy, px(c.radius * scale), color);
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

void drawAircraftIcon(int x, int y, float bearing_deg, IconShape shape, uint16_t color) {
  if (shape == IconShape::Balloon) {
    bearing_deg = 0.0f;
  }
  fillShape(x, y, bearing_deg, kAircraftIconScale, shapeFor(shape), color);
}

void drawRimArrow(int x, int y, float bearing_deg, uint16_t color) {
  fillShape(x, y, bearing_deg, 1.0f, kRimArrow, color);
}

void drawHomeMarker(int x, int y, uint16_t color, uint16_t door_color) {
  fillShape(x, y, 0.0f, kHomeMarkerScale, kHouse, color);
  fillShape(x, y, 0.0f, kHomeMarkerScale, kHouseDoor, door_color);
}

}  // namespace ui::radar
