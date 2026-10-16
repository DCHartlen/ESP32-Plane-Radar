#ifdef WEATHER_SPIKE

#include "spike/weather_spike.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/buttons.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/tls_lock.h"
#include "services/wifi_setup.h"
#include "ui/canvas_pixels.h"
#include "ui/radar_theme.h"
#include "ui/ui_scale.h"

namespace {

using ui::px;
using ui::radar::kCenterX;
using ui::radar::kCenterY;
using ui::radar::kGridOuterRadius;

// Milton, Ontario.
constexpr double kCenterLat = 43.5183;
constexpr double kCenterLon = -79.8774;

constexpr char kGeoMetUrl[] = "https://geo.weather.gc.ca/geomet";
// North American composite rain rate (mm/h), 1 km cells, a new image every 6 min.
constexpr char kRadarLayer[] = "RADAR_1KM_RRAI";

// Fetched at half the screen size, decoded once into a cache, then copied at 2x each frame.
// The decoder's cost is per pixel (an empty image took as long as one with rain), so a
// 720 px image would decode about 4x slower.
constexpr int kImageSizePx = config::kDisplayWidth / 2;
static_assert(config::kDisplayWidth == 2 * kImageSizePx &&
                  config::kDisplayHeight == config::kDisplayWidth,
              "the 2x copy assumes a square screen twice the image size");
// Cache pixels left at this value are transparent. GeoMet's rain palette has no pure black.
constexpr uint16_t kTransparentKey = 0x0000;

// Same projection as ui/radar_geometry.
constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

/** Distance on ring 3 (of 4), as in ui/radar_range; the last two are the proposed extra steps. */
constexpr float kRing3Km[] = {5.0f, 10.0f, 15.0f, 25.0f, 50.0f, 100.0f};
constexpr size_t kRangeCount = sizeof(kRing3Km) / sizeof(kRing3Km[0]);
constexpr size_t kStartRange = 4;  // 50 km

constexpr unsigned long kRefreshMs = 5UL * 60UL * 1000UL;
// Stands in for the clock's once-a-minute redraw, so the log shows frames without a fetch.
constexpr unsigned long kRedrawMs = 10000;
constexpr unsigned long kRequestTimeoutMs = 15000;
// The whole body is held in PSRAM; a radar PNG this size is far below the cap.
constexpr size_t kBodyMaxBytes = 1024 * 1024;
constexpr size_t kBodyGrowBytes = 32 * 1024;

// Fetch task, as in services/adsb_client: core 0, stack in PSRAM (so it must never write flash).
constexpr uint32_t kTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;

/** A response body in PSRAM, reused across fetches. Always NUL-terminated. */
struct Body {
  uint8_t* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
};

/** One fetch-and-decode on the task, shown on screen and in the log. */
struct Report {
  size_t range = 0;
  char time[32] = "";  // image valid time, ISO 8601 UTC
  char error[96] = "";
  bool ok = false;
  bool time_reused = false;  // a range change: no GetCapabilities this time
  size_t caps_bytes = 0;
  size_t png_bytes = 0;
  unsigned long caps_ms = 0;
  unsigned long map_ms = 0;
  unsigned long decode_ms = 0;
  // Internal RAM, KB.
  unsigned idle_free = 0;
  unsigned tls_min = 0;
  unsigned decode_min = 0;
  unsigned largest = 0;
};

// Shared between the loop and the fetch task, under s_mutex.
SemaphoreHandle_t s_mutex = nullptr;
size_t s_request_range = kStartRange;
bool s_request_new_time = true;
bool s_enabled = false;
LGFX_Sprite s_images[2];
int s_ready = -1;  // index of the image the loop draws; -1 until the first decode
size_t s_ready_range = kStartRange;
char s_ready_time[32] = "";
Report s_published;
uint32_t s_publish_count = 0;

// Fetch task only.
TaskHandle_t s_task = nullptr;
Body s_body;
char s_time[32] = "";  // last time step from GetCapabilities

// Loop only.
size_t s_range = kStartRange;

class Lock {
 public:
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
  Lock(const Lock&) = delete;
  Lock& operator=(const Lock&) = delete;
};

unsigned internalFreeKb() {
  return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}

/**
 * Lowest internal free RAM while `fn` runs. The monitor resets the "minimum free" figure,
 * so this measures this step alone, not the lowest point since boot. Only the fetch task
 * calls it, so two monitors never overlap.
 */
template <typename Fn>
unsigned internalMinKbDuring(Fn fn) {
  heap_caps_monitor_local_minimum_free_size_start();
  fn();
  const unsigned min_kb =
      static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
  heap_caps_monitor_local_minimum_free_size_stop();
  return min_kb;
}

float outerKm(size_t range) { return kRing3Km[range] * 4.0f / 3.0f; }

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
  services::tls::Guard tls_guard;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    snprintf(error, error_len, "http.begin failed");
    return false;
  }
  // HTTP/1.0: no chunked encoding, so the stream is the raw body.
  http.useHTTP10(true);
  http.setTimeout(kRequestTimeoutMs);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(error, error_len, "HTTP %d", code);
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
    snprintf(error, error_len, "body over %u KB or out of PSRAM",
             static_cast<unsigned>(kBodyMaxBytes / 1024));
    return false;
  }
  if (s_body.len == 0) {
    snprintf(error, error_len, "empty response");
    return false;
  }
  s_body.data[s_body.len] = '\0';
  return true;
}

