#ifndef CARBON_API_H
#define CARBON_API_H

#include <Arduino.h>

// Fetches GB grid carbon intensity directly from the device - no relay, no
// computer needing to be on. There is no LLM involved (decide_energy()
// below is plain rule-based logic), so nothing here requires a
// server in the loop. See README.md's "Standalone" section for the
// tradeoffs this took on to get there (mainly: no TLS certificate
// pinning - see https_get() in carbon_api.cpp).
//
// Returns true and fills all three on success (regional if location
// detection + regional lookup both worked, national otherwise). Returns
// false only if even the national fallback fails (e.g. no WiFi at all) -
// the caller (energy_ghost.ino) falls back to its own RTC-cached reading
// in that case.
bool fetch_carbon_data(int &intensity, String &index, String &recommendation);

#endif  // !CARBON_API_H
