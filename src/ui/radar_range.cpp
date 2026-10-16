#include "ui/radar_range.h"

#include "config.h"
#include "ui/radar_theme.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui::radar {

namespace {

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsWeatherRangeKey[] = "wxRangeIdx";
constexpr char kPrefsModeKey[] = "mode";
constexpr char kPrefsMilesKey[] = "useMiles";
constexpr char kPrefsRunwaysKey[] = "showRwys";
constexpr char kPrefsLandmarksKey[] = "showLand";
constexpr uint8_t kDefaultRangeIndex = 1;         // 10 km ring
constexpr uint8_t kDefaultWeatherRangeIndex = 1;  // 50 km ring
constexpr float kKmPerMile = 1.609344f;

static_assert(kWeatherRangePresetCount > 0 &&
                  kWeatherRangePresets[kWeatherRangePresetCount - 1].ring3_km ==
                      kWeatherRangePresets[0].ring3_km * (1 << (kWeatherRangePresetCount - 1)),
              "weather presets must halve from the widest one");

Preferences s_prefs;
Mode s_mode = Mode::Plane;
uint8_t s_range_index = kDefaultRangeIndex;
uint8_t s_weather_range_index = kDefaultWeatherRangeIndex;
bool s_use_miles = false;
bool s_show_runways = true;
bool s_show_landmarks = true;
bool s_range_save_pending = false;
unsigned long s_range_changed_ms = 0;

/** The current mode's preset list, its length, and its index. */
const RangePreset* presets() {
  return s_mode == Mode::Weather ? kWeatherRangePresets : kRangePresets;
}

size_t presetCount() {
  return s_mode == Mode::Weather ? kWeatherRangePresetCount : kRangePresetCount;
}

uint8_t& currentIndex() {
  return s_mode == Mode::Weather ? s_weather_range_index : s_range_index;
}

void markRangeChanged() {
  s_range_save_pending = true;
  s_range_changed_ms = millis();
}

void saveModeAndRanges() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putUChar(kPrefsModeKey, static_cast<uint8_t>(s_mode));
  s_prefs.putUChar(kPrefsRangeKey, s_range_index);
  s_prefs.putUChar(kPrefsWeatherRangeKey, s_weather_range_index);
  s_prefs.end();
}

void saveUseMiles() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsMilesKey, s_use_miles);
  s_prefs.end();
}

void saveShowRunways() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsRunwaysKey, s_show_runways);
  s_prefs.end();
}

void saveShowLandmarks() {
  if (!s_prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  s_prefs.putBool(kPrefsLandmarksKey, s_show_landmarks);
  s_prefs.end();
}

bool portalCheckboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  // WiFiManager checkbox submits its value= attribute ("T", or "F" if we prefilled F).
  if ((value[0] == 'T' || value[0] == 't' || value[0] == 'F' || value[0] == 'f') &&
      value[1] == '\0') {
    return true;
  }
  return strcmp(value, "on") == 0;
}

}  // namespace

void rangeInit() {
  if (!s_prefs.begin(kPrefsNamespace, true)) {
    return;
  }
  const uint8_t saved = s_prefs.getUChar(kPrefsRangeKey, kDefaultRangeIndex);
  s_range_index =
      (saved < kRangePresetCount) ? saved : kDefaultRangeIndex;
  const uint8_t saved_wx = s_prefs.getUChar(kPrefsWeatherRangeKey, kDefaultWeatherRangeIndex);
  s_weather_range_index =
      (saved_wx < kWeatherRangePresetCount) ? saved_wx : kDefaultWeatherRangeIndex;
  s_mode = s_prefs.getUChar(kPrefsModeKey, 0) == static_cast<uint8_t>(Mode::Weather)
               ? Mode::Weather
               : Mode::Plane;
  s_use_miles = s_prefs.getBool(kPrefsMilesKey, false);
  s_show_runways = s_prefs.getBool(kPrefsRunwaysKey, true);
  s_show_landmarks = s_prefs.getBool(kPrefsLandmarksKey, true);
  s_prefs.end();
}

Mode mode() { return s_mode; }

void setMode(Mode mode) {
  if (mode == s_mode) {
    return;
  }
  s_mode = mode;
  markRangeChanged();
}

bool rangeNext() {
  uint8_t& index = currentIndex();
  if (index + 1u >= presetCount()) {
    return false;
  }
  ++index;
  markRangeChanged();
  return true;
}

bool rangePrev() {
  uint8_t& index = currentIndex();
  if (index == 0) {
    return false;
  }
  --index;
  markRangeChanged();
  return true;
}

void rangeSaveIfDue() {
  if (s_range_save_pending && millis() - s_range_changed_ms >= config::kRangeSaveDelayMs) {
    s_range_save_pending = false;
    Serial.printf("Mode and range saved to NVS at %lu ms\n", millis());
    saveModeAndRanges();
  }
}

const RangePreset& rangeCurrent() { return presets()[currentIndex()]; }

uint8_t rangeIndex() { return currentIndex(); }

float fetchRadiusKm() {
  const float outer_km = rangeCurrent().outer_km;
  const float screen_r_px =
      static_cast<float>(kCenterX - kBeyondRingScreenMarginPx);
  return outer_km * (screen_r_px / static_cast<float>(kGridOuterRadius));
}

float screenHalfWidthKm(const RangePreset& preset) {
  return preset.outer_km * static_cast<float>(kCenterX) / static_cast<float>(kGridOuterRadius);
}

bool useMiles() { return s_use_miles; }

bool showRunways() { return s_show_runways; }

bool showLandmarks() { return s_show_landmarks; }

void saveMilesFromPortal(const char* checkbox_value) {
  s_use_miles = portalCheckboxChecked(checkbox_value);
  saveUseMiles();
  Serial.printf("Distance units: %s\n", s_use_miles ? "miles" : "km");
}

void saveRunwaysFromPortal(const char* checkbox_value) {
  s_show_runways = portalCheckboxChecked(checkbox_value);
  saveShowRunways();
  Serial.printf("Runway overlay: %s\n", s_show_runways ? "on" : "off");
}

void saveLandmarksFromPortal(const char* checkbox_value) {
  s_show_landmarks = portalCheckboxChecked(checkbox_value);
  saveShowLandmarks();
  Serial.printf("Landmark overlay: %s\n", s_show_landmarks ? "on" : "off");
}

void formatRing3Label(char* buf, size_t len, float ring3_km, bool use_miles) {
  if (use_miles) {
    const int mi = static_cast<int>(lroundf(ring3_km / kKmPerMile));
    snprintf(buf, len, "%dmi", mi);
  } else {
    const int km = static_cast<int>(lroundf(ring3_km));
    snprintf(buf, len, "%dkm", km);
  }
}

void formatCurrentRing3Label(char* buf, size_t len) {
  formatRing3Label(buf, len, rangeCurrent().ring3_km, s_use_miles);
}

void unitsReset() {
  s_use_miles = false;
  s_show_runways = true;
  s_show_landmarks = true;
  if (s_prefs.begin(kPrefsNamespace, false)) {
    s_prefs.remove(kPrefsMilesKey);
    s_prefs.remove(kPrefsRunwaysKey);
    s_prefs.remove(kPrefsLandmarksKey);
    s_prefs.end();
  }
}

}  // namespace ui::radar
