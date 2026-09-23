# Qualia ESP32-S3 port: build plan

This plan moves Plane Radar from the ESP32-C3 Super Mini with a 1.28" GC9A01 (240×240) to an
**Adafruit Qualia ESP32-S3 for TTL RGB-666** driving a **4" round 720×720 IPS panel with an NV3052C
driver** over a 40-pin connector. The work spans several sessions. Update the **Progress** section
as phases land.

## Decisions (agreed)

- The Qualia **replaces** the C3 target. There's one PlatformIO environment, `qualia`, and no
  display abstraction for two boards.
- Assume the panel matches Adafruit's **HD40015C40** and use Arduino_GFX `hd40015c40_init_operations`.
- **PlatformIO stays** and moves to the **pioarduino** platform (Arduino core 3.x / IDF 5), because
  IDF 4 builds flicker on this panel.
- **Display architecture:** Arduino_GFX (`Arduino_XCA9554SWSPI` + `Arduino_ESP32RGBPanel` +
  `Arduino_RGB_Display`) only initializes the panel and owns the framebuffer. All drawing stays in
  LovyanGFX, into a 720×720 16-bit `LGFX_Sprite` in PSRAM, which is pushed with
  `draw16bitBeRGBBitmap`. The sprite stores RGB565 byte-swapped.
- **Buttons:** Qualia UP/DOWN on the PCA9554 expander (0x3F, pins 5/6), polled from a FreeRTOS
  task. UP = zoom out, DOWN = zoom in, clamped at the ends. Holding **UP for 3 s** resets. **GPIO0/BOOT is
  display line B4 and must not be used at runtime.** It still works as a strapping pin for
  flashing (BOOT+RESET).
- **No hard-coded pixel sizes.** Layout is derived from the screen size, and element sizes scale
  with a single tunable `kUiDensity`. The screens have about the same ppi (187 vs 180), so
  density 1.0 is about 3× the old physical size. Start at 0.67 (about 2×).
- **New larger VLW fonts are required.**
- Radar refresh every **5 s**. Smoothness isn't needed, but the panel's pixel clock still has to be
  high enough to avoid scan flicker.

## Hardware reference

Pins come from arduino-esp32 `variants/adafruit_qualia_s3_rgb666/pins_arduino.h`:

| Signal | GPIO |
|---|---|
| R1..R5 | 11, 10, 9, 46, 3 |
| G0..G5 | 48, 47, 21, 14, 13, 12 |
| B1..B5 | 40, 39, 38, **0**, 45 |
| PCLK / DE / HSYNC / VSYNC | 1 / 2 / 41 / 42 |
| I2C SDA / SCL | 8 / 18 |

PCA9554A expander @ 0x3F: TFT_SCK 0, TFT_CS 1, TFT_RESET 2, CPT_IRQ 3, BACKLIGHT 4, BUTTON_UP 5,
BUTTON_DOWN 6, TFT_MOSI 7. The expander INT isn't wired to the ESP32, so it has to be polled.
Backlight is on/off through the expander. PWM needs the bottom jumper soldered and uses pin A1 (GPIO16).

Panel timings from the community HD40015C40 examples:
- hsync: polarity 1, front porch 46, pulse 2, back porch 44
- vsync: polarity 1, front porch 50, pulse 16, back porch 16
- pclk active-neg 1

The examples use a 6 MHz pclk. Aim for about 12–16 MHz with bounce buffers.

The board is ESP32-S3 N16R8: 16 MB QIO flash and 8 MB OPI PSRAM (`memory_type = qio_opi`).

---

## Phase 1: Build system

- `platformio.ini`: replace `[env:supermini]` with `[env:qualia]` on a **pinned** pioarduino
  release.
- Board: use `adafruit_qualia_s3_rgb666` if the platform ships it. Otherwise use
  `boards/adafruit_qualia_s3_rgb666.json` or `esp32-s3-devkitc-1` with qio_opi, 16MB, OPI PSRAM
  and `-DBOARD_HAS_PSRAM`.
- Flags: `-std=gnu++17`, `ARDUINO_USB_MODE=0` (TinyUSB, the board default; CDC-on-boot comes
  from the board config), `WM_NODEBUG`, `WM_MDNS`.
- `lib_deps`: add `moononournation/GFX Library for Arduino`, pinned to a version that works with core 3.x.
- Partitions (16 MB). The board config always flashes TinyUF2 at 0x410000, so the layout keeps
  that slot and puts a 4 MB OTA app slot on each side of it:
  ```
  nvs,      data, nvs,      0x9000,   0x5000,
  otadata,  data, ota,      0xe000,   0x2000,
  app0,     app,  ota_0,    0x10000,  0x400000,
  uf2,      app,  factory,  0x410000, 0x40000,
  app1,     app,  ota_1,    0x450000, 0x400000,
  spiffs,   data, spiffs,   0x850000, 0x7A0000,
  coredump, data, coredump, 0xFF0000, 0x10000,
  ```
