#!/usr/bin/env python3
"""
Relay server for Energy Ghost.

One request from the device:
  GET /carbon - GB grid carbon intensity RIGHT NOW plus a DECISION (not just
                a number): combines National Grid ESO's current reading with
                its own forward forecast via a small deterministic rule
                engine (not an LLM - reliability
                over flavour).

Uses REGIONAL carbon intensity (more accurate than the GB-wide national
figure) when it can work out roughly where this relay is running, via free
IP geolocation (ip-api.com, no key) resolved to a UK postcode area - see
detect_postcode(). Falls back to the national figure if location detection
or the regional lookup fails for any reason. Set ENERGY_POSTCODE to skip
detection entirely and pin a postcode by hand (IP geolocation can be off by
a fair distance - it reflects the ISP's routing, not the building).

Stdlib only (urllib) - no API key needed (carbonintensity.org.uk is a free,
open National Grid ESO API; ip-api.com is also free/keyless for this
volume of use). Run:
    python3 energy_relay.py
"""
import json
import os
import sys
import time
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8790
API_BASE = "https://api.carbonintensity.org.uk"

# Manual override - set this to skip IP geolocation entirely (e.g. if the
# relay's connection routes through an ISP hub far from where it actually
# is, which IP geolocation can't tell apart from genuinely being there).
ENERGY_POSTCODE = os.environ.get("ENERGY_POSTCODE")

# Re-check location at most this often - a home connection's IP-derived
# location doesn't change minute to minute, no need to hit ip-api.com every
# single /carbon request (the device only polls every 30 min anyway).
LOCATION_REFRESH_S = 6 * 3600
_location_cache = {"outcode": None, "label": None, "checked": 0.0}

# Cleanliness order, worst to best index - National Grid ESO's own five
# bands, never re-derived from raw gCO2/kWh here (their thresholds shift
# over time as the grid decarbonises; passing their classification through
# unchanged means this stays correct without needing its own thresholds).
INDEX_ORDER = {"very low": 0, "low": 1, "moderate": 2, "high": 3, "very high": 4}
LOOKAHEAD_PERIODS = 6  # 6 x 30min = next 3 hours


def fetch_json(url):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fetch_current():
    data = fetch_json(f"{API_BASE}/intensity")
    period = data["data"][0]
    intensity = period["intensity"]
    value = intensity.get("actual") or intensity.get("forecast")
    return value, intensity["index"]


def fetch_upcoming():
    """Next few half-hour periods' forecast index, via the 'forward 24h from
    now' endpoint - only the index words are used (see decide_energy)."""
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
    data = fetch_json(f"{API_BASE}/intensity/{now}/fw24h")
    periods = data["data"][:LOOKAHEAD_PERIODS]
    return [p["intensity"]["index"] for p in periods]


def detect_postcode():
    """Returns (outcode, label) for the region to use, or (None, None) to
    fall back to national. ENERGY_POSTCODE always wins if set. Otherwise
    asks ip-api.com what UK postcode area this connection appears to be in
    - cached for LOCATION_REFRESH_S since it won't change minute to minute."""
    if ENERGY_POSTCODE:
        return ENERGY_POSTCODE.strip().upper(), "manual override"

    now = time.time()
    if _location_cache["outcode"] and now - _location_cache["checked"] < LOCATION_REFRESH_S:
        return _location_cache["outcode"], _location_cache["label"]

    try:
        data = fetch_json("http://ip-api.com/json/?fields=status,message,country,regionName,city,zip")
        if data.get("status") != "success":
            raise RuntimeError(data.get("message", "lookup failed"))
        outcode = (data.get("zip") or "").split()[0].strip().upper()
        if not outcode:
            raise RuntimeError("no postcode in geolocation response (non-UK connection?)")
        label = f"{data.get('city')}, {data.get('regionName')}"
        _location_cache.update(outcode=outcode, label=label, checked=now)
        return outcode, label
    except Exception as exc:
        sys.stderr.write(f"[energy-relay] [location] IP geolocation failed, using national figure: {exc}\n")
        return None, None