/**
 * The layer's latest time step: the `default` of its time dimension in GetCapabilities.
 * Asking GetMap for that exact time keeps the image and its timestamp in step.
 */
bool fetchLatestTime(Report& r) {
  String url = kGeoMetUrl;
  url += "?service=WMS&version=1.3.0&request=GetCapabilities&layer=";
  url += kRadarLayer;
  const unsigned long t0 = millis();
  bool ok = false;
  r.tls_min = internalMinKbDuring([&] { ok = httpsGet(url, r.error, sizeof(r.error)); });
  r.caps_ms = millis() - t0;
  r.caps_bytes = s_body.len;
  if (!ok) {
    return false;
  }

  const char* xml = reinterpret_cast<const char*>(s_body.data);
  const char* dim = strstr(xml, "name=\"time\"");
  const char* def = dim != nullptr ? strstr(dim, "default=\"") : nullptr;
  const char* end = def != nullptr ? strchr(def + 9, '"') : nullptr;
  if (end == nullptr || end - (def + 9) >= static_cast<ptrdiff_t>(sizeof(s_time))) {
    snprintf(r.error, sizeof(r.error), "no time dimension in capabilities");
    Serial.printf("weather: capabilities start: %.200s\n", xml);
    return false;
  }
  memcpy(s_time, def + 9, end - (def + 9));
  s_time[end - (def + 9)] = '\0';
  return true;
}

/** Square bbox that maps to the full screen, so ring N of the radar lands on ring N here. */
bool fetchRadarPng(Report& r) {
  const float half_km = outerKm(r.range) * static_cast<float>(kCenterX) / kGridOuterRadius;
  const double dlat = half_km / kKmPerDeg;
  const double dlon = half_km / (kKmPerDeg * cosf(static_cast<float>(kCenterLat) * kDegToRad));

  // WMS 1.3.0 with EPSG:4326 orders the bbox lat,lon (min lat, min lon, max lat, max lon).
  char bbox[96];
  snprintf(bbox, sizeof(bbox), "%.5f,%.5f,%.5f,%.5f", kCenterLat - dlat, kCenterLon - dlon,
           kCenterLat + dlat, kCenterLon + dlon);
  String url = kGeoMetUrl;
  url += "?service=WMS&version=1.3.0&request=GetMap&format=image/png&transparent=true";
  url += "&styles=&crs=EPSG:4326&layers=";
  url += kRadarLayer;
  url += "&bbox=";
  url += bbox;
  url += "&width=";
  url += kImageSizePx;
  url += "&height=";
  url += kImageSizePx;
  url += "&time=";
  url += r.time;

  const unsigned long t0 = millis();
  bool ok = false;
  const unsigned tls_min =
      internalMinKbDuring([&] { ok = httpsGet(url, r.error, sizeof(r.error)); });
  r.tls_min = r.time_reused ? tls_min : std::min(r.tls_min, tls_min);
  r.map_ms = millis() - t0;
  r.png_bytes = s_body.len;
  if (!ok) {
    return false;
  }
  // WMS errors come back as 200 with an XML ServiceException body.
  static constexpr uint8_t kPngSignature[] = {0x89, 'P', 'N', 'G'};
  if (s_body.len < sizeof(kPngSignature) ||
      memcmp(s_body.data, kPngSignature, sizeof(kPngSignature)) != 0) {
    snprintf(r.error, sizeof(r.error), "not a PNG (WMS error?)");
    Serial.printf("weather: response start: %.300s\n", reinterpret_cast<char*>(s_body.data));
    return false;
  }
  return true;
}