- Merged image: pioarduino writes `firmware.factory.bin` on every build, so
  `scripts/merge_firmware.py` was removed. `merge-firmware.sh` and CI use the factory image.
- **Check:** it builds, `firmware.factory.bin` is produced, and on hardware `ESP.getPsramSize()` ≈ 8 MB and the flash is 16 MB.

## Phase 2: Getting the display running (all the hardware risk)

- `config.h`: remove the GC9A01/SPI, `kBootPin`, `kDisplayInvert` and `kDisplayRgbOrder` settings. Add the
  RGB pins, I2C pins, expander address and pins, timings, `kPanelPclkHz`, 720×720, and
  `kAdsbFetchIntervalMs = 5000`.
- New `hardware/panel.{h,cpp}` is the only file that includes Arduino_GFX headers:
  ```cpp
  bool panelInit();
  void panelBacklight(bool on);
  void panelPushBe565(const uint16_t* buf, int x, int y, int w, int h);
  bool panelReadButton(uint8_t pca_pin);   // expander access guarded by a mutex
  ```
  Enable bounce buffers if the Arduino_GFX version has that option.
- `hardware/display.{h,cpp}`: `extern LGFX_Sprite canvas;` (PSRAM, 720×720, 16-bit),
  `displayInit()`, `displayPresent()`, `displayPresentRect(x,y,w,h)`. Delete `lgfx_config.hpp`.
  `tft` goes away and its users switch to `canvas`. If the canvas can't be allocated, stop with a
  clear error. That means PSRAM is misconfigured, so there's no draw-straight-to-panel fallback.
- **Check** with a temporary `-DPANEL_TEST` build:
  1. The R/G/B bars show the right colors.
  2. Concentric circles are round, centered and fully visible.
  3. Text is sharp.
  4. There's no jitter during looped HTTPS.
  5. Log the idle and pressed levels of expander pins 5 and 6 to find the button polarity.

## Phase 3: Buttons

- New `hardware/buttons.{h,cpp}`:
  ```cpp
  enum class ButtonEvent : uint8_t { None, Up, Down };
  void buttonsInit();                 // after displayInit()
  ButtonEvent buttonsConsumeEvent();
  bool buttonsResetRequested();
  ```
  - The task runs on core 0 at low priority with a 20 ms `vTaskDelayUntil`.
  - Debounce needs 2 identical samples in a row.
  - A tap is counted on release if the hold was shorter than 3 s.
  - Taps go into a FreeRTOS queue with depth 4.
  - The reset flag is set while UP is still held.
- Remove the BOOT ISR code from `wifi_setup.cpp`. Replace `bootButtonPollLongPress()` call sites
  (`waitForLinkWithUi`, `openConfigPortal`, `wifiLoop`) with a check of `buttonsResetRequested()` that
  calls `wifiResetCredentialsAndReboot()` from the main context.
- `radar_range`: add `rangePrev()` and clamp instead of wrapping. Optionally delay the NVS save about
  2 s after the last tap.
- Status text: "Hold BOOT 3 sec" becomes "Hold UP 3 sec".
- **Check:** taps count during a fetch, a double tap moves two steps, and holding 3 s resets into the portal.

## Phase 4: Sizes relative to the screen

- `ui/radar_theme.h`:
  ```cpp
  constexpr int   kSize = config::kDisplayWidth;
  constexpr float kDesignSize = 240.0f;   // element sizes written in 1/240ths of the screen
  constexpr float kUiDensity = 0.67f;     // 1.0 = original proportions; ~0.33 = original physical size
  constexpr float kElementScale = kSize / kDesignSize * kUiDensity;
  constexpr int ui(float design_px);      // max(1, round(design_px * kElementScale))
  constexpr int kGridOuterRadius = kCenterX - kCardinalLabelHeightPx + ui(1);
  ```
  Every element constant that's currently in px becomes `ui(<old value>)`.
- Remaining hard-coded values to replace:

  | Where | Current value | New value |
  |---|---|---|
  | Scale label | pads 3/2 | `ui(3)`/`ui(2)` |
  | Tag clamps | 1 | `ui(1)` |
  | Runway labels | pads 2/1, fake-bold ±1 | `ui()` versions |
  | Cardinal offsets | -1/3 | `ui()` |
  | Status screens | `kLineGap` 6, `kSpinnerRadius` 113, dot radii 2/4, text width 220, `kPanelPadY` 8 | `ui(6)`, `kCenterX - ui(7)`, `ui()`, `0.9f * kSize`, `ui(8)` |

- Rings: replace the stacked `drawCircle` with `fillArc(cx, cy, r - w, r, 0, 360)`, which avoids pinholes when the line is thick.
- **Bug:** `segmentIntersectsDisc` overflows 32-bit int at 720 px (b² ≈ 2×10¹⁰). Switch it to
  `int64_t` or `float`.
