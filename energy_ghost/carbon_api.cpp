// Direct port of relay/energy_relay.py's logic onto the device - same two
// free, keyless APIs (National Grid ESO Carbon Intensity, ip-api.com for
// location), same deterministic decide_energy() rule engine, same
// regional-with-national-fallback shape. See that file's comments for the
// reasoning behind each decision band; this just re-expresses it in C++
// with hand-rolled scalar JSON extraction instead of Python's json module
// (no JSON library pulled in - same minimal-parsing convention as every
// other app in this project, e.g. the pipe-delimited wire formats).
#include "carbon_api.h"
#include "energy_config.h"
#include "secrets.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <vector>

// Pulls the FIRST occurrence of "key" (include the full "\"field\":\"" or
// "\"field\":" prefix) - fine for these payloads since every value this
// code reads occurs exactly once in the bodies it's read from (a single
// current-period object, or - for extract_index_series() - a flat
// repeating list where "first N occurrences in order" is exactly what's
// wanted). Not a general JSON parser; would misbehave on a body where the
// same key legitimately appears earlier in an unrelated context, which
// doesn't happen in any response used here.
static bool extract_string_after(const String &body, const String &key, String &out, int from = 0) {
  int idx = body.indexOf(key, from);
  if (idx == -1) return false;
  idx += key.length();
  int end = body.indexOf('"', idx);
  if (end == -1) return false;
  out = body.substring(idx, end);
  return true;
}

static bool extract_int_value(const String &body, const String &key, int &out) {
  int idx = body.indexOf(key);
  if (idx == -1) return false;
  idx += key.length();
  int len = body.length();
  bool neg = false;
  if (idx < len && body[idx] == '-') { neg = true; idx++; }
  int val = 0;
  bool any = false;
  while (idx < len && isDigit(body[idx])) {
    val = val * 10 + (body[idx] - '0');
    idx++;
    any = true;
  }
  if (!any) return false;  // e.g. "actual":null - no digits, correctly treated as absent
  out = neg ? -val : val;
  return true;
}

// National sometimes has "actual" (a settled reading) alongside "forecast"
// (which can be null before the period's actually happened) - prefer
// actual when present, same as fetch_current()'s `.get("actual") or
// .get("forecast")` in the Python relay. Regional only ever has
// "forecast", so the actual lookup simply won't match there.
static bool extract_intensity_value(const String &body, int &out) {
  if (extract_int_value(body, "\"actual\":", out)) return true;
  return extract_int_value(body, "\"forecast\":", out);
}

// Up to `max_count` "index" values in the order they appear - used for the
// forward-looking period lists (regional and national fw24h responses are
// both a flat repeating list of periods, just nested one level differently
// - irrelevant here since this is a plain substring scan, not a real
// parser that would care about nesting depth).
static void extract_index_series(const String &body, int max_count, std::vector<String> &out) {
  out.clear();
  int from = 0;
  const String key = "\"index\":\"";
  for (int i = 0; i < max_count; i++) {
    String value;
    int idx = body.indexOf(key, from);
    if (idx == -1) break;
    idx += key.length();
    int end = body.indexOf('"', idx);
    if (end == -1) break;
    out.push_back(body.substring(idx, end));
    from = end + 1;
  }
}

static bool https_get(const String &url, String &out) {
  WiFiClientSecure client;
  // No CA pinning - accepted tradeoff for a device that only ever reads
  // public, non-sensitive grid-intensity numbers over GET, no credentials
  // anywhere in the request. See README.md's "Standalone" section.
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    out = http.getString();
    ok = out.length() > 0;
  } else {
    Serial.printf("[carbon_api] GET %s failed, HTTP %d\n", url.c_str(), code);
  }
  http.end();
  return ok;
}

static bool http_get(const String &url, String &out) {
  HTTPClient http;
  http.setTimeout(API_TIMEOUT_MS);
  if (!http.begin(url)) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    out = http.getString();
    ok = out.length() > 0;
  }
  http.end();
  return ok;
}

// NTP sync, needed only for the fw24h forecast calls' "from" timestamp -
// nothing else on this device cares about wall-clock time. Best-effort:
// callers treat a sync failure the same as a forecast-fetch failure (skip
// the lookahead, decide_energy() just sees an empty upcoming list).
static bool sync_time_iso_now(String &iso_now) {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5000)) return false;
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%MZ", &timeinfo);
  iso_now = String(buf);
  return true;
}

static bool detect_postcode(String &outcode) {
  if (strlen(MANUAL_POSTCODE) > 0) {
    outcode = String(MANUAL_POSTCODE);
    outcode.toUpperCase();
    Serial.printf("[carbon_api] location: manual override (%s)\n", outcode.c_str());
    return true;
  }
  String body;
  if (!http_get("http://ip-api.com/json/?fields=status,zip,city,regionName", body)) {
    Serial.println("[carbon_api] location: ip-api.com unreachable, using national figure");
    return false;
  }
  if (body.indexOf("\"status\":\"success\"") == -1) {
    Serial.println("[carbon_api] location: geolocation failed (non-UK connection?), using national figure");
    return false;
  }
  String zip;
  if (!extract_string_after(body, "\"zip\":\"", zip) || zip.length() == 0) {
    Serial.println("[carbon_api] location: no postcode in response, using national figure");
    return false;
  }
  int sp = zip.indexOf(' ');
  outcode = (sp == -1) ? zip : zip.substring(0, sp);
  outcode.toUpperCase();
  Serial.printf("[carbon_api] location: auto-detected %s\n", outcode.c_str());
  return true;
}

