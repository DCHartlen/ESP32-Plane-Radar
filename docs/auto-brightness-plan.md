# Auto-brightness plan

Status: **idea / not started.** This plan dims the backlight with hardware PWM and sets the level from an ambient-light photoresistor (LDR).

Hardware: Adafruit Qualia ESP32-S3 RGB666 ([5800](https://www.adafruit.com/product/5800)) + 4" 720x720 round panel ([5793](https://www.adafruit.com/product/5793)).

## Backlight PWM

The backlight is currently switched on/off through the PCA9554 expander, pin 4 (`config::kExpanderPinBacklight`, `panelBacklight()` in `src/hardware/panel.cpp`). The expander has no PWM, and bit-banging PWM over I2C would flicker and compete with the button-poll task for the bus.

The Qualia has a built-in option for this ([pinouts guide](https://learn.adafruit.com/adafruit-qualia-esp32-s3-for-rgb666-displays/pinouts)):

> Soldering the bottom PWM jumper allows using Pin `A1` to control the backlight.

- `A1` = **GPIO16** (from the pioarduino variant `adafruit_qualia_s3_rgb666/pins_arduino.h`). It's also the default `TX1`, which the project doesn't use.
- Backlight current defaults to 25 mA. Bridging the top jumpers raises it to as much as 200 mA. Check the panel's spec sheet before you raise it.

### Open question: does the jumper conflict with expander pin 4?

Check the Qualia schematic (Adafruit PCB repo on GitHub) to see how the backlight driver's EN/PWM pin is wired:

- **Gated** (A1 in series/AND with the expander signal): keep expander pin 4 HIGH, and let A1 do the dimming.
- **Shared net** (the jumper ties A1 to the same line): set expander pin 4 to INPUT (high-Z) so it doesn't fight the PWM.

Until that's confirmed, leave the expander pin HIGH and test whether the PWM dims the screen.

### Firmware

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

## Ambient light sensor: photoresistor

An LDR is fine for this. Calibrate by eye ("dark room" and "bright room" readings), because exact lux values don't matter. Its slow, roughly logarithmic response suits brightness control.

### Pin choice: must be ADC1

ADC2 can't be read reliably while Wi-Fi is running, and Wi-Fi is always on. The free SPI header pins are on ADC1, and the RGB bus doesn't use them:

| Header pin | GPIO | ADC | Usable with Wi-Fi? |
|---|---|---|---|
| SCK | 5 | ADC1_CH4 | yes |
| **MISO** | **6** | **ADC1_CH5** | **yes (chosen)** |
| MOSI | 7 | ADC1_CH6 | yes |
| A0 | 17 | ADC2 | no |
| A1 | 16 | — | used for backlight PWM |

(As of this writing, the RGB bus uses GPIO 0–3, 9–14, 21, 38–42 and 45–48. See `config.h`.)

### Wiring

```
3V3 ── LDR ──┬── GPIO6 (MISO)
             │
            10k
             │
GND ─────────┘
```

- Brighter light means lower LDR resistance, so the voltage rises.
- Pick the fixed resistor close to the LDR's resistance in a **dim** room, where dimming matters most. 10k is a reasonable start; measure one LDR if they're unlabeled.
- Optional: a 0.1 µF capacitor from GPIO6 to GND for noise.

### Placement

Point the LDR at the room, away from the screen's own glow (for example, behind a small hole in the bezel). If it sees the screen, brightness feeds back and oscillates.

### Firmware

- `analogReadMilliVolts(6)` returns calibrated mV (11 dB attenuation by default, about 0–3.1 V).
- Average about 16 samples per reading, read every 0.5–1 s, and smooth with an EMA.
- Map mV to duty with a log curve or a small lookup table, clamped to `[min duty, 255]`.
- Add hysteresis so the level doesn't jump back and forth near a boundary.
- Put the calibration points (`kLdrDarkMv`, `kLdrBrightMv`) and the min/max duty in `config.h` first. They could move to the portal or NVS later. If they do, give them their own namespace or add them to `planeradar` (see "Persistence" in CLAUDE.md).

## Suggested structure

- `hardware/backlight`: LEDC setup, `backlightSet(uint8_t duty)`, and the expander pin handling from the open question.
- `services/ambient_light`: LDR sampling, smoothing, and mapping to duty.
- Call it from `loop()` on a timer, like the other passive modules, or from the button-poll task.

## Alternative if a soldering iron isn't handy

Software dimming: scale the theme colors (or the sprite) by a brightness factor. It works with no hardware changes, but the backlight stays at full power and blacks stay a little lifted.

## Checklist

1. [ ] Read the Qualia schematic and resolve the expander-pin-4 question.
2. [ ] Bridge the bottom PWM jumper.
3. [ ] Test PWM: sweep duty and find the minimum usable duty and a good frequency.
4. [ ] Wire the LDR and 10k divider to GPIO6, then log mV readings in dark and bright rooms.
5. [ ] Implement `hardware/backlight` and `services/ambient_light`, then tune the curve and hysteresis.
