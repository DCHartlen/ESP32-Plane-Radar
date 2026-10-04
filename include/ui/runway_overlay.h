#pragma once

#include <LovyanGFX.hpp>

#include <cstddef>

#include "ui/radar_geometry.h"

namespace ui::runway {

void drawLargeAirportRunways(lgfx::LGFXBase& gfx);

/** Airport labels drawn by the last drawLargeAirportRunways(); aircraft tags try to avoid them. */
size_t airportLabelCount();
radar::ScreenRect airportLabelBox(size_t i);

}  // namespace ui::runway
