/**
 * Plane Radar — WiFi setup, then the aircraft radar or the weather radar (one at a time) on
 * the Qualia's round 720×720 panel.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include <esp_core_dump.h>
#define PLANE_RADAR_CORE_DUMP_SUMMARY 1
#endif

#include "config.h"
#include "hardware/buttons.h"
#include "hardware/display.h"
#include "hardware/panel_test.h"
#include "services/adsb_client.h"
#include "services/clock.h"
#include "services/radar_location.h"
#include "services/weather_client.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"
#include "ui/weather_display.h"

namespace {

using ui::radar::Mode;

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
int g_drawn_minute = -1;  // clock minute on screen, so a minute change redraws
uint32_t g_drawn_publish = 0;  // services::adsb::publishCount() of the frame on screen
uint32_t g_drawn_weather_publish = 0;  // services::weather::publishCount() of the frame on screen
unsigned long g_last_draw_ms = 0;  // start of the last radar frame

/**
 * Kept in RTC memory, which a crash, watchdog or reset button leaves alone but power loss
 * doesn't: boots since power-on, and the uptime of the current run (so the next boot can
 * say how long the last one lasted). `magic` tells a kept record from power-on garbage.
 */
struct ResetLog {
  uint32_t magic;
  uint32_t boots;
  uint32_t uptime_s;
};
constexpr uint32_t kResetLogMagic = 0x52414452;  // "RADR"
RTC_NOINIT_ATTR ResetLog g_reset_log;
esp_reset_reason_t g_reset_reason = ESP_RST_UNKNOWN;
uint32_t g_previous_uptime_s = 0;
unsigned long g_last_status_ms = 0;

const char* resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power on";
    case ESP_RST_EXT:
      return "reset pin";
    case ESP_RST_SW:
      return "software restart";
    case ESP_RST_PANIC:
      return "panic (crash)";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "other watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep wake";
    case ESP_RST_BROWNOUT:
      return "brownout (supply dipped)";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_USB:
      return "USB";
    case ESP_RST_JTAG:
      return "JTAG";
    case ESP_RST_EFUSE:
      return "eFuse error";
    case ESP_RST_PWR_GLITCH:
      return "power glitch";
    case ESP_RST_CPU_LOCKUP:
      return "CPU lockup";
    default:
      return "unknown";
  }
}

/** Reads the reset reason and updates the RTC record. Call first thing in setup(). */
void recordBoot() {
  g_reset_reason = esp_reset_reason();
  if (g_reset_log.magic != kResetLogMagic || g_reset_reason == ESP_RST_POWERON) {
    g_reset_log = {kResetLogMagic, 0, 0};
  }
  g_previous_uptime_s = g_reset_log.uptime_s;
  ++g_reset_log.boots;
  g_reset_log.uptime_s = 0;
}

void logResetReason() {
  Serial.printf("Reset reason: %s; boot %u since power-on", resetReasonName(g_reset_reason),
                static_cast<unsigned>(g_reset_log.boots));
  if (g_reset_log.boots > 1) {
    Serial.printf(", previous run up %u s", static_cast<unsigned>(g_previous_uptime_s));
  }
  Serial.println();
}

/**
 * Prints where the last crash happened, from the core dump the panic handler saved to the
 * coredump partition. Decode the addresses against the matching firmware.elf, e.g.
 * xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/qualia/firmware.elf <addresses>.
 * The dump stays until the next crash overwrites it, so it can be older than this boot.
 */
