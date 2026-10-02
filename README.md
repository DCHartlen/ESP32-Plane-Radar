# Plane Radar

Firmware for an **Adafruit Qualia ESP32-S3 for TTL RGB-666** driving a **4″ round 720×720 IPS display** (NV3052C). Shows a circular **ADS-B radar** around your configured location, with **WiFiManager** for first-time setup.

Based on [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar), which targets an ESP32-C3 Super Mini with a 1.28″ GC9A01 display (and has a [3D printed case](https://makerworld.com/en/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083) for that build). This fork replaces that hardware entirely. **Firmware:** [Releases](https://github.com/DCHartlen/ESP32-Plane-Radar/releases)

<img width="800" height="450" alt="plane-radar (original 1.28″ build)" src="https://github.com/user-attachments/assets/716d0992-dab8-47ba-8f1a-2aec7f607419" />

## Hardware

| Part | Notes |
|------|-------|
| [Adafruit Qualia ESP32-S3 for TTL RGB-666 displays](https://www.adafruit.com/search?q=qualia) | ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM |
| 4″ round 720×720 IPS panel, 40-pin RGB, NV3052C driver, no touch | Treated as Adafruit's **HD40015C40** (same init sequence and timings) |

The panel plugs straight into the Qualia's 40-pin connector, so there's no wiring. Power and flashing are over the Qualia's USB-C.

- **Keep metal away from the antenna end** of the board (including metallized anti-static bags and enclosure parts). It made the setup access point unusable during testing.
- The backlight is on/off only. Dimming needs the Qualia's PWM jumper soldered (pin A1 / GPIO16) and isn't implemented.
- GPIO0 (BOOT) is a display data line on this board, so it isn't used as a button.

## What it does

1. **Wi‑Fi setup** (if needed): captive portal on AP **`PlaneRadar-Setup`**
2. **Radar**: live aircraft from [adsb.fi](https://opendata.adsb.fi/) on a sonar-style grid, refreshed every 5 s

After Wi‑Fi is saved, the device reconnects automatically. If Wi‑Fi drops, it shows a "Connecting" screen and keeps retrying. It never reopens the setup portal on its own.

## Controls (Qualia UP / DOWN buttons)

| Action | Effect |
|--------|--------|
| **UP** tap | Zoom out to the next range preset (stops at 25 km) |
| **DOWN** tap | Zoom in to the previous range preset (stops at 5 km) |
| **Hold UP 3 s** | Clear Wi‑Fi, location and units; reboot into the setup portal |

Taps register even while a fetch or redraw is in progress. The range is saved to flash 2 s after the last tap. Hold UP works on every screen, including setup and connecting.

## Wi‑Fi setup portal

**First-time setup** (no saved Wi‑Fi):

1. Connect to **`PlaneRadar-Setup`**
2. Open **`http://plane-radar.local`** (preferred) or **`http://192.168.4.1`**. Both are shown on the yellow setup screen, and the captive portal may open automatically.
3. Set home Wi‑Fi and your location, then save

**Reconfigure anytime** (after the device is on your network):

1. Open **`http://plane-radar.local`** or **`http://<device-ip>`** (from your router, or the serial log at boot)
2. Change Wi‑Fi, location, units, or runway overlay; save

The same portal runs on the setup AP and on the device's LAN IP while connected to Wi‑Fi. The mDNS hostname is `plane-radar` → **plane-radar.local** (`kPortalHostname` in `config.h`). Some clients resolve `.local` slowly, so use the IP if needed.

**Custom fields** (stored in NVS):

| Field | Purpose |
|-------|---------|
| **Latitude / Longitude** | Radar center and ADS-B query position (defaults in `config.h` until set) |
| **Display distances in miles** | Ring scale label in **mi** instead of **km** (e.g. `6mi` vs `10km`) |
| **Show airport runways** | Major-airport runway overlay on the radar (off to hide) |

A reset clears the location too, and the portal then pre-fills the default (Amsterdam), so re-enter yours.

## Radar display

### Grid

- Dark blue background, subdued green rings and crosshairs
- White **N / S / E / W** at the bezel; range label on the **east** spoke (ring 3 = ¾ of outer radius)
- White center dot

Layout and colors: `include/ui/radar_theme.h`. Element sizes are written in pixels of the original 240 px design and scaled to the 720 px screen; `kUiDensity` in `include/ui/ui_scale.h` sets the overall size (0.67 ≈ twice the original's physical size).

### Range presets

| Ring 3 label | Outer radius (aircraft scale) |
|------------|-------------------------------|
| 5 km / 3 mi | ~6.7 km |
| 10 km / 6 mi | ~13.3 km (default) |
| 15 km / 9 mi | ~20 km |
| 25 km / 16 mi | ~33.3 km |

Preset and miles/km choice persist across reboot (`planeradar` NVS namespace).

### Runways

- Major airports from OurAirports (`large_airport`); all open runway strips in range (helipads excluded)
- Teal runway lines with one ICAO label per airport (e.g. `KJFK`); toggle in the Wi‑Fi setup portal
- Update the embedded list: `python3 scripts/build_large_airports.py`

### Aircraft

- **Inside the outer ring**: red heading triangle, magenta track line showing where the aircraft will be in 30 s (clipped at the ring), callsign / type / altitude tags
- **Outside the ring** (still within the ADS-B fetch): small **red dot on the screen rim** at the correct bearing (a direction cue, not distance-accurate past the ring)
- **Tags** are placed toward the **center**: west (left) → tag on the **right** of the symbol; east (right) → tag on the **left**

As range decreases (or aircraft approach), targets move inward; rim dots become full symbols when they cross the outer ring.

### When data is missing

If there's been no successful fetch for 30 s (`kAdsbStaleAfterMs`), or Wi‑Fi has just reconnected, aircraft are hidden so old positions aren't shown as live. A message between the first and second rings says why:

| Message | Meaning |
|---------|---------|
| Waiting for data | Just booted or reconnected |
| No internet | Couldn't connect to adsb.fi (no internet, DNS or TLS failure) |
| adsb.fi not responding | Connected, but no reply within 10 s |
| Connection lost | The connection dropped mid-request |
| adsb.fi rate limit | HTTP 429 |
| adsb.fi unavailable | HTTP 5xx |
| adsb.fi error | Any other HTTP status |
| Bad data from adsb.fi | Empty or invalid JSON, or an API error message |
| Data out of date | The last fetch succeeded, but more than 30 s ago |

A single failed fetch doesn't trigger a message.

### ADS-B

- Source: `https://opendata.adsb.fi/api/v3/` (public limit: 1 request/s)
- Fetch radius: `ui::radar::fetchRadiusKm()` scales with the active preset to roughly the screen edge (so rim dots have data)
- Poll interval: `kAdsbFetchIntervalMs` (5 s) in `config.h`
- Ground aircraft hidden by default (`kAdsbShowGroundAircraft`)

## Configuration

Edit **`include/config.h`** for hardware and behavior:

| Area | Keys / notes |
|------|----------------|
| Portal | `kPortalApName`, `kPortalIp`, `kPortalHostname` / `kPortalHostUrl` (mDNS; needs `-DWM_MDNS` in `platformio.ini`) |
| Wi‑Fi timing | Connect attempts, reconnect grace and interval, portal timeout (`0` = no timeout) |
| Buttons | `kResetHoldMs`, `kButtonPollMs`, `kRangeSaveDelayMs`, expander pins, `kButtonActiveLow` |
| Display | RGB pins, panel timings, `kPanelPclkHz` (12 MHz; higher breaks Wi‑Fi, see `docs/qualia-port-plan.md`), `kPanelBounceBufferPx`, `kDisplayRotate180` |
| Default location | `kDefaultRadarLat`, `kDefaultRadarLon` (until the portal overrides them) |
| ADS-B | `kAdsbFetchIntervalMs`, `kAdsbStaleAfterMs`, `kAdsbShowGroundAircraft` |

Range presets: `include/ui/radar_range.h` (`kRangePresets`).

## Project layout

```
include/
  config.h
  hardware/
    panel.h               — Arduino_GFX RGB panel + PCA9554 expander (only user of Arduino_GFX)
    display.h             — 720×720 PSRAM canvas (LovyanGFX), present to the panel
    display_font.h
    buttons.h             — UP/DOWN polling task
    panel_test.h          — hardware bring-up test (qualia_panel_test env)
  data/
    large_airports.h
  ui/
    ui_scale.h
    radar_theme.h
    radar_geometry.h
    radar_range.h
    radar_display.h
    runway_overlay.h
    status_screens.h
  services/
    wifi_setup.h
    radar_location.h
    adsb_client.h
data/
  ui_font_small.vlw       — embedded smooth UI fonts (generated)
  ui_font_large.vlw
fonts/
  NotoSans-Bold.ttf       — source font for the VLW files (OFL.txt)
partitions/
  plane_radar.csv         — 16 MB: two OTA slots around the TinyUF2 slot
scripts/
  build_large_airports.py
  build_vlw_font.py
  merge-firmware.sh
src/
  main.cpp
  data/
    large_airports_data.cpp
  hardware/
  ui/
  services/
```

## Build

```bash
pio run -e qualia -t upload
pio device monitor
```

- PlatformIO env: **`qualia`**, on the pinned [pioarduino](https://github.com/pioarduino/platform-espressif32) platform (Arduino core 3.x / ESP-IDF 5)
- Serial: **115200** baud (USB CDC; the boot waits up to 3 s for a monitor)
- If upload can't find the port, put the board in download mode: hold **BOOT**, tap **RESET**, release BOOT
- On Windows, run `pio` from PowerShell or cmd, not Git Bash (ESP-IDF's tool installer refuses MSys)

`qualia_panel_test` is a separate env for hardware bring-up: color bars, circles, text, looped HTTPS and button levels (`pio run -e qualia_panel_test -t upload`).

### Web-flashable release image

Every build writes a single image, `.pio/build/qualia/firmware.factory.bin` (bootloader, partitions, boot_app0, the board's TinyUF2 and the app), for [esptool-js](https://espressif.github.io/esptool-js/) and similar tools. To copy it to `release/`:

```bash
./scripts/merge-firmware.sh             # builds, then writes release/plane-radar-merged.bin
./scripts/merge-firmware.sh --no-build  # copy only
```

Flash it at **0x0** (ESP32-S3, 16 MB). Put the board in download mode (hold **BOOT**, tap **RESET**), then flash with Chrome/Edge over USB.

### CI and releases (GitHub Actions)

| Workflow | When | Output |
|----------|------|--------|
| [Build](.github/workflows/build.yml) | Push to `main`, any PR, or manual run | Artifact `plane-radar-qualia` (factory + split `.bin` files, ~90 days) |
| [Release](.github/workflows/release.yml) | Git tag `v*` (e.g. `v1.0.0`) | GitHub Release asset `plane-radar-v1.0.0.bin` + `.sha256` |
| [Release](.github/workflows/release.yml) | Manual run (test) | Artifact `plane-radar-manual-<sha>` only; nothing is published |

To ship a version users can download:

```bash
git tag v1.0.0
git push origin v1.0.0
```

The release workflow builds the firmware in CI and attaches the factory image to the release. Download it from **Releases** on GitHub, then flash at **0x0** (ESP32-S3, 16 MB).

## Dependencies

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX): all drawing
- [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX): panel init and framebuffer
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
