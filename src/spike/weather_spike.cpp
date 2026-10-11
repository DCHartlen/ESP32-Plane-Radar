#ifdef WEATHER_SPIKE

#include "spike/weather_spike.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

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

// Fetched at half the screen size and drawn at 2x. The decoder's working memory depends on
// the image width, not on the area it covers, so the range doesn't change it.
constexpr int kImageSizePx = config::kDisplayWidth / 2;
constexpr float kImageScale =
    static_cast<float>(config::kDisplayWidth) / static_cast<float>(kImageSizePx);

// Same projection as ui/radar_geometry.
constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 3.14159265f / 180.0f;

/** Distance on ring 3 (of 4), as in ui/radar_range; the last two are the proposed extra steps. */
constexpr float kRing3Km[] = {5.0f, 10.0f, 15.0f, 25.0f, 50.0f, 100.0f};
constexpr size_t kRangeCount = sizeof(kRing3Km) / sizeof(kRing3Km[0]);
constexpr size_t kStartRange = 4;  // 50 km

constexpr unsigned long kRefreshMs = 5UL * 60UL * 1000UL;
constexpr unsigned long kRequestTimeoutMs = 15000;
// The whole body is held in PSRAM; a radar PNG this size is far below the cap.
constexpr size_t kBodyMaxBytes = 1024 * 1024;
constexpr size_t kBodyGrowBytes = 32 * 1024;

/** A response body in PSRAM, reused across fetches. Always NUL-terminated. */
struct Body {
  uint8_t* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
};

/** One fetch-and-draw cycle, shown on screen and in the log. */
struct Report {
  char time[32] = "";  // image valid time, ISO 8601 UTC
  char error[96] = "";
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

Body s_body;
size_t s_range = kStartRange;

unsigned internalFreeKb() {
  return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
}

/**
 * Lowest internal free RAM while `fn` runs. The monitor resets the "minimum free" figure,
 * so this measures this step alone, not the lowest point since boot.
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

float outerKm() { return kRing3Km[s_range] * 4.0f / 3.0f; }

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
  if (end == nullptr || end - (def + 9) >= static_cast<ptrdiff_t>(sizeof(r.time))) {
    snprintf(r.error, sizeof(r.error), "no time dimension in capabilities");
    Serial.printf("weather: capabilities start: %.200s\n", xml);
    return false;
  }
  memcpy(r.time, def + 9, end - (def + 9));
  r.time[end - (def + 9)] = '\0';
  return true;
}

/** Square bbox that maps to the full screen, so ring N of the radar lands on ring N here. */
bool fetchRadarPng(Report& r) {
  const float half_km = outerKm() * static_cast<float>(kCenterX) / kGridOuterRadius;
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
  Serial.printf("weather: GET %s\n", url.c_str());

  const unsigned long t0 = millis();
  bool ok = false;
  const unsigned tls_min =
      internalMinKbDuring([&] { ok = httpsGet(url, r.error, sizeof(r.error)); });
  r.tls_min = std::min(r.tls_min, tls_min);
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

void drawOverlay() {
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
  snprintf(label, sizeof(label), "%.0f km", kRing3Km[s_range]);
  canvas.setTextDatum(textdatum_t::bottom_left);
  canvas.drawString(label, kCenterX + kGridOuterRadius * 3 / 4 + px(2), kCenterY - px(2));
}

void drawStatus(const Report& r, bool image_ok) {
  displayFontApply(canvas, ui::radar::kCardinalLabelHeightPx);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(textdatum_t::middle_center);
  const int line = canvas.fontHeight();
  int y = kCenterY + kGridOuterRadius / 2 + line;
  char text[96];
  if (!image_ok) {
    canvas.drawString(r.error, kCenterX, kCenterY - line);
  }
  // Valid time as HH:MMZ.
  snprintf(text, sizeof(text), "%.5sZ  png %u KB", strlen(r.time) >= 16 ? r.time + 11 : "--:--",
           static_cast<unsigned>(r.png_bytes / 1024));
  canvas.drawString(text, kCenterX, y);
  y += line;
  snprintf(text, sizeof(text), "fetch %lu+%lu ms  decode %lu ms", r.caps_ms, r.map_ms,
           r.decode_ms);
  canvas.drawString(text, kCenterX, y);
  y += line;
  snprintf(text, sizeof(text), "int %u KB  TLS min %u  PNG min %u", r.idle_free, r.tls_min,
           r.decode_min);
  canvas.drawString(text, kCenterX, y);
}

void logReport(const Report& r, bool ok) {
  Serial.printf(
      "weather: %s range %.0f km (outer %.0f km), time %s, caps %u B in %lu ms, png %u B in "
      "%lu ms, decode %lu ms\n",
      ok ? "ok" : "FAILED", kRing3Km[s_range], outerKm(), r.time,
      static_cast<unsigned>(r.caps_bytes), r.caps_ms, static_cast<unsigned>(r.png_bytes),
      r.map_ms, r.decode_ms);
  if (!ok) {
    Serial.printf("weather: error: %s\n", r.error);
  }
  Serial.printf(
      "weather: internal KB: idle %u, min during TLS %u, min during decode %u, largest block "
      "%u; PSRAM free %u KB\n",
      r.idle_free, r.tls_min, r.decode_min, r.largest,
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

/** Fetches the latest image for the current range and draws a full frame. */
void refresh() {
  Report r;
  r.idle_free = internalFreeKb();
  r.largest = static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
  const bool fetched = fetchLatestTime(r) && fetchRadarPng(r);

  canvas.fillScreen(TFT_BLACK);
  canvas.fillCircle(kCenterX, kCenterY, kCenterX, canvas.color565(8, 16, 24));
  bool drawn = false;
  if (fetched) {
    const unsigned long t0 = millis();
    r.decode_min = internalMinKbDuring([&] {
      drawn = canvas.drawPng(s_body.data, s_body.len, 0, 0, 0, 0, 0, 0, kImageScale,
                             kImageScale);
    });
    r.decode_ms = millis() - t0;
    if (!drawn) {
      snprintf(r.error, sizeof(r.error), "PNG decode failed");
    }
  }
  drawOverlay();
  drawStatus(r, drawn);
  displayPresent();
  logReport(r, drawn);
}

}  // namespace

void weatherSpikeRun() {
  Serial.println("WEATHER SPIKE build: GeoMet radar around Milton, ON");
  if (!wifiSetupConnect()) {
    Serial.println("weather: no Wi-Fi; retrying");
  }

  unsigned long last_refresh_ms = 0;
  bool have_frame = false;
  unsigned long last_reconnect_ms = 0;
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
      if (millis() - last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
        last_reconnect_ms = millis();
        wifiReconnect();
      }
    } else if (!have_frame || range_changed || millis() - last_refresh_ms >= kRefreshMs) {
      last_refresh_ms = millis();
      have_frame = true;
      refresh();
    }
    delay(10);
  }
}

#endif  // WEATHER_SPIKE