- Move the duplicated projection and clip helpers into `ui/radar_geometry.{h,cpp}`, which fixes the
  "keep in sync" issue between `radar_display.cpp` and `runway_overlay.cpp`.
- `initPalette()`: remove the aircraft red/blue swap, because this panel is standard RGB565.
- **Check:**
  - At density 1.0 the screen should look like the old UI scaled 3×.
  - Try 0.5 and 0.67 and pick one.
  - Look at runways near the edge at the 5 km range.

## Phase 5: Fonts

- New `scripts/build_vlw_font.py` (freetype-py) writes the VLW format:
  - Header: glyph count, version 11, size, pad, ascent, descent.
  - 7 int32 measurements per glyph.
  - 8-bit alpha bitmaps.
- Source font: Noto Sans Bold, stored at `fonts/NotoSans-Bold.ttf` with `OFL.txt`.
- Characters: ASCII 0x20–0x7E, plus `…` and `°`.
- Output: `data/ui_font_small.vlw` (~32 px) and `data/ui_font_large.vlw` (~64 px), both embedded.
  Text is only ever scaled down, between 0.5× and 1.0×. The script prints each file's cap height.
- `display_font`: `displayFontApply(gfx, cap_height_px)` picks the smallest file whose native height is
  at least the target, and sets `setTextSize(target/native)`. It tracks which font is loaded on each canvas.
  The duplicated `findVlwSizeForHeight` helpers go away. Keep one bitmap fallback (`FreeSansBold24pt7b`).
- **Check:** text is sharp at every size, labels line up, and a long SSID gets an ellipsis.

## Phase 6: Drawing code and screens

- `radar_display.cpp`: draw into `canvas` and then `displayPresent()`. Remove `DrawScope`/`s_draw`
  and the fallback that draws straight to the panel. Log render and push times.
- `status_screens.cpp`: draw into `canvas`. The spinner pushes only small areas around each dot with
  `displayPresentRect`.
- `wifi_setup.cpp`: remove the `setTxPower(WIFI_POWER_8_5dBm)` C3 workaround, then test the connection.
- Memory: about 1 MB framebuffer + 1 MB canvas out of 8 MB PSRAM. `kMaxAircraft` can stay at 64.
- **Check** the whole flow on the device:
  1. Portal screen, spinner, radar.
  2. Both buttons.
  3. A Wi-Fi drop and reconnect.
  4. The LAN portal save.
  5. Runways on and off.
  6. Miles and km.

## Phase 7: Cleanup and docs

- `README.md`: hardware, buttons, flashing (BOOT+RESET, ESP32-S3, 16 MB), 5 s fetch.
- `CLAUDE.md`: env, new modules, font script, flash layout.
- Workspace and `.vscode` env names. The untracked `parts` note goes into the README or gets deleted.
- Confirm CI and the release workflow with `workflow_dispatch`.

## Things only the hardware can settle

1. **Pixel clock and bounce buffers.** Tune them in Phase 2. The fallback is a lower clock.
2. **Button polarity.** Measure it in Phase 2 and store it as a config constant.
3. **Display glitches during NVS writes.** Watch for them in Phase 3. The fallback is the delayed save.
4. **Arduino_GFX with core 3.x.** If the pinned version doesn't build, try another release. Don't fall
   back to core 2.x.

---

## Progress

| Phase | Status | Notes |
|---|---|---|
| 1 Build system | Done (build); hardware check pending | Builds on pioarduino 55.03.312-1 (core 3.3.12, IDF 5.5.5): 1.42 MB app, 28% RAM. The boot log prints flash and PSRAM size. Flash it and confirm about 8 MB PSRAM. |
| 2 Display | Not started | |
| 3 Buttons | Not started | |
| 4 Relative geometry | Not started | |
| 5 Fonts | Not started | |
| 6 Drawing code | Not started | |
| 7 Cleanup and docs | Not started | |

### Build environment notes (Windows)

- Run `pio` from **PowerShell or cmd, not Git Bash**. ESP-IDF's `idf_tools.py` refuses MSys/Mingw
  ("MSys/Mingw is not supported"), so the toolchain doesn't install and compiles fail with
  `xtensa-esp32s3-elf-g++ is not recognized`. The executable is `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.
- If a toolchain install is interrupted by a file lock, delete
  `~/.platformio/packages/toolchain-xtensa-esp-elf` and `~/.platformio/tools/toolchain-xtensa-esp-elf`,
  then rebuild. File locks can come from VS Code's PlatformIO extension running at the same time, or from Defender.
- Core 3.x API change: `HTTPClient::getStreamPtr()` returns `NetworkClient*`, not `WiFiClient*`.
- USB: the board default is USB-OTG/TinyUSB (`ARDUINO_USB_MODE=0`) with CDC on boot. Upload uses a
  1200-baud touch. If upload can't find the port, hold BOOT and tap RESET.
