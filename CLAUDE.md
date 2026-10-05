# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Arduino/PlatformIO firmware for an Adafruit Qualia ESP32-S3 (N16R8: 16 MB flash, 8 MB octal PSRAM) driving a 4" round 720×720 RGB-666 panel (NV3052C, treated as Adafruit's HD40015C40). It was ported from an ESP32-C3 + 1.28" GC9A01 (240×240) build; `docs/qualia-port-plan.md` records that port, its decisions and the open issues. It shows a sonar-style radar of live ADS-B aircraft from `opendata.adsb.fi` around a configured lat/lon. Wi-Fi, location, units and the runway toggle are set through a WiFiManager captive portal (AP `PlaneRadar-Setup`, mDNS `plane-radar.local`), which also stays up on the LAN IP after the device connects.

## Commands

The firmware env is `qualia` (pinned pioarduino platform, Arduino core 3.x / ESP-IDF 5). `qualia_panel_test` extends it with `-DPANEL_TEST` and runs `hardware/panel_test` instead of the app (color bars, circles, text, looped HTTPS, button levels). The repo has no unit tests or linter. CI (`.github/workflows/build.yml`) only checks that the firmware builds.

```bash
pio run -e qualia                    # build; also writes .pio/build/qualia/firmware.factory.bin (flash at 0x0)
pio run -e qualia -t upload          # flash over USB-C
pio device monitor                   # serial log at 115200 baud
./scripts/merge-firmware.sh [--no-build]   # copies firmware.factory.bin to release/plane-radar-merged.bin
python3 scripts/build_large_airports.py    # regenerate the runway dataset from OurAirports
python3 scripts/build_vlw_font.py          # regenerate data/ui_font_*.vlw (needs: pip install freetype-py)
python3 scripts/build_tz_table.py          # regenerate the IANA -> POSIX time zone table (needs: pip install tzdata)
```

On Windows, run `pio` from PowerShell or cmd, not Git Bash: ESP-IDF's tool installer refuses MSys, so the toolchain never installs. If upload can't find the port, hold BOOT and tap RESET.

