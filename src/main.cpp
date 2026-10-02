/**
 * Plane Radar — WiFi setup, then radar UI on the Qualia's round 720×720 panel.
 */

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "hardware/buttons.h"
#include "hardware/display.h"
#include "hardware/panel_test.h"
#include "services/adsb_client.h"
#include "services/radar_location.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  ui::radarDisplayDraw();
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
    ui::radarDisplayDraw();
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (services::adsb::fetchUpdate(services::location::lat(),
                                  services::location::lon(), fetch_km)) {
    ui::radarDisplayRefreshAircraft();
  }
  handleButtons();
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
#ifdef PANEL_TEST
  panelTestRun();
#endif
  buttonsInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::adsb::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
}

void loop() {
  handleButtons();
  wifiLoop();

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
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
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    }
  }

  delay(10);
}