def fetch_regional(outcode):
    """Current regional reading plus upcoming regional forecast for a UK
    postcode area (DNO license area, not the exact postcode - GB carbon
    intensity data is only published at that granularity). Shape mirrors
    fetch_current()/fetch_upcoming() but from the /regional/* endpoints,
    one dict level deeper. Regional data is forecast-only (no 'actual'
    field the way national sometimes has)."""
    current = fetch_json(f"{API_BASE}/regional/postcode/{outcode}")
    region = current["data"][0]
    period = region["data"][0]
    intensity = period["intensity"]["forecast"]
    index = period["intensity"]["index"]

    # The current reading is the valuable part - if only the forward-looking
    # call fails, still return the (accurate) regional current reading with
    # an empty upcoming list rather than discarding it and falling all the
    # way back to the national figure.
    upcoming = []
    try:
        now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%MZ")
        fw = fetch_json(f"{API_BASE}/regional/intensity/{now}/fw24h/postcode/{outcode}")
        upcoming = [p["intensity"]["index"] for p in fw["data"]["data"][:LOOKAHEAD_PERIODS]]
    except Exception as exc:
        sys.stderr.write(f"[energy-relay] [carbon] regional forecast lookahead failed for {outcode}: {exc}\n")

    return intensity, index, upcoming, region["shortname"]


def decide_energy(intensity, current_index, upcoming_indices):
    """Deterministic rule engine - not the LLM - for a "should I run this
    now" answer. current_index and upcoming_indices are National Grid ESO's
    own classification, ordered worst-to-best by INDEX_ORDER; the only
    thing decided here is what to DO about that, not how clean it is."""
    cur = INDEX_ORDER.get(current_index, 2)
    future_min = min((INDEX_ORDER.get(i, 2) for i in upcoming_indices), default=cur)
    clears_up = future_min < cur

    if current_index in ("very low", "low"):
        return "RUN NOW - grid's clean, good time for the big stuff"
    if current_index == "moderate":
        if clears_up:
            return "WAIT IF YOU CAN - cleaner within a few hours"
        return "OK TO RUN - unlikely to improve much soon"
    # high / very high
    if clears_up:
        return "WAIT - grid clears up later, hold off if you can"
    return f"AVOID IF POSSIBLE - {intensity} gCO2/kWh, no relief in sight"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("[energy-relay] " + (fmt % args) + "\n")

    def do_GET(self):
        if self.path == "/health":
            body = b"ok"
            self.send_response(200)

        elif self.path == "/carbon":
            try:
                region_name = None
                outcode, label = detect_postcode()
                if outcode:
                    try:
                        intensity, index, upcoming, region_name = fetch_regional(outcode)
                    except Exception as exc:
                        self.log_message("regional lookup failed for %s (%s), falling back to national: %s", outcode, label, exc)
                        outcode = None  # so the log line below correctly says "national"

                if not outcode:
                    intensity, index = fetch_current()
                    try:
                        upcoming = fetch_upcoming()
                    except Exception as exc:
                        self.log_message("forecast lookahead failed (using current only): %s", exc)
                        upcoming = []

                recommendation = decide_energy(intensity, index, upcoming)
                if region_name:
                    recommendation += f" ({region_name})"
                text = f"{intensity}|{index}|{recommendation}"
                body = text.encode("ascii", "ignore")
                self.send_response(200)
                source = f"regional/{outcode}" if region_name else "national"
                self.log_message("[carbon] %d gCO2/kWh (%s, %s) -> %r", intensity, index, source, recommendation)
            except Exception as exc:
                self.log_message("[carbon] failed: %s", exc)
                self.send_response(504)
                body = b""

        else:
            self.send_response(404)
            body = b""

        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    server = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Energy Ghost relay listening on http://0.0.0.0:{PORT}  (health check: /health)")
    print(f"Using {API_BASE} (National Grid ESO Carbon Intensity API, no key needed).")
    if ENERGY_POSTCODE:
        print(f"Location pinned via ENERGY_POSTCODE={ENERGY_POSTCODE!r} - regional carbon intensity.")
    else:
        print("Location auto-detected via IP geolocation on first request (set ENERGY_POSTCODE to override).")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
