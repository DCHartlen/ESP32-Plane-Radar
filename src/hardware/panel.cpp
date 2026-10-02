#include "hardware/panel.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_private/periph_ctrl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <soc/periph_defs.h>

#include <cstring>

#include "config.h"

namespace {

using namespace config;

// Drawing goes into the framebuffers through the CPU cache, and the bounce-buffer ISR
// also reads them through the cache, so no cache flush is needed. Scanning straight from
// PSRAM (no bounce buffers) would need one.
static_assert(kPanelBounceBufferPx > 0, "the double-buffered panel needs bounce buffers");

constexpr size_t kFramePixels = static_cast<size_t>(kDisplayWidth) * kDisplayHeight;
/** A refresh takes ~40 ms at 16 MHz; a swap that waits longer than this is logged. */
constexpr TickType_t kSwapTimeoutTicks = pdMS_TO_TICKS(150);

Arduino_XCA9554SWSPI s_expander(kExpanderPinTftReset, kExpanderPinTftCs, kExpanderPinTftSck,
                                kExpanderPinTftMosi, &Wire, kExpanderAddr);

esp_lcd_panel_handle_t s_panel = nullptr;
uint16_t* s_fb[2] = {nullptr, nullptr};
int s_back = 0;

/** Given by the ISR once the buffer that was on screen before a swap has been read out. */
SemaphoreHandle_t s_swap_done = nullptr;
volatile bool s_swap_pending = false;

SemaphoreHandle_t s_expander_mutex = nullptr;
bool s_ready = false;

class ExpanderLock {
 public:
  ExpanderLock() { xSemaphoreTake(s_expander_mutex, portMAX_DELAY); }
  ~ExpanderLock() { xSemaphoreGive(s_expander_mutex); }
};

bool expanderPresent() {
  Wire.beginTransmission(kExpanderAddr);
  return Wire.endTransmission() == 0;
}

/** Same steps as Arduino_RGB_Display::begin() without its framebuffer. */
void sendPanelInitSequence() {
  s_expander.begin();  // also pulses the panel reset line on the expander
  s_expander.sendCommand(0x01);  // software reset
  delay(120);
  s_expander.batchOperation(hd40015c40_init_operations, sizeof(hd40015c40_init_operations));
}

// Runs once a framebuffer has been fully read out, which is when the next refresh picks
// up the buffer the last draw_bitmap selected.
bool IRAM_ATTR onFrameBufComplete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*,
                                  void*) {
  if (!s_swap_pending) {
    return false;
  }
  s_swap_pending = false;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_swap_done, &woken);
  return woken == pdTRUE;
}

bool startRgbPanel() {
  // A warm reboot leaves LCD_CAM running, and the new panel can then latch onto the old
  // VSYNC phase, shifting the image down until a power cycle. Reset it first.
  periph_module_reset(PERIPH_LCD_CAM_MODULE);

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = kPanelPclkHz;
  cfg.timings.h_res = kDisplayWidth;
  cfg.timings.v_res = kDisplayHeight;
  cfg.timings.hsync_pulse_width = kPanelHsyncPulseWidth;
  cfg.timings.hsync_back_porch = kPanelHsyncBackPorch;
  cfg.timings.hsync_front_porch = kPanelHsyncFrontPorch;
  cfg.timings.vsync_pulse_width = kPanelVsyncPulseWidth;
  cfg.timings.vsync_back_porch = kPanelVsyncBackPorch;
  cfg.timings.vsync_front_porch = kPanelVsyncFrontPorch;
  cfg.timings.flags.hsync_idle_low = kPanelHsyncPolarity == 0;
  cfg.timings.flags.vsync_idle_low = kPanelVsyncPolarity == 0;
  cfg.timings.flags.pclk_active_neg = kPanelPclkActiveNeg;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 2;
  cfg.bounce_buffer_size_px = kPanelBounceBufferPx;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = kPanelPinHsync;
  cfg.vsync_gpio_num = kPanelPinVsync;
  cfg.de_gpio_num = kPanelPinDe;
  cfg.pclk_gpio_num = kPanelPinPclk;
  cfg.disp_gpio_num = -1;
  // RGB565 data lines, LSB first: B1..B5, G0..G5, R1..R5.
  int line = 0;
  for (int8_t pin : kPanelPinB) cfg.data_gpio_nums[line++] = pin;
  for (int8_t pin : kPanelPinG) cfg.data_gpio_nums[line++] = pin;
  for (int8_t pin : kPanelPinR) cfg.data_gpio_nums[line++] = pin;
  cfg.flags.fb_in_psram = 1;

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
  if (err != ESP_OK) {
    Serial.printf("panel: esp_lcd_new_rgb_panel failed: %s\n", esp_err_to_name(err));
    s_panel = nullptr;
    return false;
  }

  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_frame_buf_complete = onFrameBufComplete;
  esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr);

  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);

  void* fb0 = nullptr;
  void* fb1 = nullptr;
  err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1);
  if (err != ESP_OK) {
    Serial.printf("panel: get_frame_buffer failed: %s\n", esp_err_to_name(err));
    return false;
  }
  s_fb[0] = static_cast<uint16_t*>(fb0);
  s_fb[1] = static_cast<uint16_t*>(fb1);
  memset(s_fb[0], 0, kFramePixels * sizeof(uint16_t));
  memset(s_fb[1], 0, kFramePixels * sizeof(uint16_t));
  // Driver 0 is on screen after init, so draw into 1 first.
  s_back = 1;

  // Re-sync the scan-out to VSYNC as well, in case the reset above didn't fully align it.
  esp_lcd_rgb_panel_restart(s_panel);
  return true;
}

}  // namespace

