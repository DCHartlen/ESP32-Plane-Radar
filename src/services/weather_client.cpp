#include "services/weather_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "services/tls_lock.h"

namespace services::weather {

namespace {

constexpr char kGeoMetUrl[] = "https://geo.weather.gc.ca/geomet";
constexpr char kRadarLayer[] = "RADAR_1KM_RRAI";
constexpr unsigned long kRequestTimeoutMs = 15000;
// The whole body is held in PSRAM; a radar PNG or the capabilities are far below the cap.
constexpr size_t kBodyMaxBytes = 1024 * 1024;
constexpr size_t kBodyGrowBytes = 32 * 1024;
// Between back-to-back image fetches while filling the history, to go easy on the server.
constexpr TickType_t kBackfillGapTicks = pdMS_TO_TICKS(200);
// Used if the capabilities' time dimension can't be read: GeoMet's 6 min step and 3 h span.
constexpr int kDefaultStepS = 360;
constexpr int kDefaultSpanS = 3 * 3600;
constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

// Fetch task, as in adsb_client: core 0, stack in PSRAM (so it must never write flash).
constexpr uint32_t kTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;

constexpr size_t kMaxFrames = config::kWeatherHistoryFrames;

/** A response body in PSRAM, reused across requests. Always NUL-terminated. */
struct Body {
  uint8_t* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
};

/** One time step: its valid time and its PNG, in its own PSRAM allocation. */
struct Frame {
  time_t time = 0;
  uint32_t id = 0;
  uint8_t* png = nullptr;
  size_t len = 0;
};

struct Request {
  double lat = 0.0;
  double lon = 0.0;
  float half_km = 0.0f;
  bool valid = false;
};

// Shared between the loop and the fetch task, under s_mutex.
SemaphoreHandle_t s_mutex = nullptr;
Request s_request;
uint32_t s_request_generation = 0;
bool s_enabled = false;
Frame s_frames[kMaxFrames];  // oldest first
size_t s_frame_count = 0;
State s_state = State::Pending;
char s_error[sizeof(Status::error)] = "";
uint32_t s_publish_count = 0;
uint32_t s_last_frame_id = 0;

// Fetch task only.
TaskHandle_t s_task = nullptr;
Body s_body;
uint32_t s_task_generation = 0;
bool s_have_caps = false;
unsigned long s_caps_ms = 0;  // millis() of the last good GetCapabilities
time_t s_newest = 0;          // the layer's newest time step
time_t s_oldest = 0;          // its oldest
int s_step_s = kDefaultStepS;
// The layer's coverage, from its EX_GeographicBoundingBox.
float s_west = -180.0f;
float s_east = 180.0f;
float s_south = -90.0f;
float s_north = 90.0f;

class Lock {
 public:
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

/** Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm). */
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2 ? 1 : 0;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

/** "2026-10-16T00:12:00Z" to UTC epoch seconds; 0 if it doesn't parse. */
time_t parseIsoUtc(const char* text) {
  int y = 0;
  int mo = 0;
  int d = 0;
  int h = 0;
  int mi = 0;
  int s = 0;
  if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
    return 0;
  }
  return static_cast<time_t>(daysFromCivil(y, mo, d) * 86400L + h * 3600L + mi * 60L + s);
}

void formatIsoUtc(time_t t, char* out, size_t len) {
  struct tm tm_utc;
  gmtime_r(&t, &tm_utc);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/** "PT6M", "PT1H" or "PT30S" in seconds; 0 if it doesn't parse. */
int parseDurationS(const char* text) {
  int n = 0;
  char unit = '\0';
  if (sscanf(text, "PT%d%c", &n, &unit) != 2 || n <= 0) {
    return 0;
  }
  switch (unit) {
    case 'H': return n * 3600;
    case 'M': return n * 60;
    case 'S': return n;
    default: return 0;
  }
}

void publishLocked() { ++s_publish_count; }

void setState(State state, const char* error) {
  Lock lock;
  if (s_state == state && strcmp(s_error, error) == 0) {
    return;
  }
  s_state = state;
  snprintf(s_error, sizeof(s_error), "%s", error);
  publishLocked();
}

void freeFrame(Frame& f) {
  heap_caps_free(f.png);
  f = Frame{};
}

void clearFramesLocked() {
  for (size_t i = 0; i < s_frame_count; ++i) {
    freeFrame(s_frames[i]);
  }
  s_frame_count = 0;
}

bool bodyReserve(size_t need) {
  if (need <= s_body.cap) {
    return true;
  }
  if (need > kBodyMaxBytes) {
    return false;
  }
  const size_t cap = std::min(kBodyMaxBytes, need + kBodyGrowBytes);
  void* p = heap_caps_realloc(s_body.data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p == nullptr) {
    return false;
  }
  s_body.data = static_cast<uint8_t*>(p);
  s_body.cap = cap;
  return true;
}

/** Blocking HTTPS GET into s_body. On failure fills `error`. */
bool httpsGet(const String& url, char* error, size_t error_len) {
  tls::Guard tls_guard;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    snprintf(error, error_len, "can't start request");
    return false;
  }
  // HTTP/1.0: no chunked encoding, so the stream is the raw body.
  http.useHTTP10(true);
  http.setTimeout(kRequestTimeoutMs);
  http.setConnectTimeout(kRequestTimeoutMs);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) {
      snprintf(error, error_len, "Can't reach GeoMet (%d)", code);
    } else {
      snprintf(error, error_len, "GeoMet HTTP %d", code);
    }
    http.end();
    return false;
  }