pioarduino builds the merged `firmware.factory.bin` itself (bootloader, partitions, boot_app0, the board's TinyUF2 image and the app). Pushing a `v*` tag runs `release.yml`, which attaches `plane-radar-<tag>.bin` and its `.sha256` to a GitHub Release.

## Architecture

`src/main.cpp` owns the control flow in `setup()`/`loop()`: it drains button taps (UP = zoom out, DOWN = zoom in), runs `wifiLoop()`, handles Wi-Fi drop and reconnect (a grace period, then rate-limited `wifiReconnect()`, which never reopens the portal), and fetches ADS-B every `config::kAdsbFetchIntervalMs` (5 s), redrawing after every fetch whether or not it succeeded. On a Wi-Fi drop it calls `services::adsb::invalidate()` so old aircraft don't come back as live. The other modules are passive and get called from there, except the button task.

- `include/config.h` holds all hardware pins, panel timings, timing and defaults in `namespace config` as `constexpr` values. adsb.fi's public limit is 1 req/s. `kPanelPclkHz` is 16 MHz, which needs the 36-line `kPanelBounceBufferPx`: with 10 lines Wi-Fi stopped working (see the plan's open issues).
- `services/wifi_setup` wraps WiFiManager, the portal's custom fields (lat/lon, miles, runways) and mDNS. It checks `buttonsResetRequested()` in `wifiLoop()`, the connect wait and the portal loop; holding UP 3 s clears credentials, location, units and clock settings, then reboots into the portal.
- `hardware/buttons` polls UP/DOWN on the PCA9554 expander (pins 5/6, active low; the expander INT isn't wired) from a FreeRTOS task on core 0 every 20 ms, with a 2-sample debounce and a depth-4 tap queue, so taps register during blocking HTTP or draw work. A press held through a reset is ignored until released. **GPIO0/BOOT is display line B4 and must not be used at runtime.**
- `services/adsb_client` does a blocking HTTPS GET. It reads chunked or sized bodies by hand, parses with ArduinoJson into a fixed `Aircraft[64]` array, and calls a `PollFn` hook (set to `wifiLoop`) during long I/O so the portal stays responsive. adsb.fi errors are bare HTTP statuses, so each fetch records a `FetchStatus` from the transport (plus the API's `msg` if it isn't "No error"). `aircraftFresh()` is false after `kAdsbStaleAfterMs` (30 s) without a good fetch.
- `services/radar_location` stores the radar center in NVS.
- `services/clock` starts SNTP once Wi-Fi is up and formats the time. By default the zone is automatic: when the radar location has no cached lookup, `loop()` asks Open-Meteo (`timezone=auto`) for the location's IANA zone (a blocking HTTPS request, retried every `kClockZoneRetryMs`) and maps it to a POSIX TZ string through `data/tz_posix` (**generated** by `scripts/build_tz_table.py` from the `tzdata` package; don't hand-edit). Until a lookup succeeds it uses the nautical offset for the longitude. The portal can also pick a fixed zone from a short list (NVS stores the POSIX string, not the index) and the 12/24-hour flag. The portal's time-zone `<select>` is custom HTML that writes into a hidden WiFiManager input, because WiFiManager only renders `<input>`s. `loop()` redraws when the minute changes so the clock doesn't wait for the next fetch.
- `ui/radar_range` holds the range presets (`kRangePresets`), the miles/runway flags, and `fetchRadiusKm()`. The fetch radius scales to the screen edge, so aircraft beyond the outer ring still arrive as rim dots.
- `ui/radar_display` renders every frame in full (grid, runways, labels, aircraft) into `canvas`, then calls `displayPresent()` once, which avoids flicker. When the data isn't fresh it draws the reason (`describeFetchProblem`) instead of aircraft. Aircraft are coloured by altitude (`kAltitudeColorStops`), and aircraft beyond the ring become rim arrows along their track (a dot without one). The outlined clock is drawn last, above everything.
- `ui/tag_layout` places aircraft tags. Each frame `radar_display` registers obstacles (icons and rim markers as circles, speed vectors as segments, the home marker, cardinal and scale labels and the clock as hard boxes, airport labels as soft boxes), then places tags nearest-first: eight slots around the icon, scored, with a bonus for last frame's slot (keyed by `hex`) so tags don't jump. Each range preset's `max_full_tags` caps the full three-line tags; the rest, and any full tag that doesn't fit, show the callsign only, and a tag with no free slot is hidden. Tag corners must stay inside `kTagScreenRadiusPx`.
- `ui/radar_shapes` holds the aircraft icons (picked from the ADS-B category, else the ICAO type), the rim arrow and the home marker as mirrored triangle lists in design px, rotated to the track. Each icon's covering radius is a `kIconRadius*` constant in `radar_theme.h`; keep the two in step. It logs draw and present times per frame (at 16 MHz about 140 ms + 5.5 ms per aircraft; present is the wait for the buffer swap).
- `ui/runway_overlay` draws runways from `data/large_airports` (coordinates stored as int32 `e7`). `src/data/large_airports_data.cpp` and `include/data/large_airports.h` are **generated** by `scripts/build_large_airports.py`. Don't hand-edit them.
- `ui/radar_theme.h` holds layout constants and colors. `ui/status_screens` draws the setup and connecting screens; the spinner redraws the whole screen each tick.
- `hardware/panel` is the only file that includes Arduino_GFX, which it uses for the PCA9554 expander and the NV3052C init sequence. The RGB output is its own `esp_lcd` panel with two PSRAM framebuffers and the bounce buffers. `panelSwap()` selects the drawn buffer with `esp_lcd_panel_draw_bitmap` (no copy) and waits on the `on_frame_buf_complete` callback until the old one is off screen. It resets LCD_CAM before init so a warm reboot doesn't shift the image. A flash write can stall the bounce-buffer refill (its ISR isn't IRAM-safe) and shift the image, so `setup()` calls `displayResync()` (`esp_lcd_rgb_panel_restart()`) when Wi-Fi gets an IP. `hardware/display` owns `canvas`, a 720×720 `LGFX_Sprite` (`rgb565_nonswapped`) pointed at the back framebuffer with `setBuffer`. `displayPresent()` swaps and re-points it. The back buffer holds the frame before last, so **every screen must be drawn in full before `displayPresent()`**; there are no partial updates. All drawing goes through LovyanGFX into `canvas`. If the panel or its framebuffers can't be set up, boot stops with an error. `hardware/display_font` holds the anti-aliased VLW fonts that `board_build.embed_files` embeds (`data/ui_font_tag.vlw` / `ui_font_small.vlw` / `ui_font_large.vlw`). `displayFontApply(gfx, height_px)` draws a file unscaled when its line height is `height_px` or 1 px under (the tag file is fitted to `kAircraftTagLabelHeightPx`, so tags aren't scaled; scaling down drops glyph pixels), else picks the smallest file at least that tall and scales it down. `FreeSansBold24pt7b` is the only bitmap fallback. The VLW files are **generated** by `scripts/build_vlw_font.py` (freetype-py) from `fonts/NotoSans-Bold.ttf`.

Lat/lon is projected to screen space with an equirectangular approximation: `dx = Δlon·kKmPerDeg·cos(center_lat)`, `dy = Δlat·kKmPerDeg`. It lives in `ui/radar_geometry`, which both `radar_display.cpp` and `runway_overlay.cpp` use.

Element sizes are written in pixels of the original 240 px design and scaled with `ui::px()` / `ui::pxF()` from `ui/ui_scale.h`. `kUiDensity` sets the overall size. Don't add raw pixel constants.

### Persistence (NVS via `Preferences`)

Preferences live in separate namespaces: `planeradar` (range preset, miles, runways), `wifi` (the force-portal flag), `clock` (time zone or "auto", 24-hour, last automatic lookup), and a location namespace. They're kept apart on purpose to avoid NVS handle conflicts, so give new settings their own namespace or add them to the right existing one.

### Flash layout

`partitions/plane_radar.csv` (16 MB) has two 4 MB OTA app slots on either side of the board's TinyUF2 factory slot at 0x410000. The board config always flashes TinyUF2 there, so don't move that slot.

## Conventions

- C++17 (`-std=gnu++17`). Code is split into `include/<area>/*.h` and `src/<area>/*.cpp` under namespaces `services::*`, `ui`, and `ui::radar` / `ui::runway`. File-local state uses anonymous namespaces with an `s_` prefix, and constants use a `k` prefix.
- Colors are standard RGB565 (`canvas.color565()`); `canvas` stores them in native byte order, which the panel scans out directly. Set its color depth only before `setBuffer` (see `display.cpp`). There's no color inversion or BGR swap on this panel.
- Keep Arduino_GFX includes inside `hardware/panel.cpp`. Everything else draws with LovyanGFX into `canvas`.
