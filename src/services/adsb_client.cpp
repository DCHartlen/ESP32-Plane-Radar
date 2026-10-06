#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <sys/time.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "config.h"
#include "services/tls_lock.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr int kConnectAttemptMs = 200;
constexpr unsigned long kRequestTimeoutMs = 10000;

/** The fetch task's stack is in PSRAM; it never writes flash, which PSRAM stacks can't. */
constexpr uint32_t kTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;
/** adsb.fi allows 1 request/s; a range change can ask for a fetch right after another. */
constexpr unsigned long kMinFetchGapMs = 1100;

/** How one fetch went. Filled privately, then published under the mutex. */
struct Outcome {
  FetchStatus status = FetchStatus::Pending;
  int error_code = 0;
  char error_detail[sizeof(Snapshot::error_detail)] = "";
  size_t count = 0;
  unsigned long received_ms = 0;  // millis() when the body arrived
  int64_t received_epoch_ms = 0;  // wall clock at the same moment; 0 before SNTP sync
  double api_now_ms = 0.0;        // the response's "now" (server time); 0 if missing
  long lag_ms = 0;                // how old the server's snapshot was on arrival
  bool lag_measured = false;      // false: lag_ms is config::kAdsbDefaultLagMs
  unsigned long positions_ms = 0; // millis() at the server's "now"; seen_pos is relative to it
};

struct Request {
  double lat = 0.0;
  double lon = 0.0;
  float radius_km = 0.0f;
  bool valid = false;
};

// Everything below the mutex is shared between the fetch task and the main loop.
SemaphoreHandle_t s_mutex = nullptr;
Aircraft* s_aircraft = nullptr;  // published list, PSRAM
Aircraft* s_staging = nullptr;   // the fetch's private list, PSRAM
Outcome s_published;
unsigned long s_last_ok_ms = 0;  // 0 = no good fetch since boot or invalidate()
unsigned long s_positions_ms = 0;  // Outcome::positions_ms of the last good fetch
uint32_t s_generation = 0;       // bumped by invalidate()
uint32_t s_publish_count = 0;
Request s_request;
bool s_enabled = false;
TaskHandle_t s_task = nullptr;

PollFn s_poll_fn = nullptr;

class Lock {
 public:
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

/** Mutex and the two aircraft lists. False if PSRAM is out. */
bool ensureInit() {
  if (s_mutex == nullptr) {
    s_mutex = xSemaphoreCreateMutex();
  }
  const size_t bytes = sizeof(Aircraft) * kMaxAircraft;
  if (s_aircraft == nullptr) {
    s_aircraft = static_cast<Aircraft*>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM));
  }
  if (s_staging == nullptr) {
    s_staging = static_cast<Aircraft*>(heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM));
  }
  return s_mutex != nullptr && s_aircraft != nullptr && s_staging != nullptr;
}

/** Internal-RAM free (KB) at each step of a fetch, and the lowest seen while reading. */
struct HeapTrace {
  unsigned start = 0;
  unsigned connected = 0;  // TLS session open, headers read
  unsigned read_min = 0;   // lowest while reading the body
  unsigned body = 0;       // body read, connection still open
  unsigned closed = 0;
  unsigned parsed = 0;     // JsonDocument alive
  size_t body_bytes = 0;
  unsigned long parse_ms = 0;
};
HeapTrace s_trace;

/**
 * Keeps the JSON document in PSRAM. Its pool blocks and string copies are each under the
 * 4 KB malloc threshold, so plain malloc puts them all in internal RAM (~25 KB for 7
 * aircraft, growing with traffic). Falls back to any heap if PSRAM is full.
 */
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : malloc(size);
  }
  void deallocate(void* ptr) override { free(ptr); }
  void* reallocate(void* ptr, size_t new_size) override {
    void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : realloc(ptr, new_size);
  }
};
PsramAllocator s_json_allocator;

/** Bigger than the malloc threshold, so the body String starts out in PSRAM. */
constexpr unsigned kBodyInitialReserve = 16 * 1024;

unsigned internalFreeKb() {
  return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}

