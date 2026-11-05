#pragma once

#include <cstdint>

/**
 * Qualia UP/DOWN buttons on the PCA9554 expander, polled from a low-priority task on
 * core 0 so taps register during blocking HTTP or draw work.
 *
 * A tap counts on release. Holding one button alone for kButtonHoldMs gives a hold event
 * instead (while it's still held). Holding both for kResetHoldMs asks for the settings reset;
 * once both are down, neither gives a tap or a hold, so a staggered two-button press can't
 * also switch mode.
 */

enum class ButtonEvent : uint8_t { None, Up, Down, UpHold, DownHold };

/** Start the poll task. Call after displayInit() (the expander is set up there). */
void buttonsInit();

/** Next queued tap or hold, or None. */
ButtonEvent buttonsConsumeEvent();

/** How long both buttons have been held together (ms); 0 when they aren't. */
unsigned long buttonsBothHeldMs();

/** True once both buttons have been held together for kResetHoldMs (set while still held). */
bool buttonsResetRequested();
