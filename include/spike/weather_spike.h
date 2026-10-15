#pragma once

#ifdef WEATHER_SPIKE
/**
 * Weather radar de-risking spike (env qualia_weather_spike). A fetch task on core 0 gets
 * Environment Canada's GeoMet radar composite as one 360 px PNG centred on Milton, ON, and
 * decodes it into a PSRAM cache; each frame copies the cache to the screen at 2x under range
 * rings. It logs internal-RAM minimums through the TLS fetch and the decode, and the decode
 * and per-frame copy times. UP/DOWN step through the range presets; each step refetches,
 * reusing the last radar time step. Never returns.
 */
[[noreturn]] void weatherSpikeRun();
#endif