void logHeapTrace() {
  Serial.printf("adsb heap (internal KB free): start %u, connected %u, reading min %u, "
                "body %u, closed %u, parsed %u (body %u B, parse %lu ms, stack min free %u B)\n",
                s_trace.start, s_trace.connected, s_trace.read_min, s_trace.body,
                s_trace.closed, s_trace.parsed, static_cast<unsigned>(s_trace.body_bytes),
                s_trace.parse_ms, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

/** Wall-clock ms since the epoch, or 0 until SNTP has set the clock. */
int64_t epochMsIfSynced() {
  timeval tv{};
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1600000000) {
    return 0;
  }
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

/**
 * adsb.fi rebuilds its data every ~2 s, so the response's "now" is 1-2.5 s old on arrival, by
 * a different amount each fetch. seen_pos is relative to "now", so without this the aircraft
 * jump back and forth along their track on alternate fetches. Measured with the SNTP clock;
 * before sync, or if the result is implausible, uses config::kAdsbDefaultLagMs.
 */
void setPositionsTime(Outcome* out) {
  constexpr double kMinPlausibleLagMs = -5000.0;  // small negatives are clock error
  constexpr double kMaxPlausibleLagMs = 15000.0;
  out->lag_ms = static_cast<long>(config::kAdsbDefaultLagMs);
  out->lag_measured = false;
  if (out->received_epoch_ms != 0 && out->api_now_ms > 0.0) {
    const double lag = static_cast<double>(out->received_epoch_ms) - out->api_now_ms;
    if (lag >= kMinPlausibleLagMs && lag <= kMaxPlausibleLagMs) {
      out->lag_ms = static_cast<long>(lag);
      out->lag_measured = true;
    }
  }
  out->positions_ms = out->received_ms - static_cast<unsigned long>(out->lag_ms);
}

void setFailure(Outcome* out, FetchStatus status, int code, const char* detail = "") {
  out->status = status;
  out->error_code = code;
  strncpy(out->error_detail, detail, sizeof(out->error_detail) - 1);
  out->error_detail[sizeof(out->error_detail) - 1] = '\0';
}

FetchStatus statusForHttpCode(int code) {
  if (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_NOT_CONNECTED) {
    return FetchStatus::NoConnection;
  }
  if (code == HTTPC_ERROR_READ_TIMEOUT) {
    return FetchStatus::Timeout;
  }
  if (code < 0) {
    return FetchStatus::ConnectionLost;
  }
  if (code == 429) {
    return FetchStatus::RateLimited;
  }
  if (code >= 500) {
    return FetchStatus::ServerError;
  }
  return FetchStatus::HttpError;
}

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGetWithPoll(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  int code = HTTPC_ERROR_READ_TIMEOUT;
  while (millis() < deadline) {
    pollNetwork();
    code = http.GET();
    if (code > 0) {
      return code;
    }
    if (code != HTTPC_ERROR_CONNECTION_REFUSED &&
        code != HTTPC_ERROR_NOT_CONNECTED) {
      return code;
    }
    delay(5);
  }
  // Still unable to connect at the deadline: report that, not a read timeout.
  return code;
}

bool readResponseBodyWithPoll(HTTPClient& http, String& payload) {
  NetworkClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    return false;
  }

  const int content_length = http.getSize();
  payload.reserve(std::max(kBodyInitialReserve,
                           content_length > 0 ? static_cast<unsigned>(content_length + 1) : 0u));

  uint8_t buffer[512];
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (millis() < deadline) {
    pollNetwork();
    s_trace.read_min = std::min(s_trace.read_min, internalFreeKb());
    const int available = stream->available();
    if (available > 0) {
      const int to_read =
          available > static_cast<int>(sizeof(buffer)) ? static_cast<int>(sizeof(buffer))
                                                       : available;
      const int read_bytes = stream->readBytes(buffer, to_read);
      if (read_bytes > 0) {
        payload.concat(reinterpret_cast<const char*>(buffer),
                       static_cast<unsigned>(read_bytes));
      }
    }
    if (content_length > 0 &&
        static_cast<int>(payload.length()) >= content_length) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }

  return payload.length() > 0;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

/** False (and *out = 0) if the message carries no track or heading at all. */
bool pickTrackHeading(const JsonObject& plane, float* out) {
  *out = 0.0f;
  return readJsonFloat(plane, "track", out) ||
         readJsonFloat(plane, "true_heading", out) ||
         readJsonFloat(plane, "mag_heading", out) ||
         readJsonFloat(plane, "dir", out);
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  out[0] = '\0';
  if (out_len == 0 || !obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void parseAltitude(Aircraft* ac, const JsonObject& plane) {
  ac->alt_state = AltState::Unknown;
  ac->alt_ft = 0;
  if (isOnGround(plane)) {
    ac->alt_state = AltState::Ground;
    return;
  }
  float alt = 0.0f;
  if (readJsonFloat(plane, "alt_baro", &alt) || readJsonFloat(plane, "alt_geom", &alt)) {
    ac->alt_state = AltState::Airborne;
    ac->alt_ft = static_cast<int32_t>(lroundf(alt));
  }
}

void formatAltitudeTag(const Aircraft& ac, char* out, size_t out_len) {
  switch (ac.alt_state) {
    case AltState::Ground:
      snprintf(out, out_len, "GND");
      break;
    case AltState::Airborne:
      snprintf(out, out_len, "%ld ft", static_cast<long>(ac.alt_ft));
      break;
    case AltState::Unknown:
      out[0] = '\0';
      break;
  }
}

void parseVerticalRate(Aircraft* ac, const JsonObject& plane) {
  float rate = 0.0f;
  ac->has_vrate =
      readJsonFloat(plane, "baro_rate", &rate) || readJsonFloat(plane, "geom_rate", &rate);
  ac->vrate_fpm = ac->has_vrate
                      ? static_cast<int16_t>(std::max(-32000.0f, std::min(32000.0f, rate)))
                      : 0;
}

uint8_t parseCategory(const JsonObject& plane) {
  if (!plane["category"].is<const char*>()) {
    return kCategoryUnknown;
  }
  const char* s = plane["category"].as<const char*>();
  if (s[0] < 'A' || s[0] > 'D' || s[1] < '0' || s[1] > '7' || s[2] != '\0') {
    return kCategoryUnknown;
  }
  return categoryCode(s[0], static_cast<uint8_t>(s[1] - '0'));
}

Emergency parseEmergency(const JsonObject& plane) {
  if (!plane["emergency"].is<const char*>()) {
    return Emergency::Unknown;
  }
  const char* s = plane["emergency"].as<const char*>();
  struct Entry {
    const char* name;
    Emergency value;
  };
  static constexpr Entry kEntries[] = {
      {"none", Emergency::None},         {"general", Emergency::General},
      {"lifeguard", Emergency::Lifeguard}, {"minfuel", Emergency::MinFuel},
      {"nordo", Emergency::NoRadio},     {"unlawful", Emergency::Unlawful},
      {"downed", Emergency::Downed},
  };
  for (const Entry& e : kEntries) {
    if (strcmp(s, e.name) == 0) {
      return e.value;
    }
  }
  return Emergency::Unknown;
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  copyJsonStringTrimmed(plane, "squawk", ac->squawk, sizeof(ac->squawk));
  parseAltitude(ac, plane);
  formatAltitudeTag(*ac, ac->alt, sizeof(ac->alt));
  parseVerticalRate(ac, plane);
  ac->category = parseCategory(plane);
  ac->emergency = parseEmergency(plane);
  ac->db_flags = plane["dbFlags"].is<int>() ? static_cast<uint8_t>(plane["dbFlags"].as<int>())
                                            : 0;
  ac->seen_pos_s = 0.0f;
  readJsonFloat(plane, "seen_pos", &ac->seen_pos_s);
}

/**
 * One blocking fetch into `out` (the staging list); touches no shared state. Holds the
 * TLS lock only while the connection is open, so the JSON parse doesn't block the clock's
 * zone lookup.
 */
bool fetchAircraft(const Request& req, Aircraft* out, Outcome* outcome) {
  const float dist_nm = kmToNauticalMiles(req.radius_km);

  String url = kApiBase;
  url += String(req.lat, 6);
  url += "/lon/";
  url += String(req.lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  // One TLS session at a time: each takes ~48 KB of internal RAM at its peak.
  tls::Guard tls_guard;

  s_trace = HeapTrace{};
  s_trace.start = internalFreeKb();

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    setFailure(outcome, FetchStatus::ConnectionLost, 0);
    return false;
  }

  http.useHTTP10(true);
  http.setTimeout(kRequestTimeoutMs);
  const int code = performGetWithPoll(http);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d\n", code);
    setFailure(outcome, statusForHttpCode(code), code);
    http.end();
    return false;
  }

  s_trace.connected = internalFreeKb();
  s_trace.read_min = s_trace.connected;

  String payload;
  if (!readResponseBodyWithPoll(http, payload)) {
    Serial.println("adsb: empty response");
    setFailure(outcome, FetchStatus::BadResponse, HTTP_CODE_OK, "empty response");
    http.end();
    return false;
  }
  s_trace.body = internalFreeKb();
  s_trace.body_bytes = payload.length();
  outcome->received_ms = std::max(millis(), 1UL);  // 0 means "never"
  outcome->received_epoch_ms = epochMsIfSynced();
  http.end();
  s_trace.closed = internalFreeKb();
  tls_guard.release();

  JsonDocument doc(&s_json_allocator);
  const unsigned long parse_start = millis();
  const DeserializationError err = deserializeJson(doc, payload);
  s_trace.parse_ms = millis() - parse_start;
  s_trace.parsed = internalFreeKb();
  logHeapTrace();
  if (err) {
    Serial.printf("adsb: JSON parse error: %s\n", err.c_str());
    setFailure(outcome, FetchStatus::BadResponse, HTTP_CODE_OK, err.c_str());
    return false;
  }

  // adsb.fi sends "msg": "No error" on success; anything else is an API-level error.
  const char* api_msg = doc["msg"] | "No error";
  if (strcmp(api_msg, "No error") != 0) {
    Serial.printf("adsb: API error: %s\n", api_msg);
    setFailure(outcome, FetchStatus::BadResponse, HTTP_CODE_OK, api_msg);
    return false;
  }

  outcome->status = FetchStatus::Ok;
  outcome->count = 0;
  // readsb's "now" is seconds in aircraft.json and ms in the API; normalise to ms.
  const double api_now = doc["now"] | 0.0;
  outcome->api_now_ms = api_now > 1e11 ? api_now : api_now * 1000.0;
  setPositionsTime(outcome);

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    out[n].lat = plane["lat"].as<float>();
    out[n].lon = plane["lon"].as<float>();
    out[n].nose_deg = pickNoseHeading(plane);
    out[n].has_track = pickTrackHeading(plane, &out[n].track_deg);
    out[n].gs_knots = pickGroundSpeed(plane);
    fillTagFields(&out[n], plane);
    if (config::kAdsbLogFields) {
      const Aircraft& a = out[n];
      Serial.printf("  %s %-8s %-4s cat %02X alt %d/%ld vr %d sq %s em %u db %02X age %.1f\n",
                    a.hex, a.callsign, a.type, a.category, static_cast<int>(a.alt_state),
                    static_cast<long>(a.alt_ft), a.has_vrate ? a.vrate_fpm : 0, a.squawk,
                    static_cast<unsigned>(a.emergency), a.db_flags, a.seen_pos_s);
    }
    ++n;
  }

  outcome->count = n;
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

/** One aircraft's dead-reckoning miss, for logDeadReckonMisses(). */
struct DrMiss {
  const char* hex;
  float dt_s;
  float gs_knots;
  float track_from;
  float track_to;
  float along_m;  // + = new fix ahead of the prediction
  float cross_m;  // + = right of the old track
  float seen_from;
  float seen_to;
};

float medianOf(float* v, size_t n) {
  std::sort(v, v + n);
  return n % 2 == 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/**
 * Diagnostic (config::kAdsbLogDeadReckon): for each aircraft in both the previous and the new
 * list, projects the previous fix along its track and speed to the new fix's time and logs how
 * far off it was, split along and across the track. Fix times are positions_ms - seen_pos, the
 * same times the display extrapolates from; "lag" is how old the server's "now" was on arrival
 * ("default" before SNTP sync). Runs on the fetch task, which is the only writer of the lists.
 */
void logDeadReckonMisses(const Aircraft* prev, size_t prev_count,
                         unsigned long prev_positions_ms, const Aircraft* cur, size_t cur_count,
                         const Outcome& outcome) {
  constexpr float kKmPerDegLat = 111.0f;
  constexpr float kDegToRad = 0.01745329252f;
  constexpr float kKmPerKnotSecond = 1.852f / 3600.0f;
  constexpr float kMinFixGapS = 0.5f;  // a fix this close to the last one is the same fix
  constexpr size_t kMaxDetailLines = 8;

  char lag[24];
  snprintf(lag, sizeof(lag), "%.2f s%s", outcome.lag_ms / 1000.0,
           outcome.lag_measured ? "" : " (default)");
  if (prev_count == 0) {
    Serial.printf("DR fetch: lag %s, no previous list\n", lag);
    return;
  }

  DrMiss misses[kMaxAircraft];
  size_t n = 0;
  size_t same_fix = 0;
  for (size_t i = 0; i < cur_count; ++i) {
    const Aircraft& new_ac = cur[i];
    const Aircraft* old_ac = nullptr;
    for (size_t j = 0; j < prev_count; ++j) {
      if (strcmp(prev[j].hex, new_ac.hex) == 0) {
        old_ac = &prev[j];
        break;
      }
    }
    if (old_ac == nullptr || !old_ac->has_track || old_ac->gs_knots <= 0.0f) {
      continue;
    }
    // Fix times relative to millis(); the wrap-around in the subtraction is harmless.
    const float dt_s = static_cast<float>(outcome.positions_ms - prev_positions_ms) / 1000.0f -
                       new_ac.seen_pos_s + old_ac->seen_pos_s;
    if (dt_s < kMinFixGapS) {
      ++same_fix;
      continue;
    }
    const float cos_lat = cosf(old_ac->lat * kDegToRad);
    const float moved_x = (new_ac.lon - old_ac->lon) * kKmPerDegLat * cos_lat;
    const float moved_y = (new_ac.lat - old_ac->lat) * kKmPerDegLat;
    const float rad = old_ac->track_deg * kDegToRad;
    const float s = sinf(rad);
    const float c = cosf(rad);
    const float predicted_km = old_ac->gs_knots * kKmPerKnotSecond * dt_s;
    const float ex = moved_x - s * predicted_km;
    const float ey = moved_y - c * predicted_km;
    DrMiss& m = misses[n++];
    m.hex = new_ac.hex;
    m.dt_s = dt_s;
    m.gs_knots = old_ac->gs_knots;
    m.track_from = old_ac->track_deg;
    m.track_to = new_ac.track_deg;
    m.along_m = (ex * s + ey * c) * 1000.0f;
    m.cross_m = (ex * c - ey * s) * 1000.0f;
    m.seen_from = old_ac->seen_pos_s;
    m.seen_to = new_ac.seen_pos_s;
  }

  if (n == 0) {
    Serial.printf("DR fetch: lag %s, 0 matched (%u same fix)\n", lag,
                  static_cast<unsigned>(same_fix));
    return;
  }
  float along[kMaxAircraft];
  float cross_abs[kMaxAircraft];
  float along_min = misses[0].along_m;
  float along_max = misses[0].along_m;
  float cross_max = 0.0f;
  for (size_t k = 0; k < n; ++k) {
    along[k] = misses[k].along_m;
    cross_abs[k] = fabsf(misses[k].cross_m);
    along_min = std::min(along_min, misses[k].along_m);
    along_max = std::max(along_max, misses[k].along_m);
    cross_max = std::max(cross_max, cross_abs[k]);
  }
  Serial.printf("DR fetch: lag %s, %u matched (%u same fix), along med %+.0f m (%+.0f..%+.0f), "
                "|cross| med %.0f m max %.0f\n",
                lag, static_cast<unsigned>(n), static_cast<unsigned>(same_fix),
                medianOf(along, n), along_min, along_max, medianOf(cross_abs, n), cross_max);
  for (size_t k = 0; k < n && k < kMaxDetailLines; ++k) {
    const DrMiss& m = misses[k];
    float turn = m.track_to - m.track_from;
    turn -= 360.0f * floorf((turn + 180.0f) / 360.0f);
    Serial.printf("DR %s dt %.1f gs %.0f trk %.0f%+.0f along %+.0f cross %+.0f m "
                  "seen %.1f>%.1f\n",
                  m.hex, m.dt_s, m.gs_knots, m.track_from, turn, m.along_m, m.cross_m,
                  m.seen_from, m.seen_to);
  }
}

/**
 * Makes a fetch's result the current one, unless invalidate() ran since it started. A
 * failure keeps the last good list (aircraftFresh() expires it) and only updates the status.
 */
void publish(const Outcome& outcome, uint32_t generation) {
  Lock lock;
  if (generation != s_generation) {
    Serial.println("adsb: result dropped (invalidated during the fetch)");
    return;
  }
  const size_t kept_count = s_published.count;
  s_published = outcome;
  if (outcome.status == FetchStatus::Ok) {
    std::swap(s_aircraft, s_staging);  // the old list becomes the next staging area
    s_last_ok_ms = outcome.received_ms;
    s_positions_ms = outcome.positions_ms;
  } else {
    s_published.count = kept_count;
  }
  ++s_publish_count;
}

/** Fetches every kAdsbFetchIntervalMs (start to start) while enabled; a notify fetches now. */
void fetchTask(void*) {
  unsigned long last_start = 0;
  bool fetched = false;
  TickType_t wait = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, wait);

    Request req;
    uint32_t generation = 0;
    bool enabled = false;
    {
      Lock lock;
      req = s_request;
      generation = s_generation;
      enabled = s_enabled;
    }
    if (!enabled || !req.valid) {
      wait = portMAX_DELAY;  // setEnabled() / setRequest() notify
      continue;
    }
    const unsigned long since = millis() - last_start;
    if (fetched && since < kMinFetchGapMs) {
      wait = pdMS_TO_TICKS(kMinFetchGapMs - since);
      continue;
    }

    last_start = millis();
    fetched = true;
    Outcome outcome;
    fetchAircraft(req, s_staging, &outcome);
    if (config::kAdsbLogDeadReckon && outcome.status == FetchStatus::Ok) {
      size_t prev_count = 0;
      unsigned long prev_positions_ms = 0;
      {
        Lock lock;
        prev_count = s_last_ok_ms != 0 ? s_published.count : 0;
        prev_positions_ms = s_positions_ms;
      }
      // s_aircraft only changes in publish(), on this task, so it's safe to read here.
      logDeadReckonMisses(s_aircraft, prev_count, prev_positions_ms, s_staging, outcome.count,
                          outcome);
    }
    publish(outcome, generation);

    const unsigned long elapsed = millis() - last_start;
    wait = elapsed >= config::kAdsbFetchIntervalMs
               ? 0
               : pdMS_TO_TICKS(config::kAdsbFetchIntervalMs - elapsed);
  }
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

bool startFetchTask() {
  if (!ensureInit()) {
    Serial.println("adsb: out of memory for the aircraft lists");
    return false;
  }
  if (s_task != nullptr) {
    return true;
  }
  if (xTaskCreatePinnedToCoreWithCaps(fetchTask, "adsb_fetch", kTaskStackBytes, nullptr,
                                      kTaskPriority, &s_task, kTaskCore,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    s_task = nullptr;
    Serial.println("adsb: could not start the fetch task");
    return false;
  }
  return true;
}

void setRequest(double center_lat, double center_lon, float fetch_radius_km) {
  if (s_mutex == nullptr) {
    return;
  }
  bool changed = false;
  {
    Lock lock;
    changed = !s_request.valid || s_request.lat != center_lat ||
              s_request.lon != center_lon || s_request.radius_km != fetch_radius_km;
    s_request = Request{center_lat, center_lon, fetch_radius_km, true};
  }
  if (changed && s_task != nullptr) {
    xTaskNotifyGive(s_task);
  }
}

void setEnabled(bool enabled) {
  if (s_mutex == nullptr) {
    return;
  }
  bool turned_on = false;
  {
    Lock lock;
    turned_on = enabled && !s_enabled;
    s_enabled = enabled;
  }
  if (turned_on && s_task != nullptr) {
    xTaskNotifyGive(s_task);
  }
}

uint32_t publishCount() {
  if (s_mutex == nullptr) {
    return 0;
  }
  Lock lock;
  return s_publish_count;
}

size_t aircraftSnapshot(Aircraft* out, size_t max_count, Snapshot* meta) {
  *meta = Snapshot{};
  if (s_mutex == nullptr) {
    return 0;
  }
  Lock lock;
  const size_t n = std::min(s_published.count, max_count);
  memcpy(out, s_aircraft, n * sizeof(Aircraft));
  meta->count = n;
  meta->status = s_published.status;
  meta->error_code = s_published.error_code;
  memcpy(meta->error_detail, s_published.error_detail, sizeof(meta->error_detail));
  meta->fresh = s_last_ok_ms != 0 && millis() - s_last_ok_ms <= config::kAdsbStaleAfterMs;
  meta->fetched_ms = s_last_ok_ms;
  meta->positions_ms = s_positions_ms;
  return n;
}

size_t aircraftCount() {
  if (s_mutex == nullptr) {
    return 0;
  }
  Lock lock;
  return s_published.count;
}

void invalidate() {
  if (s_mutex == nullptr) {
    return;
  }
  Lock lock;
  s_published = Outcome{};
  s_last_ok_ms = 0;
  ++s_generation;
}

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  if (!ensureInit()) {
    return false;
  }
  uint32_t generation = 0;
  {
    Lock lock;
    generation = s_generation;
  }
  Outcome outcome;
  const bool ok = fetchAircraft(Request{center_lat, center_lon, fetch_radius_km, true},
                                s_staging, &outcome);
  publish(outcome, generation);
  return ok;
}

}  // namespace services::adsb
