#pragma once

#ifdef WEATHER_SPIKE
/**
 * Weather radar de-risking spike (env qualia_weather_spike). It fetches Environment
 * Canada's GeoMet radar composite as one PNG centred on Milton, ON, draws it under range
 * rings and logs internal-RAM headroom through the TLS fetch and the PNG decode.
 * UP/DOWN step through the range presets; each step refetches. Never returns.
 */
[[noreturn]] void weatherSpikeRun();
#endif