/** Decodes s_body unscaled into `image`; untouched pixels stay kTransparentKey. */
bool decodeInto(LGFX_Sprite& image, Report& r) {
  image.fillSprite(kTransparentKey);
  const unsigned long t0 = millis();
  bool ok = false;
  r.decode_min =
      internalMinKbDuring([&] { ok = image.drawPng(s_body.data, s_body.len, 0, 0); });
  r.decode_ms = millis() - t0;
  if (!ok) {
    snprintf(r.error, sizeof(r.error), "PNG decode failed");
  }
  return ok;
}

/**
 * Fetches and decodes whenever notified. The loop notifies on a range change (reusing the
 * last time step) and on each periodic refresh (asking for the latest one). Taps that
 * arrive during a fetch collapse into one more pass with the newest range.
 */
void fetchTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    Report r;
    bool new_time = false;
    int staging = 0;
    {
      Lock lock;
      if (!s_enabled) {
        continue;
      }
      r.range = s_request_range;
      new_time = s_request_new_time || s_time[0] == '\0';
      s_request_new_time = false;
      // Only this task changes s_ready, so the other image is free until the next publish.
      staging = s_ready == 0 ? 1 : 0;
    }

    r.idle_free = internalFreeKb();
    r.largest =
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
    r.time_reused = !new_time;
    bool ok = !new_time || fetchLatestTime(r);
    if (ok) {
      memcpy(r.time, s_time, sizeof(r.time));
      ok = fetchRadarPng(r) && decodeInto(s_images[staging], r);
    } else {
      // A failed lookup leaves the request asking for a new time step.
      Lock lock;
      s_request_new_time = true;
    }
    r.ok = ok;

    Lock lock;
    s_published = r;
    if (ok) {
      s_ready = staging;
      s_ready_range = r.range;
      memcpy(s_ready_time, r.time, sizeof(s_ready_time));
    }
    ++s_publish_count;
  }
}

