#pragma once

void statusScreenPortal();
void statusScreenConnectFailed();
void statusScreenWifiReset();
/** Both buttons held: the reset happens in seconds_left unless they're released. */
void statusScreenResetCountdown(int seconds_left);

/** Saved-network connect animation (call Tick until connect finishes). */
void statusScreenConnectingBegin(const char* ssid);
void statusScreenConnectingTick();
