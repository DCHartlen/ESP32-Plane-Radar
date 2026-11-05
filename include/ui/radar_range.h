#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::radar {

/**
 * Range presets (label on ring 3 = ¾ of outer radius).
 *
 * Recommended for ADS-B on a 1.28″ display:
 *   5 km  — pattern / very local (airfield vicinity)
 *  10 km  — default; neighborhood spotting
 *  15 km  — wider local area
 *  25 km  — metro / regional picture
 *
 * Outer radius (for aircraft math) is ring-3 distance ÷ 0.75.
 */
struct RangePreset {
  /** Distance shown on ring 3 (¾ of outer radius), always stored in km. */
  float ring3_km;
  float outer_km;
  /**
   * Speed vector: from the aircraft to where it will be after this many seconds at its
   * groundspeed and track. Longer at wider ranges so a jet's line stays ~80–100 px.
   */
  float track_horizon_s;
  /** The nearest this many aircraft get a full tag; the rest get the callsign only. */
  uint8_t max_full_tags;
};

constexpr float kRing3ToOuterKm = 4.0f / 3.0f;

constexpr RangePreset kRangePresets[] = {
    {5.0f, 5.0f * kRing3ToOuterKm, 15.0f, 12},
    {10.0f, 10.0f * kRing3ToOuterKm, 25.0f, 10},
    {15.0f, 15.0f * kRing3ToOuterKm, 40.0f, 8},
    {25.0f, 25.0f * kRing3ToOuterKm, 60.0f, 6},
};

constexpr size_t kRangePresetCount =
    sizeof(kRangePresets) / sizeof(kRangePresets[0]);

/**
 * Weather mode's presets. The radar image is always fetched for the widest one, and the
 * others show its centre at 2x and 4x the scale (weather_display), so each ring-3 distance
 * must be the widest one divided by a power of two. Aircraft fields are unused.
 */
constexpr RangePreset kWeatherRangePresets[] = {
    {25.0f, 25.0f * kRing3ToOuterKm, 0.0f, 0},
    {50.0f, 50.0f * kRing3ToOuterKm, 0.0f, 0},
    {100.0f, 100.0f * kRing3ToOuterKm, 0.0f, 0},
};

constexpr size_t kWeatherRangePresetCount =
    sizeof(kWeatherRangePresets) / sizeof(kWeatherRangePresets[0]);

/** What the screen shows; each mode keeps its own range. */
enum class Mode : uint8_t { Plane, Weather };

/** Load the saved mode, ranges and distance units from flash. Call once after boot. */
void rangeInit();
Mode mode();
/** Switches mode (saved like a range change). */
void setMode(Mode mode);
/** Next larger preset of the current mode (zoom out), clamped at the last. False if already there. */
bool rangeNext();
/** Next smaller preset of the current mode (zoom in), clamped at the first. False if already there. */
bool rangePrev();
/** Saves the mode and ranges to flash once kRangeSaveDelayMs has passed since the last change. */
void rangeSaveIfDue();
/** The current mode's preset; the projection, runways and landmarks all follow it. */
const RangePreset& rangeCurrent();
uint8_t rangeIndex();
/** ADSB fetch radius (km): scaled to screen edge so beyond-ring dots have data. */
float fetchRadiusKm();
/** Half the width of the screen in km at a preset (centre to the edge of the square). */
float screenHalfWidthKm(const RangePreset& preset);

bool useMiles();
bool showRunways();
/** Shorelines, rivers and the border (landmark_overlay). */
bool showLandmarks();
/** WiFi portal checkbox: "T" = miles, otherwise km. */
void saveMilesFromPortal(const char* checkbox_value);
void saveRunwaysFromPortal(const char* checkbox_value);
void saveLandmarksFromPortal(const char* checkbox_value);
void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles);
void formatCurrentRing3Label(char* buf, size_t len);
/** Reset distance units to km and the overlays to on (e.g. with WiFi credential wipe). */
void unitsReset();

}  // namespace ui::radar
