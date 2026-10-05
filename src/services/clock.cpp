#include "services/clock.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"
#include "data/tz_posix.h"
#include "services/radar_location.h"
#include "services/tls_lock.h"

namespace services::clock {

namespace {

/** Own namespace, apart from planeradar/wifi/location (see "Persistence" in CLAUDE.md). */
constexpr char kPrefsNamespace[] = "clock";
/** "auto", or a fixed zone's POSIX string. (Not "tz": that key held the pre-auto setting.) */
constexpr char kPrefsZoneKey[] = "zone";
constexpr char kPrefsHour24Key[] = "hour24";
/** Last automatic lookup: its POSIX string, IANA name and the location it was for. */
constexpr char kPrefsAutoTzKey[] = "autoTz";
constexpr char kPrefsAutoNameKey[] = "autoName";
constexpr char kPrefsAutoLatKey[] = "autoLat";
constexpr char kPrefsAutoLonKey[] = "autoLon";
constexpr char kAutoValue[] = "auto";

/** Open-Meteo answers with the IANA zone for a coordinate; no key needed. */
constexpr char kZoneLookupUrl[] = "https://api.open-meteo.com/v1/forecast?forecast_days=1"
                                  "&timezone=auto&latitude=";
constexpr unsigned long kLookupTimeoutMs = 6000;
/** A lookup is reused while the radar centre stays within this many degrees. */
constexpr float kSameLocationDeg = 0.01f;

struct Zone {
  const char* label;
  const char* posix_tz;
};

/**
 * Fixed zones for the portal, as POSIX TZ strings so newlib handles DST. NVS stores the
 * string, not the index, so reordering or adding entries never changes a saved zone.
 */
constexpr Zone kZones[] = {
    {"UTC", "UTC0"},
    {"Hawaii (Honolulu)", "HST10"},
    {"Alaska (Anchorage)", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"US/Canada Pacific (Vancouver, Los Angeles)", "PST8PDT,M3.2.0,M11.1.0"},
    {"Arizona, Yukon (no DST)", "MST7"},
    {"US/Canada Mountain (Calgary, Denver)", "MST7MDT,M3.2.0,M11.1.0"},
    {"Saskatchewan, Mexico City (no DST)", "CST6"},
    {"US/Canada Central (Winnipeg, Chicago)", "CST6CDT,M3.2.0,M11.1.0"},
    {"US/Canada Eastern (Toronto, New York)", "EST5EDT,M3.2.0,M11.1.0"},
    {"Atlantic (Halifax)", "AST4ADT,M3.2.0,M11.1.0"},
    {"Newfoundland (St. John's)", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"Brazil, Argentina (Sao Paulo, Buenos Aires)", "<-03>3"},
    {"UK, Ireland (London, Dublin)", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Portugal (Lisbon)", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Central Europe (Amsterdam, Berlin, Paris)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"West Africa (Lagos)", "WAT-1"},
    {"Eastern Europe (Athens, Helsinki, Kyiv)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"South Africa (Johannesburg)", "SAST-2"},
    {"Moscow, Istanbul", "<+03>-3"},
    {"East Africa (Nairobi)", "EAT-3"},
    {"Gulf (Dubai)", "<+04>-4"},
    {"India (Kolkata)", "IST-5:30"},
    {"Indochina (Bangkok, Jakarta)", "<+07>-7"},
    {"China, Singapore, Perth", "<+08>-8"},
    {"Japan, Korea (Tokyo, Seoul)", "JST-9"},
    {"Australia Central (Adelaide)", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia Queensland (Brisbane)", "AEST-10"},
    {"Australia Eastern (Sydney, Melbourne)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New Zealand (Auckland)", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
constexpr size_t kZoneCount = sizeof(kZones) / sizeof(kZones[0]);
constexpr size_t kUtcZone = 0;

/** Anything before this is the unset RTC (1970), not NTP time. */
constexpr time_t kValidAfter = 1700000000;  // Nov 2023

bool s_auto = true;
size_t s_zone = kUtcZone;  // used when !s_auto
bool s_hour24 = true;
bool s_sntp_started = false;

/** Result of the last automatic lookup (or the longitude estimate until one succeeds). */
char s_auto_tz[64] = "";
float s_auto_lat = NAN;
float s_auto_lon = NAN;
bool s_auto_resolved = false;  // s_auto_tz came from a lookup for (s_auto_lat, s_auto_lon)
unsigned long s_last_lookup_ms = 0;
bool s_lookup_tried = false;

const char* activeTz() {
  if (!s_auto) {
    return kZones[s_zone].posix_tz;
  }
  return s_auto_tz[0] != '\0' ? s_auto_tz : "UTC0";
}

void applyZone() {
  setenv("TZ", activeTz(), 1);
  tzset();
}

size_t zoneIndexFor(const char* posix_tz) {
  for (size_t i = 0; i < kZoneCount; ++i) {
    if (strcmp(kZones[i].posix_tz, posix_tz) == 0) {
      return i;
    }
  }
  return kUtcZone;
}

void saveSettings() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kPrefsZoneKey, s_auto ? kAutoValue : kZones[s_zone].posix_tz);
  prefs.putBool(kPrefsHour24Key, s_hour24);
  prefs.end();
}

void saveAutoLookup(const char* iana) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putString(kPrefsAutoTzKey, s_auto_tz);
  prefs.putString(kPrefsAutoNameKey, iana);
  prefs.putFloat(kPrefsAutoLatKey, s_auto_lat);
  prefs.putFloat(kPrefsAutoLonKey, s_auto_lon);
  prefs.end();
}

/** POSIX fixed offset, e.g. -14400 s -> "<-04>4", 19800 s -> "<+0530>-5:30" (POSIX signs are inverted). */
void fixedOffsetTz(int offset_s, char* out, size_t out_len) {
  const char sign = offset_s < 0 ? '-' : '+';
  const int abs_min = std::abs(offset_s) / 60;
  const int h = abs_min / 60;
  const int m = abs_min % 60;
  const char* posix_sign = offset_s < 0 ? "" : "-";
  if (m == 0) {
    snprintf(out, out_len, "<%c%02d>%s%d", sign, h, posix_sign, h);
  } else {
    snprintf(out, out_len, "<%c%02d%02d>%s%d:%02d", sign, h, m, posix_sign, h, m);
  }
}

const char* posixForIana(const char* iana) {
  size_t lo = 0;
  size_t hi = data::kTzPosixCount;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = strcmp(data::kTzPosix[mid].iana, iana);
    if (c == 0) {
      return data::kTzPosix[mid].posix;
    }
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return nullptr;
}

bool sameLocation(float lat, float lon) {
  return fabsf(lat - s_auto_lat) < kSameLocationDeg && fabsf(lon - s_auto_lon) < kSameLocationDeg;
}

/** Until a lookup succeeds: the nautical zone for the longitude (right offset, no DST). */
void estimateFromLongitude(float lon) {
  const int hours = static_cast<int>(lroundf(lon / 15.0f));
  fixedOffsetTz(hours * 3600, s_auto_tz, sizeof(s_auto_tz));
}

/** Blocking HTTPS lookup of the radar centre's IANA zone. */
bool lookupZone(float lat, float lon) {
  char url[160];
  snprintf(url, sizeof(url), "%s%.4f&longitude=%.4f", kZoneLookupUrl, lat, lon);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(kLookupTimeoutMs);
  http.setConnectTimeout(kLookupTimeoutMs);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("Clock: zone lookup HTTP %d\n", code);
    http.end();
    return false;
  }
  const String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    Serial.println("Clock: zone lookup returned bad JSON");
    return false;
  }
  const char* iana = doc["timezone"] | "";
  const char* posix = posixForIana(iana);
  if (posix != nullptr) {
    snprintf(s_auto_tz, sizeof(s_auto_tz), "%s", posix);
  } else if (doc["utc_offset_seconds"].is<int>()) {
    // Not in the table (newer tzdata than the build): today's offset, without DST rules.
    fixedOffsetTz(doc["utc_offset_seconds"].as<int>(), s_auto_tz, sizeof(s_auto_tz));
  } else {
    Serial.printf("Clock: zone lookup gave no usable zone ('%s')\n", iana);
    return false;
  }

  s_auto_lat = lat;
  s_auto_lon = lon;
  s_auto_resolved = true;
  saveAutoLookup(iana);
  Serial.printf("Clock: radar location is in %s (%s)\n", iana, s_auto_tz);
  return true;
}

bool localNow(struct tm* out) {
  const time_t now = time(nullptr);
  if (now < kValidAfter) {
    return false;
  }
  localtime_r(&now, out);
  return true;
}

void logSettings() {
  if (s_auto) {
    Serial.printf("Clock: automatic (%s), %s\n", activeTz(), s_hour24 ? "24 h" : "12 h");
  } else {
    Serial.printf("Clock: %s, %s\n", kZones[s_zone].label, s_hour24 ? "24 h" : "12 h");
  }
}

}  // namespace

void init() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, true)) {
    char zone[64] = "";
    prefs.getString(kPrefsZoneKey, zone, sizeof(zone));
    s_auto = zone[0] == '\0' || strcmp(zone, kAutoValue) == 0;
    s_zone = s_auto ? kUtcZone : zoneIndexFor(zone);
    s_hour24 = prefs.getBool(kPrefsHour24Key, true);
    prefs.getString(kPrefsAutoTzKey, s_auto_tz, sizeof(s_auto_tz));
    s_auto_lat = prefs.getFloat(kPrefsAutoLatKey, NAN);
    s_auto_lon = prefs.getFloat(kPrefsAutoLonKey, NAN);
    prefs.end();
  }
  const float lat = static_cast<float>(services::location::lat());
  const float lon = static_cast<float>(services::location::lon());
  s_auto_resolved = s_auto_tz[0] != '\0' && sameLocation(lat, lon);
  if (!s_auto_resolved) {
    estimateFromLongitude(lon);
  }
  applyZone();
  logSettings();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!s_sntp_started) {
    s_sntp_started = true;
    configTzTime(activeTz(), config::kNtpServer1, config::kNtpServer2);
    Serial.println("Clock: SNTP started");
  }

  if (!s_auto) {
    return;
  }
  const float lat = static_cast<float>(services::location::lat());
  const float lon = static_cast<float>(services::location::lon());
  if (s_auto_resolved && sameLocation(lat, lon)) {
    return;
  }
  if (s_lookup_tried && millis() - s_last_lookup_ms < config::kClockZoneRetryMs) {
    return;
  }
  // The ADS-B fetch task may hold the TLS session; try again on a later loop.
  if (!tls::tryLock()) {
    return;
  }
  s_lookup_tried = true;
  s_last_lookup_ms = millis();
  const bool found = lookupZone(lat, lon);
  tls::unlock();
  if (!found) {
    s_auto_resolved = false;
    estimateFromLongitude(lon);
    Serial.printf("Clock: zone lookup failed, using %s until it works\n", s_auto_tz);
  }
  applyZone();
}

