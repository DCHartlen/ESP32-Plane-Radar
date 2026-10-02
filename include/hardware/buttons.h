#pragma once

#include <cstdint>

/**
 * Qualia UP/DOWN buttons on the PCA9554 expander, polled from a low-priority task on
 * core 0 so taps register during blocking HTTP or draw work.
 */

enum class ButtonEvent : uint8_t { None, Up, Down };

/** Start the poll task. Call after displayInit() (the expander is set up there). */
void buttonsInit();

/** Next queued tap (counted on release, held < kResetHoldMs), or None. */
ButtonEvent buttonsConsumeEvent();

/** True once UP has been held for kResetHoldMs (set while it is still held). */
bool buttonsResetRequested();
