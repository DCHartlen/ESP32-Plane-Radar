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

## Open issues

### Pixel clock: 16 MHz works with 36-line bounce buffers (resolved 2026-10-02)

- **Was:** at 16 MHz pclk (~25 Hz refresh) with 10-line bounce buffers, Wi-Fi never connected,
  even in STA mode (status 6/4), and the setup AP was unusable. 12 MHz (~18 Hz) worked but thin
  lines and dark shades shimmered.
- **Fix:** `kPanelBounceBufferPx = 720 * 36`, from pvanbaren's port of the same board (see
  `docs/improvements-plan.md`). Nothing else changed: same Arduino_GFX path, same prebuilt
  framework, `pclk_active_neg` still 1. 20 lines was never tried at 16 MHz, so the minimum
  that works isn't known.
- **Verified on hardware:** panel test 9/9 fetches; full firmware runs for minutes with
  fetches ~1.2 s, range changes with NVS saves cause no frame roll, hold-UP reset into the
  setup AP joins quickly with no drops, and it reconnects to STA after setup. Text and fine
  detail look clearer.
- **Costs:** the two bounce buffers take 103.7 KB of internal RAM (46 KB more than 20 lines).
  Internal heap: 94 KB free, 46 KB minimum, 35 KB largest block in normal running; 37 KB
  minimum after the portal and reconnect. PSRAM draws are ~30% slower because the scan-out
  reads PSRAM faster: draw ~140 ms empty and ~245 ms at 20 aircraft (was 107 and 185), present
  ~110 ms (was 100).
