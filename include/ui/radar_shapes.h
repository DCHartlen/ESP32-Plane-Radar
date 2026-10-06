#pragma once

#include <cstdint>

#include "services/adsb_client.h"

namespace ui::radar {

/**
 * Aircraft silhouettes; picked from the ADS-B emitter category, refined by the ICAO type.
 * Jet is the airliner: anything with wing-mounted engines, turboprops included. BizJet is
 * rear-mounted engines and a T-tail: business jets, CRJs, ERJs, 717/MD-80s.
 */
enum class IconShape : uint8_t { Generic, Jet, BizJet, Light, Glider, Helicopter, Balloon };

IconShape iconShapeFor(const services::adsb::Aircraft& plane);

/** Screen radius (px) that covers the icon at any rotation; see the kIconRadius* constants. */
int iconRadiusPx(IconShape shape);

/** Filled icon centred on (x, y), rotated to bearing_deg (0 = north, clockwise). Balloons don't rotate. */
void drawAircraftIcon(int x, int y, float bearing_deg, IconShape shape, uint16_t color);

/** Notched arrow for beyond-ring traffic, centred on (x, y), pointing along bearing_deg. */
void drawRimArrow(int x, int y, float bearing_deg, uint16_t color);

/** Up (climbing) or down triangle centred on (x, y), kClimbArrowWidthPx wide. */
void drawClimbArrow(int x, int y, bool climbing, uint16_t color);

/** House at the radar centre; door_color is normally the background. */
void drawHomeMarker(int x, int y, uint16_t color, uint16_t door_color);

}  // namespace ui::radar