bool panelInit() {
  if (s_ready) {
    return true;
  }
  s_expander_mutex = xSemaphoreCreateMutex();
  s_swap_done = xSemaphoreCreateBinary();
  Wire.begin(kI2cPinSda, kI2cPinScl, kI2cHz);

  if (!expanderPresent()) {
    Serial.printf("panel: no PCA9554 at 0x%02X\n", kExpanderAddr);
    return false;
  }

  ExpanderLock lock;
  sendPanelInitSequence();
  if (!startRgbPanel()) {
    return false;
  }

  s_expander.pinMode(kExpanderPinButtonUp, INPUT);
  s_expander.pinMode(kExpanderPinButtonDown, INPUT);
  s_expander.pinMode(kExpanderPinBacklight, OUTPUT);
  s_expander.digitalWrite(kExpanderPinBacklight, HIGH);

  Serial.printf("panel: %dx%d, pclk %ld Hz, bounce %u px, 2 framebuffers\n", kDisplayWidth,
                kDisplayHeight, static_cast<long>(kPanelPclkHz),
                static_cast<unsigned>(kPanelBounceBufferPx));
  s_ready = true;
  return true;
}

void panelBacklight(bool on) {
  if (!s_ready) {
    return;
  }
  ExpanderLock lock;
  s_expander.digitalWrite(kExpanderPinBacklight, on ? HIGH : LOW);
}

uint16_t* panelBackBuffer() { return s_fb[s_back]; }

void panelSwap() {
  if (!s_ready) {
    return;
  }
  // Drop a give left over from a swap that timed out.
  xSemaphoreTake(s_swap_done, 0);
  // With one of the driver's own buffers, draw_bitmap selects it instead of copying.
  esp_lcd_panel_draw_bitmap(s_panel, 0, 0, kDisplayWidth, kDisplayHeight, s_fb[s_back]);
  // Set after draw_bitmap: if the frame ends in between, this waits one refresh longer
  // instead of returning while the old buffer is still on screen.
  s_swap_pending = true;
  if (xSemaphoreTake(s_swap_done, kSwapTimeoutTicks) != pdTRUE) {
    s_swap_pending = false;
    Serial.println("panel: swap timed out");
  }
  s_back ^= 1;
}

void panelResync() {
  if (!s_ready) {
    return;
  }
  esp_lcd_rgb_panel_restart(s_panel);
  Serial.println("panel: scan-out resync");
}

bool panelReadButton(uint8_t pca_pin) {
  if (!s_ready) {
    return false;
  }
  ExpanderLock lock;
  return s_expander.digitalRead(pca_pin) == HIGH;
}
