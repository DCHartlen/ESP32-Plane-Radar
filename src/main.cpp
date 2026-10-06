/**
 * Plane Radar — WiFi setup, then radar UI on the Qualia's round 720×720 panel.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "hardware/buttons.h"
#include "hardware/display.h"
#include "hardware/panel_test.h"
#include "services/adsb_client.h"
#include "services/clock.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
int g_drawn_minute = -1;  // clock minute on screen, so a minute change redraws
uint32_t g_drawn_publish = 0;  // services::adsb::publishCount() of the frame on screen
unsigned long g_last_draw_ms = 0;  // start of the last radar frame

/** Internal RAM headroom (bounce buffers and TLS both come from it). */
void logInternalHeap(const char* when) {
  Serial.printf("Heap internal%s: free %u KB, min ever %u KB, largest block %u KB\n", when,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
}

/** Draws a radar frame. `log` prints its timing; animation frames in between pass false. */
void drawRadar(bool log) {
  // Read before drawing: a fetch that publishes mid-frame gets its own frame next pass.
  g_drawn_publish = services::adsb::publishCount();
  g_last_draw_ms = millis();
  ui::radarDisplayDraw(log);
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  drawRadar(true);
  g_radar_visible = true;
}

/** UP = zoom out, DOWN = zoom in. Drains every queued tap, then redraws once. */
void handleButtons() {
  bool changed = false;
  for (ButtonEvent event = buttonsConsumeEvent(); event != ButtonEvent::None;
       event = buttonsConsumeEvent()) {
    changed |= (event == ButtonEvent::Up) ? ui::radar::rangeNext() : ui::radar::rangePrev();
  }
  ui::radar::rangeSaveIfDue();
  if (!changed) {
    return;
  }

  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    drawRadar(true);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // USB CDC re-enumerates on every reset; give the monitor up to 3 s to reattach.
  while (!Serial && millis() < 3000) {
    delay(10);
  }
  Serial.println();
  Serial.println("Plane Radar");
  Serial.printf("Reset reason: %d\n", static_cast<int>(esp_reset_reason()));
  Serial.printf("Flash %u MB, PSRAM %u KB (free %u KB)\n",
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
                static_cast<unsigned>(ESP.getPsramSize() / 1024),
                static_cast<unsigned>(ESP.getFreePsram() / 1024));

  displayInit();
  logInternalHeap(" (display up)");
  // Wi-Fi's first connect writes to flash, which can stall the panel refill and leave the
  // image shifted up. Re-align the scan-out once the link is up.
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t) { displayResync(); },
               ARDUINO_EVENT_WIFI_STA_GOT_IP);
#ifdef PANEL_TEST
  panelTestRun();
#endif
  buttonsInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::clock::init();
  // Fetches on core 0 from here on, once loop() enables it (Wi-Fi up).
  services::adsb::startFetchTask();

  const bool connected = wifiSetupConnect();
  logInternalHeap(" (Wi-Fi setup done)");
  if (connected) {
    showRadarIfConnected();
  }
}

void loop() {
  handleButtons();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    services::adsb::setEnabled(false);
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
      // Don't bring the old aircraft back when the radar returns (a fetch still in flight
      // is dropped too).
      services::adsb::invalidate();
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    services::clock::loop();
    // The fetch task picks up a new radius (range change) or location right away.
    services::adsb::setRequest(services::location::lat(), services::location::lon(),
                               ui::radar::fetchRadiusKm());
    services::adsb::setEnabled(true);
    // Read before drawing: if the minute ticks over mid-frame, the next pass redraws.
    const int minute = services::clock::minuteOfDay();
    const uint32_t published = services::adsb::publishCount();
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (published != g_drawn_publish) {
      // A fetch finished. Redraw on failure too, so stale aircraft give way to the error.
      drawRadar(true);
      logInternalHeap("");
    } else if (ui::radarDisplayAnimating() &&
               millis() - g_last_draw_ms >= config::kRadarRedrawIntervalMs) {
      // Dead reckoning moves the aircraft between fetches.
      drawRadar(false);
    } else if (minute != g_drawn_minute) {
      // Nothing is moving, so frames only come with fetches; keep the clock from lagging.
      drawRadar(false);
    }
    g_drawn_minute = minute;
  }

  delay(10);
}