bool timeValid() { return time(nullptr) >= kValidAfter; }

bool formatTime(char* hhmm, size_t hhmm_len, char* suffix, size_t suffix_len) {
  struct tm t;
  if (!localNow(&t)) {
    return false;
  }
  if (s_hour24) {
    snprintf(hhmm, hhmm_len, "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(suffix, suffix_len, "%s", "");
  } else {
    const int hour12 = (t.tm_hour % 12 == 0) ? 12 : t.tm_hour % 12;
    snprintf(hhmm, hhmm_len, "%d:%02d", hour12, t.tm_min);
    snprintf(suffix, suffix_len, "%s", t.tm_hour < 12 ? "AM" : "PM");
  }
  return true;
}

int minuteOfDay() {
  struct tm t;
  if (!localNow(&t)) {
    return -1;
  }
  return t.tm_hour * 60 + t.tm_min;
}

size_t zoneCount() { return kZoneCount; }

const char* zoneLabel(size_t index) {
  return index < kZoneCount ? kZones[index].label : "";
}

bool zoneAutomatic() { return s_auto; }

size_t currentZoneIndex() { return s_zone; }

bool use24Hour() { return s_hour24; }

void saveFromPortal(const char* zone_value, const char* checkbox_24h) {
  if (zone_value != nullptr && strcmp(zone_value, kAutoValue) == 0) {
    s_auto = true;
  } else if (zone_value != nullptr && zone_value[0] != '\0') {
    char* end = nullptr;
    const unsigned long index = strtoul(zone_value, &end, 10);
    if (end != zone_value && *end == '\0' && index < kZoneCount) {
      s_auto = false;
      s_zone = index;
    }
  }
  // WiFiManager checkboxes submit their value ("T") when checked and nothing otherwise.
  s_hour24 = checkbox_24h != nullptr && checkbox_24h[0] != '\0';
  // A new radar location is looked up on the next loop(); retry right away, not after the backoff.
  s_lookup_tried = false;
  applyZone();
  saveSettings();
  logSettings();
}

void reset() {
  s_auto = true;
  s_zone = kUtcZone;
  s_hour24 = true;
  s_auto_tz[0] = '\0';
  s_auto_resolved = false;
  s_auto_lat = NAN;
  s_auto_lon = NAN;
  applyZone();
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
}

}  // namespace services::clock