static bool fetch_regional_current(const String &outcode, int &intensity, String &index, String &region_name) {
  String body;
  if (!https_get(String("https://api.carbonintensity.org.uk/regional/postcode/") + outcode, body)) return false;
  if (!extract_string_after(body, "\"shortname\":\"", region_name)) return false;
  if (!extract_string_after(body, "\"index\":\"", index)) return false;
  if (!extract_intensity_value(body, intensity)) return false;
  return true;
}

static bool fetch_regional_upcoming(const String &outcode, std::vector<String> &upcoming) {
  String now_iso;
  if (!sync_time_iso_now(now_iso)) return false;
  String body;
  String url = String("https://api.carbonintensity.org.uk/regional/intensity/") + now_iso + "/fw24h/postcode/" + outcode;
  if (!https_get(url, body)) return false;
  extract_index_series(body, LOOKAHEAD_PERIODS, upcoming);
  return !upcoming.empty();
}

static bool fetch_national_current(int &intensity, String &index) {
  String body;
  if (!https_get("https://api.carbonintensity.org.uk/intensity", body)) return false;
  if (!extract_string_after(body, "\"index\":\"", index)) return false;
  if (!extract_intensity_value(body, intensity)) return false;
  return true;
}

static bool fetch_national_upcoming(std::vector<String> &upcoming) {
  String now_iso;
  if (!sync_time_iso_now(now_iso)) return false;
  String body;
  if (!https_get(String("https://api.carbonintensity.org.uk/intensity/") + now_iso + "/fw24h", body)) return false;
  extract_index_series(body, LOOKAHEAD_PERIODS, upcoming);
  return !upcoming.empty();
}

static int index_order(const String &idx) {
  if (idx == INDEX_VERY_LOW) return 0;
  if (idx == INDEX_LOW) return 1;
  if (idx == INDEX_MODERATE) return 2;
  if (idx == INDEX_HIGH) return 3;
  if (idx == INDEX_VERY_HIGH) return 4;
  return 2;  // unrecognised index word - treat as moderate, same default as the Python relay
}

// Direct port of relay/energy_relay.py's decide_energy() - deterministic,
// not the LLM (there is no LLM anywhere in this app). See that file for
// the full reasoning; kept byte-for-byte equivalent band logic here.
static String decide_energy(int intensity, const String &current_index, const std::vector<String> &upcoming) {
  int cur = index_order(current_index);
  int future_min = cur;
  for (auto &u : upcoming) {
    int v = index_order(u);
    if (v < future_min) future_min = v;
  }
  bool clears_up = future_min < cur;

  if (current_index == INDEX_VERY_LOW || current_index == INDEX_LOW) {
    return "RUN NOW - grid's clean, good time for the big stuff";
  }
  if (current_index == INDEX_MODERATE) {
    return clears_up ? "WAIT IF YOU CAN - cleaner within a few hours"
                      : "OK TO RUN - unlikely to improve much soon";
  }
  if (clears_up) return "WAIT - grid clears up later, hold off if you can";
  char buf[64];
  snprintf(buf, sizeof(buf), "AVOID IF POSSIBLE - %d gCO2/kWh, no relief in sight", intensity);
  return String(buf);
}

bool fetch_carbon_data(int &intensity, String &index, String &recommendation) {
  String outcode;
  bool have_postcode = detect_postcode(outcode);

  String region_name;
  std::vector<String> upcoming;
  bool got_regional = have_postcode && fetch_regional_current(outcode, intensity, index, region_name);
  if (got_regional) {
    if (!fetch_regional_upcoming(outcode, upcoming)) {
      Serial.println("[carbon_api] regional forecast lookahead failed (using current only)");
    }
  } else {
    if (have_postcode) {
      Serial.println("[carbon_api] regional lookup failed, falling back to national");
    }
    if (!fetch_national_current(intensity, index)) {
      Serial.println("[carbon_api] national lookup failed too - no reading available");
      return false;
    }
    if (!fetch_national_upcoming(upcoming)) {
      Serial.println("[carbon_api] national forecast lookahead failed (using current only)");
    }
  }

  recommendation = decide_energy(intensity, index, upcoming);
  if (got_regional) {
    recommendation += " (" + region_name + ")";
  }
  Serial.printf("[carbon_api] %d gCO2/kWh (%s, %s) -> %s\n", intensity, index.c_str(),
                got_regional ? "regional" : "national", recommendation.c_str());
  return true;
}
