#pragma once

#include <cstddef>
#include <cstdint>

namespace config {

// --- Wi-Fi portal ---
constexpr char kPortalApName[] = "PlaneRadar-Setup";
constexpr char kPortalIp[] = "192.168.4.1";
/** mDNS host (no ".local" suffix); browser: http://plane-radar.local */
constexpr char kPortalHostname[] = "plane-radar";
constexpr char kPortalHostUrl[] = "plane-radar.local";

/** Per-attempt STA connect wait (ms); retried kWifiConnectAttempts times. */
constexpr unsigned long kWifiConnectAttemptMs = 15000;
constexpr uint8_t kWifiConnectAttempts = 3;
constexpr unsigned long kWifiPortalTimeoutSec = 0;  // 0 = no timeout while configuring
constexpr unsigned long kWifiConnectingFrameMs = 50;
/** Wait after disconnect before reconnecting (avoids portal on brief drops). */
constexpr unsigned long kWifiDownGraceMs = 4000;
/** Minimum interval between background reconnect tries. */
constexpr unsigned long kWifiReconnectIntervalMs = 15000;

// --- Buttons (Qualia UP/DOWN on the PCA9554 expander; GPIO0/BOOT is display line B4) ---
/** Holding UP this long clears Wi-Fi + settings and reboots into the portal. */
constexpr unsigned long kResetHoldMs = 3000UL;
/** Expander poll period; a level counts after 2 identical samples in a row. */
constexpr unsigned long kButtonPollMs = 20;
/** Range taps are saved to NVS once no tap has arrived for this long. */
constexpr unsigned long kRangeSaveDelayMs = 2000;

// --- I2C (expander) ---
constexpr int kI2cPinSda = 8;
constexpr int kI2cPinScl = 18;
constexpr uint32_t kI2cHz = 400000;

// --- PCA9554A expander @ 0x3F (interrupt not wired: poll it) ---
constexpr uint8_t kExpanderAddr = 0x3F;
constexpr uint8_t kExpanderPinTftSck = 0;
constexpr uint8_t kExpanderPinTftCs = 1;
constexpr uint8_t kExpanderPinTftReset = 2;
constexpr uint8_t kExpanderPinBacklight = 4;
constexpr uint8_t kExpanderPinButtonUp = 5;
constexpr uint8_t kExpanderPinButtonDown = 6;
constexpr uint8_t kExpanderPinTftMosi = 7;
/** Measured on hardware: UP/DOWN read 1 when released, 0 when pressed. */
constexpr bool kButtonActiveLow = true;

// --- Display: 4" round 720x720 NV3052C (HD40015C40), RGB-666 wired as RGB565 ---
constexpr int kDisplayWidth = 720;
constexpr int kDisplayHeight = 720;

constexpr int8_t kPanelPinDe = 2;
constexpr int8_t kPanelPinVsync = 42;
constexpr int8_t kPanelPinHsync = 41;
constexpr int8_t kPanelPinPclk = 1;
constexpr int8_t kPanelPinR[5] = {11, 10, 9, 46, 3};       // R1..R5
constexpr int8_t kPanelPinG[6] = {48, 47, 21, 14, 13, 12};  // G0..G5
constexpr int8_t kPanelPinB[5] = {40, 39, 38, 0, 45};       // B1..B5 (B4 = GPIO0/BOOT)

constexpr uint16_t kPanelHsyncPolarity = 1;
constexpr uint16_t kPanelHsyncFrontPorch = 46;
constexpr uint16_t kPanelHsyncPulseWidth = 2;
constexpr uint16_t kPanelHsyncBackPorch = 44;
constexpr uint16_t kPanelVsyncPolarity = 1;
constexpr uint16_t kPanelVsyncFrontPorch = 50;
constexpr uint16_t kPanelVsyncPulseWidth = 16;
constexpr uint16_t kPanelVsyncBackPorch = 16;
constexpr uint16_t kPanelPclkActiveNeg = 1;

/**
 * Refresh = pclk / (812 * 802): 12 MHz ~18 Hz (dark shades flicker on hardware),
 * 16 MHz ~25 Hz, 20 MHz ~31 Hz. Lower it if the image jitters or drifts under Wi-Fi load.
 */
// Measured: 12 MHz = setup AP and STA + HTTPS work (keep metal away from the antenna;
// a shielding bag under the board made the AP unusable). 16 MHz looks better, but Wi-Fi
// can't connect, even in STA mode.
constexpr int32_t kPanelPclkHz = 12000000;  // See "Open issues" in docs/qualia-port-plan.md
/**
 * SRAM bounce buffer (pixels, must divide 720*720); 0 = scan straight from PSRAM.
 * Two are allocated in internal RAM. Each line gives the refill ISR ~68 us more slack
 * at 12 MHz; 10 lines slipped the frame (vertical shift) under Wi-Fi + present load.
 */
constexpr size_t kPanelBounceBufferPx = kDisplayWidth * 20;
/** Rotate the whole UI 180 degrees if the panel is mounted upside down. */
constexpr bool kDisplayRotate180 = false;

// --- Radar center defaults (overridden via WiFi setup portal) ---
constexpr double kDefaultRadarLat = 52.3676;
constexpr double kDefaultRadarLon = 4.9041;

/** Poll adsb.fi (API public limit: 1 req/s). */
constexpr unsigned long kAdsbFetchIntervalMs = 5000;
/** Legacy scale unused — fetch uses radar::fetchRadiusKm() to screen edge. */
constexpr float kAdsbFetchRadiusScale = 1.0f;
/** false = hide aircraft with alt_baro "ground"; true = show them too. */
constexpr bool kAdsbShowGroundAircraft = false;

// --- UI colors (RGB565) — status screens ---
constexpr uint16_t kColorBlack = 0x0000;
constexpr uint16_t kColorYellow = 0xFFE0;
constexpr uint16_t kTextOnYellow = kColorBlack;
constexpr uint16_t kTextOnBlack = 0xFFFF;

}  // namespace config