bool startFetchTask() {
  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) {
    return false;
  }
  for (LGFX_Sprite& image : s_images) {
    image.setPsram(true);
    // Same pixel format as canvas, so the copy is a plain 16-bit store.
    image.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    if (image.createSprite(kImageSizePx, kImageSizePx) == nullptr) {
      return false;
    }
  }
  return xTaskCreatePinnedToCoreWithCaps(fetchTask, "weather_fetch", kTaskStackBytes, nullptr,
                                         kTaskPriority, &s_task, kTaskCore,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

/** Asks the task for the current range; `new_time` also re-reads the latest time step. */
void requestFetch(bool new_time) {
  {
    Lock lock;
    s_request_range = s_range;
    s_request_new_time = s_request_new_time || new_time;
  }
  xTaskNotifyGive(s_task);
}

void setEnabled(bool enabled) {
  Lock lock;
  s_enabled = enabled;
}

/** One row of the background, in internal RAM so clearing never reads PSRAM back. */
alignas(4) uint16_t s_clear_row[config::kDisplayWidth];

/**
 * Fills the round glass with `color`, as radar_display's clearCanvas does but without its
 * landmark layer. fillScreen/fillCircle with a colour whose two bytes differ copy PSRAM to
 * PSRAM, reading back as much as they write; this copies the internal row instead and skips
 * the corners (~21% of the buffer), which are never seen.
 */
void clearRound(uint16_t color) {
  auto* fb = static_cast<uint16_t*>(canvas.getBuffer());
  if (s_clear_row[0] != color || s_clear_row[config::kDisplayWidth - 1] != color) {
    std::fill(std::begin(s_clear_row), std::end(s_clear_row), color);
  }
  constexpr int kW = config::kDisplayWidth;
  constexpr float kRadius = kW * 0.5f;
  // Spans start and end on 16 px (one 32-byte cache line), which also leaves a few px of
  // margin outside the glass.
  constexpr int kAlignPx = 16;
  static_assert(kW % kAlignPx == 0, "rows must split into whole cache lines");
  for (int y = 0; y < config::kDisplayHeight; ++y) {
    const float dy = static_cast<float>(y) + 0.5f - config::kDisplayHeight * 0.5f;
    const float half_sq = kRadius * kRadius - dy * dy;
    const int half = half_sq > 0.0f ? static_cast<int>(ceilf(sqrtf(half_sq))) : 0;
    const int left = std::max(0, (kW / 2 - half) / kAlignPx * kAlignPx);
    const int right = kW - left;  // symmetric about the centre
    memcpy(fb + static_cast<size_t>(y) * kW + left, s_clear_row,
           static_cast<size_t>(right - left) * sizeof(uint16_t));
  }
}

/** Copies `image` into canvas at 2x, skipping transparent pixels. */
void copyImage2x(LGFX_Sprite& image) {
  const auto* src = static_cast<const uint16_t*>(image.getBuffer());
  auto* fb = static_cast<uint16_t*>(canvas.getBuffer());
  for (int sy = 0; sy < kImageSizePx; ++sy) {
    const uint16_t* row = src + static_cast<size_t>(sy) * kImageSizePx;
    const int y = sy * 2;
    for (int sx = 0; sx < kImageSizePx; ++sx) {
      const uint16_t p = row[sx];
      if (p == kTransparentKey) {
        continue;
      }
      const int x = sx * 2;
      fb[ui::fbIndex(x, y)] = p;
      fb[ui::fbIndex(x + 1, y)] = p;
      fb[ui::fbIndex(x, y + 1)] = p;
      fb[ui::fbIndex(x + 1, y + 1)] = p;
    }
  }
}

void drawOverlay(size_t range) {
  const uint16_t ring_color = canvas.color565(90, 110, 120);
  for (int i = 1; i <= ui::radar::kRingCount; ++i) {
    const int r = kGridOuterRadius * i / ui::radar::kRingCount;
    canvas.drawCircle(kCenterX, kCenterY, r, ring_color);
    canvas.drawCircle(kCenterX, kCenterY, r - 1, ring_color);
  }
  canvas.drawFastHLine(kCenterX - kGridOuterRadius, kCenterY, kGridOuterRadius * 2, ring_color);
  canvas.drawFastVLine(kCenterX, kCenterY - kGridOuterRadius, kGridOuterRadius * 2, ring_color);
  canvas.fillCircle(kCenterX, kCenterY, px(3), TFT_WHITE);

  displayFontApply(canvas, ui::radar::kCardinalLabelHeightPx);
  canvas.setTextColor(TFT_WHITE);
  char label[16];
  snprintf(label, sizeof(label), "%.0f km", kRing3Km[range]);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.drawString(label, kCenterX + kGridOuterRadius * 3 / 4 + px(2), kCenterY - px(2));
}

void drawStatus(const Report& r, const char* image_time, unsigned long copy_ms) {
  displayFontApply(canvas, ui::radar::kCardinalLabelHeightPx);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(textdatum_t::middle_center);
  const int line = canvas.fontHeight();
  int y = kCenterY + kGridOuterRadius / 2 + line;
  char text[96];
  if (!r.ok) {
    canvas.drawString(r.error, kCenterX, kCenterY - line);
  }
  // Valid time of the image on screen, as HH:MMZ.
  snprintf(text, sizeof(text), "%.5sZ  png %u B  fetch %lu+%lu ms",
           strlen(image_time) >= 16 ? image_time + 11 : "--:--",
           static_cast<unsigned>(r.png_bytes), r.caps_ms, r.map_ms);
  canvas.drawString(text, kCenterX, y);
  y += line;
  snprintf(text, sizeof(text), "decode %lu ms (bg)  copy %lu ms", r.decode_ms, copy_ms);
  canvas.drawString(text, kCenterX, y);
  y += line;
  snprintf(text, sizeof(text), "int %u KB  TLS min %u  PNG min %u", r.idle_free, r.tls_min,
           r.decode_min);
  canvas.drawString(text, kCenterX, y);
}

void logReport(const Report& r) {
  Serial.printf(
      "weather: %s range %.0f km (outer %.0f km), time %s%s, caps %u B in %lu ms, png %u B in "
      "%lu ms, decode %lu ms (fetch task)\n",
      r.ok ? "ok" : "FAILED", kRing3Km[r.range], outerKm(r.range), r.time,
      r.time_reused ? " (reused)" : "", static_cast<unsigned>(r.caps_bytes), r.caps_ms,
      static_cast<unsigned>(r.png_bytes), r.map_ms, r.decode_ms);
  if (!r.ok) {
    Serial.printf("weather: error: %s\n", r.error);
  }
  Serial.printf(
      "weather: internal KB: idle %u, min during TLS %u, min during decode %u, largest block "
      "%u; PSRAM free %u KB\n",
      r.idle_free, r.tls_min, r.decode_min, r.largest,
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

/** Draws a full frame from the cached image; `reason` labels its timing line. */
void drawFrame(const char* reason) {
  const unsigned long t0 = millis();
  clearRound(canvas.color565(8, 16, 24));
  const unsigned long clear_ms = millis() - t0;

  Report r;
  size_t range = s_range;
  char image_time[32] = "";
  unsigned long copy_ms = 0;
  {
    // Held through the copy so the task can't publish over the image mid-frame; it only
    // waits for the lock to publish, never to fetch or decode.
    Lock lock;
    r = s_published;
    if (s_ready >= 0) {
      range = s_ready_range;
      memcpy(image_time, s_ready_time, sizeof(image_time));
      const unsigned long c0 = millis();
      copyImage2x(s_images[s_ready]);
      copy_ms = millis() - c0;
    }
  }
  drawOverlay(range);
  drawStatus(r, image_time, copy_ms);
  const unsigned long t1 = millis();
  displayPresent();
  Serial.printf("weather: %s frame: draw %lu ms (clear %lu ms, copy %lu ms), present %lu ms\n",
                reason, t1 - t0, clear_ms, copy_ms, millis() - t1);
}

}  // namespace

void weatherSpikeRun() {
  Serial.println("WEATHER SPIKE build: GeoMet radar around Milton, ON");
  if (!startFetchTask()) {
    for (;;) {
      Serial.println("weather: could not start the fetch task (PSRAM?)");
      delay(2000);
    }
  }
  if (!wifiSetupConnect()) {
    Serial.println("weather: no Wi-Fi; retrying");
  }

  unsigned long last_refresh_ms = 0;
  unsigned long last_draw_ms = 0;
  unsigned long last_reconnect_ms = 0;
  bool requested = false;
  uint32_t drawn_publish = 0;
  for (;;) {
    wifiLoop();
    bool range_changed = false;
    for (ButtonEvent e = buttonsConsumeEvent(); e != ButtonEvent::None;
         e = buttonsConsumeEvent()) {
      if (e == ButtonEvent::Up && s_range + 1 < kRangeCount) {
        ++s_range;
        range_changed = true;
      } else if (e == ButtonEvent::Down && s_range > 0) {
        --s_range;
        range_changed = true;
      }
    }

    if (WiFi.status() != WL_CONNECTED) {
      setEnabled(false);
      if (millis() - last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
        last_reconnect_ms = millis();
        wifiReconnect();
      }
      requested = false;  // fetch with a new time step once the link is back
    } else {
      setEnabled(true);
      if (!requested || millis() - last_refresh_ms >= kRefreshMs) {
        last_refresh_ms = millis();
        requested = true;
        requestFetch(true);
      } else if (range_changed) {
        // The screen keeps the old image until the new range arrives.
        requestFetch(false);
      }
    }

    uint32_t published = 0;
    Report r;
    {
      Lock lock;
      published = s_publish_count;
      r = s_published;
    }
    if (published != drawn_publish) {
      drawn_publish = published;
      last_draw_ms = millis();
      logReport(r);
      drawFrame("fetch");
    } else if (published != 0 && millis() - last_draw_ms >= kRedrawMs) {
      last_draw_ms = millis();
      drawFrame("redraw");
    }
    delay(10);
  }
}

#endif  // WEATHER_SPIKE
