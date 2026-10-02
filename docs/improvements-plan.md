# Improvements plan (after the Qualia port)

Status: **roadmap step 1 (faster display output) done 2026-10-02; the rest not started.** This plan collects what the
upstream forks have built, picks what's worth bringing over to the 720×720 Qualia build, and
includes the auto-brightness plan.

## Fork survey (2026-10-02)

Upstream: [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar) (MIT). GitHub
listed 295 forks; 250 could be compared against upstream `main`, and 131 of those have their own
commits. The survey read the commit history of the ~60 forks that are 6+ commits ahead, plus the
READMEs and key source of the most relevant ones. Screenshots weren't checked, so visual judgments
come from descriptions and code.

Most C3 fork work is about running out of memory (stream-parsing JSON, skipping fetches when the
heap is low, dropping trails to free RAM for TLS). That doesn't apply on the S3 with 8 MB PSRAM.
Forks that move to other boards (CYD, Nextion, ILI9488, Waveshare) add little here.

Upstream and the forks checked are MIT licensed, so porting code is fine with credit.

### pvanbaren: the same Qualia board

[pvanbaren/ESP32-Plane-Radar](https://github.com/pvanbaren/ESP32-Plane-Radar) runs on the same
Adafruit Qualia ESP32-S3 + 4" 720×720 NV3052C panel, on pioarduino and core 3.x, with the UP/DN
buttons. See its `docs/qualia_display.md`, `src/hardware/qualia_rgb.cpp` and
`src/hardware/display.cpp`.

- **16 MHz pclk with Wi-Fi working. Done here (2026-10-02):** their 36-line bounce buffer
  (`bounce_buffer_size_px = 720 * 36`) alone fixed it on our Arduino_GFX path. See the port
  plan's open issues for measurements. Their other panel settings, compared with ours:
  - `num_fbs = 2`: needed for the no-copy present below. Arduino_GFX hard-codes 1.
  - `dma_burst_size = 64`: **already the same.** It shares a union with `psram_trans_align`,
    which Arduino_GFX sets to 64.
  - `pclk_active_neg = 0` (ours is 1): not needed for 16 MHz. Only worth trying if fine detail
    shimmers.
  - `periph_module_reset(PERIPH_LCD_CAM_MODULE)` before init, and `esp_lcd_rgb_panel_restart()`
    after, so a warm reboot doesn't shift the image vertically. Done with the no-copy present.
  - `WiFi.setSleep(WIFI_PS_NONE)`: we already do this. `WiFi.setTxPower(WIFI_POWER_8_5dBm)` isn't
    theirs: it's an upstream ESP32-C3 fix (WatskeBart, `2e2808e`) merged into their fork, and it
    crippled our setup AP in Phase 2. Don't bring it over.
- **No-copy present. Done here (2026-10-02):** LovyanGFX draws straight into the panel's back
  framebuffer (`setBuffer`, color depth `rgb565_nonswapped`), and presenting calls
  `esp_lcd_panel_draw_bitmap` with the driver's own buffer, which switches buffers instead of
  copying. Unlike theirs, `panelSwap()` then waits on the `on_frame_buf_complete` callback until
  the old buffer is off screen, so the next frame never draws into the visible one. Present went
  from ~110 ms to 0–40 ms (the rest of the current refresh). See `hardware/panel.cpp`.
- **Smooth motion.** Between fetches each aircraft is dead-reckoned along `track_deg` at
  `gs_knots`, seeded with the ADS-B `seen_pos` age, and the radar redraws at 4 Hz
  (`kRadarRedrawIntervalMs = 250`). The fetch runs on a background FreeRTOS task. Commits
  `690e466`, `4dabcf3`, `9a3e1dd`.
- **Water outlines** from USGS NHD, baked for one region (US only): `scripts/build_water_bodies.py`,
  `src/ui/water_overlay.cpp`.
- Medium airports globally, small airports near home, with the list pre-filtered to the area
  around the radar center (`d5404d2`).
- Optional clock at top or bottom (NTP), and a DN-tap status screen (RSSI, IP, heap, temperature,
  uptime).

### Other visual features

| Feature | Forks | Fits 720 px? |
|---|---|---|
| Color by altitude (warm low, cool high) | selmapi, tbirdrv | Yes. A color lookup. |
| Fading trails behind aircraft | selmapi (64×8 ring buffer), GCamilleri, RubenKremer, JoaoCostaIFG, blinkidy | Yes. Cheap line draws; RAM isn't an issue on the S3. |
| Themes (green CRT, amber, submarine red, navy "CIC" scope with bearing ring and ticks) | selmapi | Yes. Colors already live in `radar_theme.h`. The ring and ticks suit 720 px better than 240. |
| Label decluttering (move tags to open space, rank-limit tags per range, rotate details when crowded) | blinkidy (most complete), benyaffe, GCamilleri/stewartallen, aroyer-qc | Yes, and it's needed: tag overlap is an open item in the port plan. |
| Plane icons by type (jet, light plane, spinning helicopter, balloon) | timclarke07, vfranchi | Partly. They're 10–16 px bitmaps; redo as polygons through `ui::px()` or regenerate larger. |
| Highlights: military, emergency squawk (7500/7600/7700), favorites | GCamilleri, giplgwm, dreamiurg, mlciskey, oldjiberjaber, benyaffe | Yes. Small change. |
| Climb/descend arrow, flight levels | cuotos, JoaoCostaIFG, blinkidy | Yes. |
| Rotating sweep line | GCamilleri, smgam29, daredevilbear, RockBase-iot | Not until drawing is faster. At ~3.5 fps it stutters. |
| Land, terrain and coastline fill | benyaffe (filled land tiles), bartdelange (shaded terrain) | Easier than on the C3: draw the background once into a PSRAM sprite and copy it each frame. Big project. |
| Route labels (origin → destination, airline) | blinkidy, devnulluk, Niko12345678, bartdelange, JoaoCostaIFG | Yes; room for another tag line. Needs extra adsbdb lookups. |
| Extra screens (cockpit clock with wind and pressure, airport weather map) | benyaffe | Yes, but a lot of work. |
| Day/night brightness | fmurodov (sun position from NTP), oldjiberjaber, JoaoCostaIFG, aroyer-qc | See "Auto-brightness" below. |

## Current frame budget

With the no-copy present at 16 MHz (2026-10-02): draw ~230–250 ms with 16–19 aircraft, present
0–40 ms. That's ~250–280 ms per frame, ~3.5–4 fps. Drawing didn't get faster: the framebuffers
are in PSRAM like the old canvas was.

| | Draw, ~20 aircraft | Present | Frame |
|---|---|---|---|
| 12 MHz, copy (Phase 6) | ~185 ms | ~100 ms | ~285 ms |
| 16 MHz, copy | ~245 ms | ~110 ms | ~355 ms |
| 16 MHz, no-copy | ~240 ms | 0–40 ms | ~260 ms |

Smooth motion at 4 Hz needs under 250 ms, and a sweep needs more. That's close now. pvanbaren's
loop redraws as fast as it can up to 4 Hz, which would give ~3.5 Hz here. If that looks steppy,
drawing has to get cheaper; profile which parts cost most (anti-aliased labels read back the
pixels they blend onto) before caching anything.

## Roadmap

1. **Faster display output** (pvanbaren). **Done 2026-10-02, verified on hardware:** 16 MHz
   with 36-line bounce buffers, the panel owned through `esp_lcd` with two framebuffers and a
   no-copy swap, LCD_CAM reset, and a scan-out resync on Wi-Fi connect. Checked: present time,
   colors, spinner, Wi-Fi and setup AP, warm reboot. Not adopted: `pclk_active_neg = 0` (only
   if fine detail shimmers) and `setTxPower`.
2. **Smooth motion** (pvanbaren). Move the ADS-B fetch to a background task (their fetch task
   fills the aircraft list under a mutex; drawing takes a snapshot per frame), dead-reckon between
   fetches with `seen_pos`, and redraw every 250 ms instead of after each fetch. Settle first:
   `adsb_client`'s `PollFn` (portal servicing during a fetch), Wi-Fi drop handling and
   `invalidate()`, and applying a range change on the next frame.
3. **Label decluttering** (blinkidy). Closes the tag-overlap item in the port plan.
4. **Color by altitude and fading trails** (selmapi). Cheap and noticeable.
5. **Auto-brightness** (below). Independent of 1–4; can be done any time.
6. **Optional later:** emergency/military highlights, climb arrows, icons by type, themes, sweep
   (after 1), water or land background, route labels, clock.

## Auto-brightness

This dims the backlight with hardware PWM and sets the level from an ambient-light photoresistor
(LDR). Hardware: Adafruit Qualia ESP32-S3 RGB666
([5800](https://www.adafruit.com/product/5800)) + 4" 720×720 round panel
([5793](https://www.adafruit.com/product/5793)).

### Backlight PWM

The backlight is switched on/off through the PCA9554 expander, pin 4
(`config::kExpanderPinBacklight`, `panelBacklight()` in `src/hardware/panel.cpp`). The expander has
no PWM, and bit-banging PWM over I2C would flicker and compete with the button-poll task for the
bus.

The Qualia has a built-in option
([pinouts guide](https://learn.adafruit.com/adafruit-qualia-esp32-s3-for-rgb666-displays/pinouts)):

> Soldering the bottom PWM jumper allows using Pin `A1` to control the backlight.

- `A1` = **GPIO16** (from the pioarduino variant `adafruit_qualia_s3_rgb666/pins_arduino.h`). It's
  also the default `TX1`, which the project doesn't use.
- Backlight current defaults to 25 mA. Bridging the top jumpers raises it to as much as 200 mA.
  Check the panel's spec sheet before raising it.

**Open question: does the jumper conflict with expander pin 4?** Check the Qualia schematic
(Adafruit PCB repo on GitHub) for how the backlight driver's EN/PWM pin is wired:

- **Gated** (A1 in series/AND with the expander signal): keep expander pin 4 HIGH and let A1 dim.
- **Shared net** (the jumper ties A1 to the same line): set expander pin 4 to INPUT (high-Z) so it
  doesn't fight the PWM.

Until that's confirmed, leave the expander pin HIGH and test whether the PWM dims the screen.

```cpp
// config.h
constexpr int kPinBacklightPwm = 16;          // A1, needs bottom PWM jumper bridged
constexpr uint32_t kBacklightPwmHz = 20000;   // silent, no camera flicker
constexpr uint8_t kBacklightPwmBits = 8;

// panel.cpp (Arduino core 3.x LEDC API)
ledcAttach(config::kPinBacklightPwm, config::kBacklightPwmHz, config::kBacklightPwmBits);
ledcWrite(config::kPinBacklightPwm, duty);  // 0..255
```

- Start at 20 kHz. If the low end looks steppy or cuts out, drop to about 5 kHz.
- Backlight drivers usually turn off below some minimum duty, so find that point and clamp to it.

### Ambient light sensor: photoresistor

An LDR is fine. Calibrate by eye ("dark room" and "bright room" readings); exact lux doesn't
matter. Its slow, roughly logarithmic response suits brightness control.

The pin must be on ADC1: ADC2 can't be read reliably while Wi-Fi is running, and Wi-Fi is always
on. The free SPI header pins are on ADC1, and the RGB bus doesn't use them:

| Header pin | GPIO | ADC | Usable with Wi-Fi? |
|---|---|---|---|
| SCK | 5 | ADC1_CH4 | yes |
| **MISO** | **6** | **ADC1_CH5** | **yes (chosen)** |
| MOSI | 7 | ADC1_CH6 | yes |
| A0 | 17 | ADC2 | no |
| A1 | 16 | — | used for backlight PWM |

(The RGB bus uses GPIO 0–3, 9–14, 21, 38–42 and 45–48. See `config.h`.)

```
3V3 ── LDR ──┬── GPIO6 (MISO)
             │
            10k
             │
GND ─────────┘
```

- Brighter light means lower LDR resistance, so the voltage rises.
- Pick the fixed resistor close to the LDR's resistance in a **dim** room, where dimming matters
  most. 10k is a reasonable start; measure one LDR if they're unlabeled.
- Optional: 0.1 µF from GPIO6 to GND for noise.
- Point the LDR at the room, away from the screen's own glow (for example, behind a small hole in
  the bezel). If it sees the screen, brightness feeds back and oscillates.

Firmware:

- `analogReadMilliVolts(6)` returns calibrated mV (11 dB attenuation by default, about 0–3.1 V).
- Average about 16 samples per reading, read every 0.5–1 s, and smooth with an EMA.
- Map mV to duty with a log curve or a small lookup table, clamped to `[min duty, 255]`.
- Add hysteresis so the level doesn't flip back and forth near a boundary.
- Put the calibration points (`kLdrDarkMv`, `kLdrBrightMv`) and min/max duty in `config.h` first.
  If they move to the portal or NVS later, give them their own namespace or add them to
  `planeradar` (see "Persistence" in CLAUDE.md).

### Structure

- `hardware/backlight`: LEDC setup, `backlightSet(uint8_t duty)`, and the expander pin handling
  from the open question.
- `services/ambient_light`: LDR sampling, smoothing, and mapping to duty.
- Call it from `loop()` on a timer, like the other passive modules, or from the button-poll task.

### Alternatives

- **No sensor:** set day/night brightness from the sun's position at the radar location, using
  NTP time ([fmurodov](https://github.com/fmurodov/ESP32-Plane-Radar), "Add auto day/night
  brightness from NTP sun position"). Still needs the PWM jumper. It ignores lamps and curtains,
  but it's a fallback if the LDR is unplugged.
- **No soldering:** scale the theme colors (or the frame) by a brightness factor. Works with no
  hardware changes, but the backlight stays at full power and blacks stay a little lifted.

### Checklist

1. [ ] Read the Qualia schematic and resolve the expander-pin-4 question.
2. [ ] Bridge the bottom PWM jumper.
3. [ ] Test PWM: sweep duty and find the minimum usable duty and a good frequency.
4. [ ] Wire the LDR and 10k divider to GPIO6, then log mV readings in dark and bright rooms.
5. [ ] Implement `hardware/backlight` and `services/ambient_light`, then tune the curve and
       hysteresis.