- **If Wi-Fi turns fragile again:** `custom_sdkconfig` with
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n` (pioarduino recompiles the framework, first build
  10–30 min), then PSRAM at 120 MHz, then dropping pclk while the portal is open.

### Frame roll with bounce buffers (2026-10-02, once since 36-line buffers; fix pending test)

- **Symptom:** the whole image rolled up ~30-40 rows (N drawn under S) and stayed that way for
  minutes while the radar kept redrawing. It's a bounce-buffer refill slip (20-line buffers), and
  the framework's `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` didn't resync it.
- **Likely triggers:** flash writes (NVS range save, Wi-Fi connect) while the RGB ISR isn't
  IRAM-safe, and PSRAM load from the 1 MB present copy.
- **Status:** not seen at 16 MHz with 36-line buffers, including range changes with NVS saves
  over several minutes (2026-10-02). The no-copy esp_lcd rewrite (`docs/improvements-plan.md`,
  roadmap step 1) has since removed the 1 MB copy load.
- **One slip after the rewrite:** in the panel test, the image shifted up right after Wi-Fi first
  connected (the driver writes to flash then) and stayed shifted until reset. Minutes of running
  and button presses didn't cause another. Fix: `setup()` calls `displayResync()`
  (`esp_lcd_rgb_panel_restart()`, re-aligns at the next VSYNC) on `ARDUINO_EVENT_WIFI_STA_GOT_IP`;
  the serial log prints `panel: scan-out resync`. **Not yet verified on hardware.**
- **If slips show up at other times:** call `displayResync()` after those flash writes too, or
  rebuild the framework with `custom_sdkconfig`: `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y` (the actual
  cause: the refill ISR can't run while flash is written), possibly with
  `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`, `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`,
  `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=n`.

### Wi-Fi credentials lost after a hold-UP reset (fixed 2026-10-02)

- **Symptom:** after hold-UP reset and setup through the portal, the device connected, but the
  next reboot had no saved credentials.
- **Cause:** core 3.x picks credential storage (flash or RAM) each time Wi-Fi starts, from
  `WiFi.persistent()`, and `WIFI_OFF` shuts Wi-Fi down fully. `eraseWifiCredentials()` left it
  `false`, so the portal started Wi-Fi in RAM-only mode.
- **Fix:** `eraseWifiCredentials()` sets `WiFi.persistent(true)` (the core default) when done.
  Verified: hold-UP, setup, then RESET reconnects without the portal.

### Appearance tuning (later)

- **Track line length:** changed (2026-10-02) to a true 30 s prediction at the active
  range, drawn from the aircraft's position (was 60 s × 0.3 at a fixed 13.3 km scale from
  the nose, so its meaning changed with zoom). Check the 5 km preset near busy airports;
  if it's too cluttered, lower `kAircraftTrackHorizonSec` (20 s is the fallback).
- **Tag overlap:** in dense traffic (CYYZ departures) the three-line tags overlap each other
  and nearby symbols. This is pre-existing behavior, made more visible by the larger text. It
  needs a decluttering pass (for example, flip a tag to the other side or drop lines on collision).
- **Heading vs track:** the arrow uses heading (true, else magnetic) and the line uses ground
  track, so they differ by the crab angle, plus ~10° declination when only `mag_heading` is
  reported. Option: draw the arrow along the track.
- **Backlight** is bright. Dimming needs the PWM jumper (pin A1 / GPIO16).

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
| 1 Build system | Done, verified on hardware | Builds on pioarduino 55.03.312-1 (core 3.3.12, IDF 5.5.5): 1.42 MB app, 28% RAM. esptool reports 8 MB PSRAM and 16 MB flash. |
| 2 Display | Done, verified on hardware | **Hardware results (2026-10-01):** colors, R/G/B order and byte order correct; circles round, centered, edge fully visible; buttons active-low (`kButtonActiveLow`). Text is fuzzy because the old font is upscaled (fixed in Phase 5). **Pclk vs Wi-Fi:** at 12 MHz (~18 Hz refresh) dark shades flicker, green most. At 16 MHz the setup AP was unusable (phones got no IP, AP dropped) and STA can't connect at all (status 4, connect failed); at 8 MHz the portal works. At 12 MHz STA + HTTPS is clean (19/19 fetches, ~1 s each, full present ~75 ms). **Pclk is set to 12 MHz.** The flicker is thin-line/dim-shade shimmer from the low refresh, not aliasing. The setup AP at 12 MHz was verified in Phase 3. No jitter was reported during fetches (not checked closely). Options for going faster: Wi-Fi/lwIP buffers in internal RAM (`custom_sdkconfig`), PSRAM at 120 MHz, smaller porches, or dropping pclk while the portal is open. The C3 `setTxPower(8.5 dBm)` workaround was removed early (it crippled the AP). Diagnostics reverted (`WM_NODEBUG` back). The boot now waits up to 3 s for the USB monitor and prints the reset reason. Original notes: | `panel`, `display` (PSRAM `canvas`) and the `qualia_panel_test` env build. Starting values: pclk 12 MHz, 7200 px bounce buffer, rotation 0 (`kDisplayRotate180` flips the canvas). BOOT/GPIO0 handling is stubbed out until Phase 3. The radar still uses the 240 px layout (top-left of the screen) until Phase 4. Test: `pio run -e qualia_panel_test -t upload`, then check the five items above against the serial log. |
| 3 Buttons | Done, verified on hardware | Taps, double tap, tap during fetch, hold-UP reset into the portal, and the setup portal at 12 MHz all work. The earlier weak, flaky AP was mostly a metallized anti-static bag under the board's antenna. **Keep metal away from the antenna end in the enclosure.** Fallback if the AP is flaky again: own the RGB panel through esp_lcd in `panel.cpp` (Arduino_GFX keeps only the init sequence) so pclk can drop to 8 MHz while the portal is open. Note: a reset wipes the location, and the portal pre-fills the Amsterdam default. | `hardware/buttons` task (core 0, 20 ms, 2-sample debounce, depth-4 queue). A press held over a reset is ignored until released, so the reboot can't loop. Range clamps (`rangeNext`/`rangePrev`) and saves to NVS 2 s after the last tap. Reset is checked in `wifiLoop` (so also during HTTP), the connect wait and the portal loop. **Extra check:** after hold-UP, the setup portal must work at 12 MHz pclk (deployment depends on it). |
| 4 Relative geometry | Done, verified on hardware (density 0.67 kept; flicker acceptable at 12 MHz with the thicker lines). After hardware review: W/E inset by `kCardinalSideInsetPx`, aircraft symbol 1.5× (nose 12 / tail 4.5 / half-width 6 design px), rim dots inset by their radius. | `ui/ui_scale.h` has `kUiDensity` (0.67) and `ui::px()`/`pxF()`; the plan's `ui()` was renamed `px()` so it reads well inside `namespace ui`. Every theme and status-screen size is scaled. Rings use `fillArc`. `segmentIntersectsDisc` is 64-bit. Projection and clipping moved to `ui/radar_geometry`, with clipping steps cut from 5% to 1% (5% left visible gaps at 720 px). The VLW size search upper bound was raised from 1.2 to `kMaxVlwTextSize` (8.0), because labels need ~2× the embedded font. There was no red/blue swap left in `initPalette()`. Bitmap-font fallbacks are not scaled (Phase 5). |
| 5 Fonts | Done, verified on hardware (2026-10-02): text sharp at all sizes, labels aligned, connecting screen good. The long-SSID `…` case wasn't exercised. Boot log reports line heights 33 / 66. | `scripts/build_vlw_font.py` renders `fonts/NotoSans-Bold.ttf` (FreeType light hinting) to `data/ui_font_small.vlw` (32 px: cap 23, line 33, 36 KB) and `data/ui_font_large.vlw` (64 px: cap 46, line 66, 132 KB), 97 glyphs incl. `°` and `…`. `displayFontApply(gfx, height_px)` targets **line height** (`fontHeight()`), not cap height, so the existing `k*HeightPx` layout constants keep their meaning. It tracks the loaded VLW per surface and reloads only on a switch. All radar labels (22–28 px) use the small font; status-screen text (30–37 px) uses the large one. Status-screen sizes became design-px line heights (old VLW scale × 16, the old font's line height). Removed: both `findVlwSizeForHeight` copies, `kMaxVlwTextSize`, the GFX candidate pickers, and `initLabelMetrics()` (it only computed values nothing read). `data/ui_font.vlw` deleted. |
| 6 Drawing code | **Complete.** Verified on hardware (2026-10-02) including Wi-Fi drop/reconnect. Frame timing: draw ~107 ms empty, +~4 ms per aircraft (~185 ms at 19); present ~100 ms (panel test measured ~75 ms). Internal heap steady at 141 KB free, 92 KB min, 59 KB largest block. The 2 s delayed NVS range save showed no display glitch. | Most of it landed earlier: status screens already draw into `canvas` with the spinner pushing only dot rects, the draw-to-panel fallback went in Phase 2 (`displayInit()` halts if the canvas can't be allocated), and `setTxPower` was removed in Phase 2. This phase removed `DrawScope`/`s_draw` (everything draws into `canvas`; `drawStaticGrid` is no longer a template), runs `initPalette()` once per frame, made `radarDisplayRefreshAircraft()` an alias of `radarDisplayDraw()`, and logs `Radar frame: draw N ms, present M ms, K aircraft`. CLAUDE.md display lines updated (stale `lgfx_config.hpp` / sprite-fallback text). Wi-Fi drop/reconnect verified: retries without opening the portal and recovers. It showed old aircraft as live after reconnect (failed fetches kept the list, and the frame only redrew on success), so: `adsb_client` now records a `FetchStatus` per fetch and the time of the last good one; after `kAdsbStaleAfterMs` (30 s) without a good fetch, or after Wi-Fi drops (`invalidate()`), the radar hides aircraft and shows the reason (No internet / not responding / rate limit / HTTP n / bad data / Waiting for data) between rings 1 and 2. Every fetch now redraws. adsb.fi errors are bare HTTP statuses (400 empty body, 429 Cloudflare HTML); success JSON carries `msg: "No error"`, and any other `msg` is shown. Connect failures at the 10 s deadline are now reported as such instead of -11. The rate-limit, server-error and bad-data messages can't easily be triggered, so they were only checked by reading the code. Phase marked complete 2026-10-02. |
| 7 Cleanup and docs | Docs done; **CI/release run pending** | README rewritten for the Qualia (hardware, UP/DOWN buttons, flashing with BOOT+RESET at 0x0 for ESP32-S3 16 MB, 5 s fetch, stale-data messages, `qualia_panel_test`, credit to the upstream C3 project). CLAUDE.md updated (hardware, envs, buttons, fetch status, colors, Windows pio note). The `parts` note was folded into the README Hardware table and deleted. The `.vscode` files are PlatformIO-generated and already use `qualia`; the workspace file has no env names. `release.yml`: a manual (`workflow_dispatch`) run now uploads the packaged files as an artifact instead of trying to publish, because `action-gh-release` needs a tag. Remaining: push the branch, enable Actions on the fork, then check Build (PR or manual run) and a manual Release run. `workflow_dispatch` only appears once the workflow file is on the default branch. |

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