  NetworkClient* stream = http.getStreamPtr();
  const int content_length = http.getSize();
  s_body.len = 0;
  bool ok = stream != nullptr &&
            bodyReserve(content_length > 0 ? static_cast<size_t>(content_length) + 1
                                           : kBodyGrowBytes);
  const unsigned long deadline = millis() + kRequestTimeoutMs;
  while (ok && millis() < deadline) {
    const int available = stream->available();
    if (available > 0) {
      if (!bodyReserve(s_body.len + available + 1)) {
        ok = false;
        break;
      }
      const int n = stream->readBytes(s_body.data + s_body.len, available);
      if (n > 0) {
        s_body.len += n;
      }
    }
    if (content_length > 0 && s_body.len >= static_cast<size_t>(content_length)) {
      break;
    }
    if (!http.connected() && stream->available() <= 0) {
      break;
    }
    delay(1);
  }
  http.end();

  if (!ok) {
    snprintf(error, error_len, "response too big");
    return false;
  }
  if (s_body.len == 0) {
    snprintf(error, error_len, "empty response");
    return false;
  }
  s_body.data[s_body.len] = '\0';
  return true;
}

/** Text of the first <tag>...</tag> at or after `from`, as a float; false if missing. */
bool readXmlFloat(const char* from, const char* tag, float* out) {
  char open[48];
  snprintf(open, sizeof(open), "<%s>", tag);
  const char* p = strstr(from, open);
  if (p == nullptr) {
    return false;
  }
  *out = strtof(p + strlen(open), nullptr);
  return true;
}

/**
 * The layer's time steps (default = newest, and the start/end/period extent) and its
 * coverage box, from GetCapabilities for the layer alone.
 */
bool fetchCapabilities(char* error, size_t error_len) {
  String url = kGeoMetUrl;
  url += "?service=WMS&version=1.3.0&request=GetCapabilities&layer=";
  url += kRadarLayer;
  const unsigned long t0 = millis();
  if (!httpsGet(url, error, error_len)) {
    return false;
  }
  const unsigned long ms = millis() - t0;

  const char* xml = reinterpret_cast<const char*>(s_body.data);
  // The root layer has a whole-world box; the radar layer's own comes after its name.
  const char* layer = strstr(xml, "<Name>RADAR_1KM_RRAI</Name>");
  const char* dim = layer != nullptr ? strstr(layer, "<Dimension name=\"time\"") : nullptr;
  const char* def = dim != nullptr ? strstr(dim, "default=\"") : nullptr;
  if (def == nullptr) {
    snprintf(error, error_len, "no time steps from GeoMet");
    Serial.printf("weather: capabilities start: %.200s\n", xml);
    return false;
  }
  const time_t newest = parseIsoUtc(def + 9);
  if (newest == 0) {
    snprintf(error, error_len, "bad time from GeoMet");
    return false;
  }

  // Extent "start/end/PT6M" between the tag's '>' and '<'.
  time_t oldest = newest - kDefaultSpanS;
  int step = kDefaultStepS;
  const char* text = strchr(def, '>');
  if (text != nullptr) {
    ++text;
    const char* slash1 = strchr(text, '/');
    const char* slash2 = slash1 != nullptr ? strchr(slash1 + 1, '/') : nullptr;
    const char* close = strchr(text, '<');
    if (slash2 != nullptr && close != nullptr && slash2 < close) {
      const time_t start = parseIsoUtc(text);
      const int period = parseDurationS(slash2 + 1);
      if (start != 0 && start <= newest) {
        oldest = start;
      }
      if (period > 0) {
        step = period;
      }
    }
  }

  float west = s_west;
  float east = s_east;
  float south = s_south;
  float north = s_north;
  const char* box = strstr(layer, "<EX_GeographicBoundingBox>");
  if (box != nullptr && box < dim) {
    readXmlFloat(box, "westBoundLongitude", &west);
    readXmlFloat(box, "eastBoundLongitude", &east);
    readXmlFloat(box, "southBoundLatitude", &south);
    readXmlFloat(box, "northBoundLatitude", &north);
  }

  s_newest = newest;
  s_oldest = oldest;
  s_step_s = step;
  s_west = west;
  s_east = east;
  s_south = south;
  s_north = north;
  s_have_caps = true;
  s_caps_ms = millis();

  char newest_text[24];
  formatIsoUtc(newest, newest_text, sizeof(newest_text));
  Serial.printf("weather: newest %s, step %d s, coverage lat %.1f..%.1f lon %.1f..%.1f "
                "(capabilities %u B in %lu ms)\n",
                newest_text, step, south, north, west, east,
                static_cast<unsigned>(s_body.len), ms);
  return true;
}

