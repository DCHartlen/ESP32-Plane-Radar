#pragma once

#ifdef PANEL_TEST
/**
 * Phase 2 bring-up screen (env qualia_panel_test): color bars, circles, text,
 * looped HTTPS fetches and expander button logging. Never returns.
 */
[[noreturn]] void panelTestRun();
#endif