void logLastCoreDump() {
#ifdef PLANE_RADAR_CORE_DUMP_SUMMARY
  if (esp_core_dump_image_check() != ESP_OK) {
    Serial.println("Core dump: none saved");
    return;
  }
  esp_core_dump_summary_t summary{};
  if (esp_core_dump_get_summary(&summary) != ESP_OK) {
    Serial.println("Core dump: saved, but its summary can't be read");
    return;
  }
  Serial.printf("Core dump: task %s, PC 0x%08lx, cause %lu, vaddr 0x%08lx, elf %.16s\n",
                summary.exc_task, static_cast<unsigned long>(summary.exc_pc),
                static_cast<unsigned long>(summary.ex_info.exc_cause),
                static_cast<unsigned long>(summary.ex_info.exc_vaddr),
                reinterpret_cast<const char*>(summary.app_elf_sha256));
  Serial.print("Core dump backtrace:");
  for (uint32_t i = 0; i < summary.exc_bt_info.depth; ++i) {
    Serial.printf(" 0x%08lx", static_cast<unsigned long>(summary.exc_bt_info.bt[i]));
  }
  Serial.println(summary.exc_bt_info.corrupted ? " (corrupted)" : "");
#endif
}

/** Keeps the RTC uptime current and prints a status line every kStatusLogIntervalMs. */
void updateStatus() {
  g_reset_log.uptime_s = millis() / 1000;
  if (millis() - g_last_status_ms < config::kStatusLogIntervalMs) {
    return;
  }
  g_last_status_ms = millis();
  Serial.printf("Status: up %u s, boot %u, last reset: %s\n",
                static_cast<unsigned>(g_reset_log.uptime_s),
                static_cast<unsigned>(g_reset_log.boots), resetReasonName(g_reset_reason));
}

/**
 * Internal RAM headroom (bounce buffers and TLS both come from it), and the least free stack
 * this task (loopTask, which draws) has had.
 */