bool inCoverage(double lat, double lon) {
  return lat >= s_south && lat <= s_north && lon >= s_west && lon <= s_east;
}

/** GetMap for one time step into s_body: a square bbox of the request, kImagePx wide. */
bool fetchImage(const Request& req, time_t t, char* error, size_t error_len) {
  const double dlat = req.half_km / kKmPerDeg;
  const double dlon = req.half_km / (kKmPerDeg * cosf(static_cast<float>(req.lat) * kDegToRad));
  // WMS 1.3.0 with EPSG:4326 orders the bbox lat,lon (min lat, min lon, max lat, max lon).
  char bbox[96];
  snprintf(bbox, sizeof(bbox), "%.5f,%.5f,%.5f,%.5f", req.lat - dlat, req.lon - dlon,
           req.lat + dlat, req.lon + dlon);
  char time_text[24];
  formatIsoUtc(t, time_text, sizeof(time_text));

  String url = kGeoMetUrl;
  url += "?service=WMS&version=1.3.0&request=GetMap&format=image/png&transparent=true";
  url += "&styles=&crs=EPSG:4326&layers=";
  url += kRadarLayer;
  url += "&bbox=";
  url += bbox;
  url += "&width=";
  url += kImagePx;
  url += "&height=";
  url += kImagePx;
  url += "&time=";
  url += time_text;

  const unsigned long t0 = millis();
  if (!httpsGet(url, error, error_len)) {
    return false;
  }
  // WMS errors come back as 200 with an XML ServiceException body.
  static constexpr uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G'};
  if (s_body.len < sizeof(kPngSignature) ||
      memcmp(s_body.data, kPngSignature, sizeof(kPngSignature)) != 0) {
    snprintf(error, error_len, "GeoMet sent no image");
    Serial.printf("weather: response start: %.300s\n", reinterpret_cast<char*>(s_body.data));
    return false;
  }
  Serial.printf("weather: image %s, %u B in %lu ms\n", time_text,
                static_cast<unsigned>(s_body.len), millis() - t0);
  return true;
}

/** Drops frames older than `window_start`; true if any went. */
bool dropOlderThan(time_t window_start) {
  Lock lock;
  size_t keep_from = 0;
  while (keep_from < s_frame_count && s_frames[keep_from].time < window_start) {
    freeFrame(s_frames[keep_from]);
    ++keep_from;
  }
  if (keep_from == 0) {
    return false;
  }
  for (size_t i = keep_from; i < s_frame_count; ++i) {
    s_frames[i - keep_from] = s_frames[i];
    s_frames[i] = Frame{};
  }
  s_frame_count -= keep_from;
  publishLocked();
  return true;
}

bool haveFrame(time_t t) {
  Lock lock;
  for (size_t i = 0; i < s_frame_count; ++i) {
    if (s_frames[i].time == t) {
      return true;
    }
  }
  return false;
}

/**
 * Keeps s_body as frame `t`, in time order, unless the request changed while it was being
 * fetched. Drops the oldest frame if the history is full.
 */
bool storeFrame(time_t t, uint32_t generation) {
  uint8_t* png = static_cast<uint8_t*>(heap_caps_malloc(s_body.len, MALLOC_CAP_SPIRAM));
  if (png == nullptr) {
    return false;
  }
  memcpy(png, s_body.data, s_body.len);

  Lock lock;
  if (generation != s_request_generation) {
    heap_caps_free(png);
    return true;  // not an error: the next pass starts over for the new request
  }
  if (s_frame_count == kMaxFrames) {
    freeFrame(s_frames[0]);
    for (size_t i = 1; i < kMaxFrames; ++i) {
      s_frames[i - 1] = s_frames[i];
    }
    s_frames[kMaxFrames - 1] = Frame{};
    --s_frame_count;
  }
  size_t at = s_frame_count;
  while (at > 0 && s_frames[at - 1].time > t) {
    s_frames[at] = s_frames[at - 1];
    --at;
  }
  s_frames[at] = Frame{t, ++s_last_frame_id, png, s_body.len};
  ++s_frame_count;
  s_state = State::Ok;
  s_error[0] = '\0';
  publishLocked();
  return true;
}

