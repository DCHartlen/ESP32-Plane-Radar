# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Arduino/PlatformIO firmware for an ESP32-C3 Super Mini driving a 1.28" round GC9A01 (240×240) display. It shows a sonar-style radar of live ADS-B aircraft from `opendata.adsb.fi` around a configured lat/lon. Wi-Fi, location, units and the runway toggle are set through a WiFiManager captive portal (AP `PlaneRadar-Setup`, mDNS `plane-radar.local`), which also stays up on the LAN IP after the device connects.

## Commands

There is one PlatformIO environment, `qualia` (pioarduino platform, Arduino core 3.x). A port from the ESP32-C3/GC9A01 build is in progress: see `docs/qualia-port-plan.md` for the phased plan and progress. The repo has no unit tests or linter. CI (`.github/workflows/build.yml`) only checks that the firmware builds.

```bash
pio run -e qualia                    # build; also writes .pio/build/qualia/firmware.factory.bin (flash at 0x0)
pio run -e qualia -t upload          # flash over USB-C
pio device monitor                   # serial log at 115200 baud
./scripts/merge-firmware.sh [--no-build]   # copies firmware.factory.bin to release/plane-radar-merged.bin
python3 scripts/build_large_airports.py    # regenerate the runway dataset from OurAirports
```

pioarduino builds the merged `firmware.factory.bin` itself (bootloader, partitions, boot_app0, the board's TinyUF2 image and the app). Pushing a `v*` tag runs `release.yml`, which attaches `plane-radar-<tag>.bin` and its `.sha256` to a GitHub Release.

## Architecture

`src/main.cpp` owns the control flow in `setup()`/`loop()`: it polls the BOOT button, runs `wifiLoop()`, handles Wi-Fi drop and reconnect (a grace period, then rate-limited `wifiReconnect()`, which never reopens the portal), and fetches ADS-B every `config::kAdsbFetchIntervalMs`. The other modules are passive and get called from there.

- `include/config.h` holds all hardware pins, timing and defaults in `namespace config` as `constexpr` values. The README says the fetch interval is 5 s, but the code uses 3000 ms. adsb.fi's public limit is 1 req/s.
- `services/wifi_setup` wraps WiFiManager, the portal's custom fields (lat/lon, miles, runways), mDNS, and the BOOT button. The button uses an interrupt that latches taps, so a tap still registers during blocking HTTP or draw work. A short tap cycles the range. Holding 3 s clears credentials, location and units, then reboots into the portal.
- `services/adsb_client` does a blocking HTTPS GET. It reads chunked or sized bodies by hand, parses with ArduinoJson into a fixed `Aircraft[64]` array, and calls a `PollFn` hook (set to `wifiLoop`) during long I/O so the portal stays responsive.
- `services/radar_location` stores the radar center in NVS.
- `ui/radar_range` holds the range presets (`kRangePresets`), the miles/runway flags, and `fetchRadiusKm()`. The fetch radius scales to the screen edge, so aircraft beyond the outer ring still arrive as rim dots.
- `ui/radar_display` renders every frame (grid, runways, labels, aircraft) into one full-screen 16-bit `LGFX_Sprite` and pushes it in a single `pushSprite`, which avoids flicker. If the sprite can't be allocated, it draws straight to `tft`. Drawing helpers target whatever `s_draw` points to, and a `DrawScope` switches it.
- `ui/runway_overlay` draws runways from `data/large_airports` (coordinates stored as int32 `e7`). `src/data/large_airports_data.cpp` and `include/data/large_airports.h` are **generated** by `scripts/build_large_airports.py`. Don't hand-edit them.
- `ui/radar_theme.h` holds layout constants and colors. `ui/status_screens` draws the setup and connecting screens.
- `hardware/lgfx_config.hpp` sets up the LovyanGFX panel and bus from `config.h`. `hardware/display_font` loads the anti-aliased VLW font that `board_build.embed_files` embeds (`data/ui_font.vlw`), with GFX fonts as a fallback.

Lat/lon is projected to screen space with an equirectangular approximation: `dx = Δlon·kKmPerDeg·cos(center_lat)`, `dy = Δlat·kKmPerDeg`. It appears in both `radar_display.cpp` and `runway_overlay.cpp`, so keep the two in sync.

### Persistence (NVS via `Preferences`)

Preferences live in separate namespaces: `planeradar` (range preset, miles, runways), `wifi` (the force-portal flag), and a location namespace. They're kept apart on purpose to avoid NVS handle conflicts, so give new settings their own namespace or add them to the right existing one.

### Flash layout

`partitions/plane_radar.csv` (16 MB) has two 4 MB OTA app slots on either side of the board's TinyUF2 factory slot at 0x410000. The board config always flashes TinyUF2 there, so don't move that slot.

## Conventions

- C++17 (`-std=gnu++17`). Code is split into `include/<area>/*.h` and `src/<area>/*.cpp` under namespaces `services::*`, `ui`, and `ui::radar` / `ui::runway`. File-local state uses anonymous namespaces with an `s_` prefix, and constants use a `k` prefix.
- Colors are RGB565. The panel needs `kDisplayInvert = true` and BGR order.