void logInternalHeap(const char* when) {
  Serial.printf("Heap internal%s: free %u KB, min ever %u KB, largest block %u KB; "
                "loop stack min free %u B\n",
                when, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

/** Draws a frame of the current mode. `log` prints its timing; in-between frames pass false. */
void drawRadar(bool log) {
  // Read before drawing: a fetch that publishes mid-frame gets its own frame next pass.
  g_last_draw_ms = millis();
  if (ui::radar::mode() == Mode::Weather) {
    g_drawn_weather_publish = services::weather::publishCount();
    ui::weatherDisplayDraw(log);
  } else {
    g_drawn_publish = services::adsb::publishCount();
    ui::radarDisplayDraw(log);
  }
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  drawRadar(true);
  g_radar_visible = true;
}

/**
 * Points the fetch tasks at the current mode: only that mode's source fetches. Leaving plane
 * mode drops the aircraft, so coming back doesn't show old ones as live.
 */
void applyModeToFetchers(bool wifi_up) {
  const bool weather = ui::radar::mode() == Mode::Weather;
  services::adsb::setEnabled(wifi_up && !weather);
  services::weather::setEnabled(wifi_up && weather);
  if (!wifi_up) {
    return;
  }
  if (weather) {
    // Always the widest preset's extent: narrower ranges show its centre scaled up.
    const ui::radar::RangePreset& widest =
        ui::radar::kWeatherRangePresets[ui::radar::kWeatherRangePresetCount - 1];
    services::weather::setRequest(services::location::lat(), services::location::lon(),
                                  ui::radar::screenHalfWidthKm(widest));
  } else {
    // The fetch task picks up a new radius (range change) or location right away.
    services::adsb::setRequest(services::location::lat(), services::location::lon(),
                               ui::radar::fetchRadiusKm());
  }
}

void switchMode() {
  const Mode next = ui::radar::mode() == Mode::Weather ? Mode::Plane : Mode::Weather;
  ui::weatherDisplayReplayStop();
  if (next == Mode::Weather) {
    services::adsb::invalidate();
  }
  ui::radar::setMode(next);
  Serial.printf("Mode: %s\n", next == Mode::Weather ? "weather" : "plane");
  applyModeToFetchers(WiFi.status() == WL_CONNECTED);
}

/**
 * Taps: UP = zoom out, DOWN = zoom in. Holds: UP switches mode, DOWN replays the weather
 * radar. Drains every queued event, then redraws once.
 */
void handleButtons() {
  bool changed = false;
  bool replay = false;
  for (ButtonEvent event = buttonsConsumeEvent(); event != ButtonEvent::None;
       event = buttonsConsumeEvent()) {
    switch (event) {
      case ButtonEvent::Up:
        changed |= ui::radar::rangeNext();
        break;
      case ButtonEvent::Down:
        changed |= ui::radar::rangePrev();
        break;
      case ButtonEvent::UpHold:
        switchMode();
        changed = true;
        replay = false;
        break;
      case ButtonEvent::DownHold:
        replay = ui::radar::mode() == Mode::Weather;
        break;
      case ButtonEvent::None:
        break;
    }
  }
  ui::radar::rangeSaveIfDue();
  if (!g_radar_visible || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (changed) {
    char range_label[12];
    ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
    Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                  ui::radar::rangeCurrent().outer_km);
    // A zoom mid-replay ends it; the live image comes back at the new range.
    ui::weatherDisplayReplayStop();
    drawRadar(true);
  }
  if (replay && !ui::weatherDisplayReplayStart()) {
    Serial.println("Weather: nothing to replay yet");
  }
}

}  // namespace

void setup() {
  recordBoot();
  Serial.begin(115200);
  // USB CDC re-enumerates on every reset; give the monitor up to 3 s to reattach.
  while (!Serial && millis() < 3000) {
    delay(10);
  }
  Serial.println();
  Serial.println("Plane Radar");
  Serial.printf("Flash %u MB, PSRAM %u KB (free %u KB)\n",
                static_cast<unsigned>(ESP.getFlashChipSize() / (1024 * 1024)),
                static_cast<unsigned>(ESP.getPsramSize() / 1024),
                static_cast<unsigned>(ESP.getFreePsram() / 1024));

  displayInit();
  // Printed after the display is up: the lines before it are usually lost while the USB
  // serial port re-attaches after a reset.
  logResetReason();
  logLastCoreDump();
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
  // Fetch on core 0 from here on, once loop() enables the current mode's (Wi-Fi up).
  services::adsb::startFetchTask();
  if (!services::weather::startFetchTask()) {
    Serial.println("Weather: could not start the fetch task");
  }

  const bool connected = wifiSetupConnect();
  logInternalHeap(" (Wi-Fi setup done)");
  if (connected) {
    showRadarIfConnected();
  }
}

void loop() {
  updateStatus();
  wifiLoop();
  // Holding both buttons: the reset countdown owns the screen until they're let go.
  if (wifiResetCountdownShowing()) {
    delay(10);
    return;
  }
  if (wifiTakeResetCountdownCancelled()) {
    if (g_radar_visible) {
      drawRadar(false);
    } else {
      statusScreenConnectFailed();
    }
  }
  handleButtons();

  if (WiFi.status() != WL_CONNECTED) {
    applyModeToFetchers(false);
    ui::weatherDisplayReplayStop();
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
    applyModeToFetchers(true);
    // Read before drawing: if the minute ticks over mid-frame, the next pass redraws.
    const int minute = services::clock::minuteOfDay();
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (ui::radar::mode() == Mode::Weather) {
      if (ui::weatherDisplayReplaying()) {
        // New images and the clock wait until the replay ends; it draws the live frame then.
        ui::weatherDisplayReplayStep();
      } else if (const uint32_t published = services::weather::publishCount();
                 published != g_drawn_weather_publish) {
        // Read the count before the status: a publish in between gets looked at next pass.
        if (ui::weatherDisplayLiveChanged()) {
          drawRadar(true);
          logInternalHeap("");
        } else {
          // Only older images (the backfill): the screen would look the same.
          g_drawn_weather_publish = published;
        }
      } else if (minute != g_drawn_minute) {
        // The clock and the image's age.
        drawRadar(false);
      }
    } else if (services::adsb::publishCount() != g_drawn_publish) {
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