/**
 * One step of keeping the history current; returns how long to wait before the next.
 * Each pass fetches at most one image, newest missing first, so the live image comes first
 * and the replay fills in behind it.
 */
TickType_t updateOnce() {
  Request req;
  uint32_t generation = 0;
  {
    Lock lock;
    if (!s_enabled || !s_request.valid) {
      return portMAX_DELAY;
    }
    req = s_request;
    generation = s_request_generation;
  }
  if (generation != s_task_generation) {
    s_task_generation = generation;
    Lock lock;
    clearFramesLocked();
    s_state = State::Pending;
    s_error[0] = '\0';
    publishLocked();
  }

  char error[sizeof(Status::error)] = "";
  if (!s_have_caps || millis() - s_caps_ms >= config::kWeatherCheckIntervalMs) {
    if (!fetchCapabilities(error, sizeof(error))) {
      Serial.printf("weather: capabilities failed: %s\n", error);
      setState(State::Error, error);
      return pdMS_TO_TICKS(config::kWeatherRetryMs);
    }
  }
  const TickType_t until_check = pdMS_TO_TICKS(
      config::kWeatherCheckIntervalMs - std::min(config::kWeatherCheckIntervalMs,
                                                 millis() - s_caps_ms));

  if (!inCoverage(req.lat, req.lon)) {
    setState(State::NoCoverage, "");
    return until_check;
  }

  const time_t window_start =
      std::max(s_oldest, s_newest - static_cast<time_t>(kMaxFrames - 1) * s_step_s);
  dropOlderThan(window_start);

  for (time_t t = s_newest; t >= window_start; t -= s_step_s) {
    if (haveFrame(t)) {
      continue;
    }
    if (!fetchImage(req, t, error, sizeof(error))) {
      Serial.printf("weather: image failed: %s\n", error);
      setState(State::Error, error);
      return pdMS_TO_TICKS(config::kWeatherRetryMs);
    }
    if (!storeFrame(t, generation)) {
      setState(State::Error, "out of memory");
      return pdMS_TO_TICKS(config::kWeatherRetryMs);
    }
    return kBackfillGapTicks;
  }
  setState(State::Ok, "");
  return until_check;
}

void fetchTask(void*) {
  TickType_t wait = portMAX_DELAY;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, wait);
    wait = updateOnce();
  }
}

}  // namespace

bool startFetchTask() {
  if (s_task != nullptr) {
    return true;
  }
  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) {
    return false;
  }
  return xTaskCreatePinnedToCoreWithCaps(fetchTask, "weather_fetch", kTaskStackBytes, nullptr,
                                         kTaskPriority, &s_task, kTaskCore,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

void setRequest(double center_lat, double center_lon, float half_width_km) {
  if (s_mutex == nullptr) {
    return;
  }
  bool changed = false;
  {
    Lock lock;
    changed = !s_request.valid || s_request.lat != center_lat || s_request.lon != center_lon ||
              s_request.half_km != half_width_km;
    if (changed) {
      s_request = Request{center_lat, center_lon, half_width_km, true};
      ++s_request_generation;
    }
  }
  if (changed) {
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
  if (turned_on) {
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

Status status() {
  Status out;
  if (s_mutex == nullptr) {
    return out;
  }
  Lock lock;
  out.state = s_state;
  snprintf(out.error, sizeof(out.error), "%s", s_error);
  out.frame_count = s_frame_count;
  out.newest_time = s_frame_count > 0 ? s_frames[s_frame_count - 1].time : 0;
  return out;
}

time_t frameTime(size_t i) {
  if (s_mutex == nullptr) {
    return 0;
  }
  Lock lock;
  return i < s_frame_count ? s_frames[i].time : 0;
}

uint32_t frameId(size_t i) {
  if (s_mutex == nullptr) {
    return 0;
  }
  Lock lock;
  return i < s_frame_count ? s_frames[i].id : 0;
}

size_t copyFramePng(size_t i, uint8_t** out, size_t* capacity) {
  if (s_mutex == nullptr) {
    return 0;
  }
  Lock lock;
  if (i >= s_frame_count) {
    return 0;
  }
  const Frame& f = s_frames[i];
  if (f.len > *capacity) {
    void* p = heap_caps_realloc(*out, f.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
      return 0;
    }
    *out = static_cast<uint8_t*>(p);
    *capacity = f.len;
  }
  memcpy(*out, f.png, f.len);
  return f.len;
}

}  // namespace services::weather
