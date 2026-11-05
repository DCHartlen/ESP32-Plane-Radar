#pragma once

/** True when the next boot should show the setup screen first (after credential reset). */
bool wifiShowsSetupScreenOnBoot();
void wifiResetCredentialsAndReboot();
/** Boot flow: connect with UI, open portal only if saved creds fail. */
bool wifiSetupConnect();
/** Reconnect using saved creds; never opens the captive portal. */
bool wifiReconnect();
/**
 * Keeps the LAN config portal alive and runs the two-button reset (countdown screen, then the
 * reset); call every loop() iteration (also the adsb poll hook during long HTTP I/O).
 */
void wifiLoop();
/** The reset countdown is on screen; don't draw over it. */
bool wifiResetCountdownShowing();
/** True once after a countdown was cancelled: the screen under it needs redrawing. */
bool wifiTakeResetCountdownCancelled();
