#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  char callsign[9];
  char type[5];
  char alt[12];
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
