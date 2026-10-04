#pragma once

#include <cstddef>

namespace services::clock {

/** Load the time zone and 12/24-hour setting from NVS and apply the time zone. */
void init();
/**
 * Starts SNTP the first time Wi-Fi is up (SNTP then keeps resyncing on its own). In
 * automatic mode it also looks up the radar location's time zone when the location has
 * changed since the last lookup. That lookup is a blocking HTTPS request (a second or two),
 * retried every config::kClockZoneRetryMs on failure.
 */
void loop();

/** True once SNTP has set the clock. */
bool timeValid();

/**
 * Current local time as "14:05" (24 h) or "2:05" plus suffix "PM" (12 h; suffix empty in
 * 24 h). False until the time is valid.
 */
bool formatTime(char* hhmm, size_t hhmm_len, char* suffix, size_t suffix_len);
/** Minute of the day (0..1439) for spotting when the display needs a redraw; -1 if not valid. */
int minuteOfDay();

/** Fixed time zones offered in the portal after "Automatic", in display order. */
size_t zoneCount();
const char* zoneLabel(size_t index);
/** True when the zone follows the radar location (the default). */
bool zoneAutomatic();
/** Index of the chosen fixed zone; meaningless when zoneAutomatic(). */
size_t currentZoneIndex();
bool use24Hour();

/** Portal values: "auto" or a zone index as text, and the 24-hour checkbox. */
void saveFromPortal(const char* zone_value, const char* checkbox_24h);
/** Back to automatic and 24 h (with the Wi-Fi credential wipe). */
void reset();

}  // namespace services::clock
