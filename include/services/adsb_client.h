#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

enum class AltState : uint8_t { Unknown, Ground, Airborne };

/** readsb "emergency" values; Unknown when the field is missing or unrecognised. */
enum class Emergency : uint8_t {
  Unknown,
  None,
  General,
  Lifeguard,
  MinFuel,
  NoRadio,
  Unlawful,
  Downed,
};

/** ADS-B emitter category ("A0".."C7") packed as (letter - 'A') << 4 | digit; 0 = unknown. */
constexpr uint8_t categoryCode(char letter, uint8_t digit) {
  return static_cast<uint8_t>(((letter - 'A') << 4) | digit);
}
constexpr uint8_t kCategoryUnknown = 0;  // "A0" (no info) also encodes as 0

/** dbFlags bits from adsb.fi. */
constexpr uint8_t kDbFlagMilitary = 0x01;

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;  // 0 when has_track is false
  float gs_knots;
  bool has_track;   // false if the message had no track or heading
  char hex[7];      // ICAO 24-bit address, lowercase hex
  uint8_t category; // see categoryCode()
  AltState alt_state;
  int32_t alt_ft;   // valid when alt_state == Airborne
  bool has_vrate;
  int16_t vrate_fpm;
  float seen_pos_s; // age of the position when fetched; 0 if not reported
  char squawk[5];
  Emergency emergency;
  uint8_t db_flags;
  char callsign[9];
  char type[5];
  char alt[12];     // formatted for the tag ("12345 ft", "GND")
};

constexpr size_t kMaxAircraft = 64;

/** Outcome of the most recent fetch. adsb.fi errors are bare HTTP statuses, so these come from the transport. */
enum class FetchStatus : uint8_t {
  Pending,         // nothing fetched since boot or since the last invalidate()
  Ok,
  NoConnection,    // couldn't open a connection (no internet, DNS or TLS failure)
  Timeout,         // connected, but no response in time
  ConnectionLost,  // connection dropped mid-request, or another client error
  RateLimited,     // HTTP 429
  ServerError,     // HTTP 5xx
  HttpError,       // any other non-200 status
  BadResponse,     // empty body, invalid JSON, or a "msg" other than "No error"
};

size_t aircraftCount();
const Aircraft* aircraftList();

FetchStatus lastStatus();
/** HTTP status, or the negative HTTPClient error code, of the last failed fetch. */
int lastErrorCode();
/** Extra text for BadResponse (parse error or the API's "msg"); empty otherwise. */
const char* lastErrorDetail();

/** True if the last successful fetch is recent enough to draw (config::kAdsbStaleAfterMs). */
bool aircraftFresh();

/** Drop the aircraft list and go back to Pending, e.g. after Wi-Fi drops. */
void invalidate();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

}  // namespace services::adsb
