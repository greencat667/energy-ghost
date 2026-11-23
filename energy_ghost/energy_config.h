#ifndef ENERGY_CONFIG_H
#define ENERGY_CONFIG_H

// GB grid carbon-intensity bands, straight from National Grid ESO's own
// "index" classification (carbon_api.cpp passes it through unchanged) -
// the device never invents its own thresholds, just maps the five index
// words onto a GhostMood for rendering (ghost_render.h) and feeds them to
// decide_energy() (carbon_api.cpp) for the recommendation.
#define INDEX_VERY_LOW  "very low"
#define INDEX_LOW       "low"
#define INDEX_MODERATE  "moderate"
#define INDEX_HIGH      "high"
#define INDEX_VERY_HIGH "very high"

// Carbon Intensity API settlement periods are half-hourly - no point
// waking more often than that. A button press always forces an immediate
// refresh regardless of this timer.
#define WAKE_INTERVAL_US (30ULL * 60ULL * 1000000ULL)  // 30 minutes

#define API_TIMEOUT_MS 8000  // HTTPS to a public API over WiFi - a bit more headroom than a same-LAN relay call needed

// How many upcoming half-hour periods decide_energy() looks at to decide
// whether the grid "clears up" soon - 6 x 30min = next 3 hours.
#define LOOKAHEAD_PERIODS 6

#endif  // !ENERGY_CONFIG_H
