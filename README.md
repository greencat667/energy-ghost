# Energy Ghost

**A ghost on an e-paper screen that tells you whether now is a good time to run the washing machine.** Every 30 minutes it checks how dirty Great Britain's electricity grid is and draws one ghost: hollow, sparkly and smiling when the grid is clean, solid black and jagged when it is dirty. Underneath is a short recommendation such as `RUN NOW` or `WAIT IF YOU CAN`. It reads from across the room in half a second, with no numbers to interpret.

It is standalone: no server, no phone app, no sensor, no menu. It joins your WiFi, calls two free public APIs itself, then goes back to deep sleep. The screen keeps its image with no power.

## What it shows

| Grid carbon index | Ghost | Recommendation |
|---|---|---|
| Very low | Hollow, sparkles, closed happy eyes | `RUN NOW` |
| Low | Hollow, cheerful smile | `RUN NOW` |
| Moderate | Hollow, neutral flat mouth | `WAIT IF YOU CAN` or `OK TO RUN` |
| High | Hatched texture, worried frown | `WAIT` or `AVOID IF POSSIBLE` |
| Very high | Solid black, jagged hem, alarmed eyes | `AVOID IF POSSIBLE` |
| No reading yet | Hollow, confused `?` | none |

The screen also shows the band word, the gCO2/kWh figure, your region, and a `(live)` or `(cached)` tag. Press either button to refresh straight away.

## Buy the hardware

Energy Ghost runs on the **Waveshare ESP32-S3-ePaper-1.54** board:

**https://www.waveshare.com/esp32-s3-epaper-1.54.htm?sku=32298**

You also need a USB-C cable that carries data (many charge-only cables don't). A small LiPo battery is optional; check the Waveshare page for the right connector. You need 2.4 GHz WiFi.

## The quickest way: point your agent at this page

If you use an AI coding agent (Claude Code, Codex, Cursor, or similar), you don't have to do the steps below yourself. Plug the board in and tell the agent:

> Read https://github.com/greencat667/energy-ghost and flash Energy Ghost onto the ESP32 board plugged into this computer. Ask me for my WiFi name and password.

The agent can clone the repo, install the tools, write `secrets.h`, compile and flash. The only physical step is that you may need to hold BOOT while plugging in (see [Download mode](#download-mode)).

## How the decision works

`decide_energy()` is a small deterministic rule engine, not an LLM. It combines the current half-hour's index with the forecast for the next three hours (6 half-hour periods):

- `very low` / `low`: always `RUN NOW`, whatever is coming.
- `moderate`: `WAIT IF YOU CAN` if the next few hours clear up to a better band, otherwise `OK TO RUN`.
- `high` / `very high`: `WAIT` if it clears up later, otherwise `AVOID IF POSSIBLE`.

The five-band classification is never re-derived on the device. It is passed through unchanged from the Carbon Intensity API, because the official thresholds shift as the grid decarbonises.

## Region, auto-detected

The GB-wide figure hides a lot, so Energy Ghost uses the API's regional endpoints. On each wake it asks `ip-api.com` which UK postcode area your connection appears to be in, then reads the regional current and forecast figures for that outward code.

IP geolocation reflects your ISP's routing, not your building, and can be a long way out. To pin your location, set `MANUAL_POSTCODE` in `secrets.h` to a UK outward code such as `"SW1A"`, which skips detection (and the ip-api.com call) entirely. It falls back to the national figure if detection fails (non-UK connection, ip-api.com unreachable), the postcode is invalid, or the regional endpoints fail.

## Offline fallback

If WiFi or the APIs are unreachable, it shows the last good reading (kept in RTC memory, which survives deep sleep) tagged `(cached)`. On a cold start with nothing cached it shows the confused `?` ghost rather than guessing.

## Install it yourself

### 1. Install the tools

Install [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/) (macOS: `brew install arduino-cli`). On Apple silicon you may also need Rosetta; if compiling fails with "bad CPU type in executable", run `softwareupdate --install-rosetta --agree-to-license`.

### 2. Get the code and add your WiFi

```bash
git clone https://github.com/greencat667/energy-ghost.git
cd energy-ghost
cp energy_ghost/secrets.h.example energy_ghost/secrets.h
# edit energy_ghost/secrets.h: WiFi name and password; leave MANUAL_POSTCODE empty to auto-detect
```

`secrets.h` is git-ignored. Your WiFi password is stored in the firmware on the board, so don't share the compiled binary.

### 3. Flash

Plug the board in, then run:

```bash
./flash.sh
```

The script installs the ESP32 core and the Adafruit GFX library if needed, lists visible boards, asks for the port, then compiles and uploads. You can pass the port directly: `./flash.sh /dev/cu.usbmodem1101`.

The manual equivalent:

```bash
arduino-cli core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install "Adafruit GFX Library"
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashMode=qio,FlashSize=8M,PartitionScheme=default_8MB" energy_ghost
arduino-cli upload  --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashMode=qio,FlashSize=8M,PartitionScheme=default_8MB" -p <your-port> energy_ghost
```

Ports look like `/dev/cu.usbmodem...` on macOS, `/dev/ttyACM0` on Linux and `COM5` on Windows. On Linux you may need to join the `dialout` group.

### Download mode

The board sleeps between refreshes and **deep sleep switches off the USB port**. If `arduino-cli` can't find the port:

1. Unplug the USB cable.
2. Hold the **BOOT** button.
3. Plug the cable in while still holding BOOT.
4. Wait about 3 seconds, then release BOOT.
5. Run the flash step straight away.

If the port appears and then vanishes, run the upload again.

## Troubleshooting

| Symptom | Try |
|---|---|
| No serial port appears | Use a data-capable cable, plug in directly (not via a hub), use download mode. |
| "bad CPU type in executable" (macOS) | Install Rosetta (see above). |
| Port vanishes during upload | Run the upload again. |
| Stuck at "waiting for download" | Press a button or power-cycle, then upload; if sleep wins the race, hold the button rather than tapping. |
| Screen shows `?` ghost | No reading yet: check WiFi name/password (2.4 GHz only), then press a button. |
| Screen looks frozen | E-paper refreshes slowly; wait 5 seconds. |
| Wrong region | Set `MANUAL_POSTCODE`. |

## Status: what has and hasn't been tested

- The firmware **compiles** for the Waveshare board (see the compile figures below).
- The decision logic was smoke-tested against the live Carbon Intensity API, regional and national, using the optional Python relay (same logic as the firmware). The firmware's C++ port of it has not been separately verified.
- **The on-device WiFi, HTTPS and NTP path has been compiled but, as far as the author's log records, never run on real hardware.** Expect to debug it. Issues and fixes welcome.

## Things you should know

- **No TLS certificate checking.** The HTTPS calls use `setInsecure()`, so the server certificate is not validated. The device only sends GET requests for public numbers and sends no credentials, but a network attacker could feed it false data. A proper build would pin the CA root.
- **NTP** (`pool.ntp.org`) is used only to timestamp the forecast request; if it fails, the forecast lookahead is skipped.
- **JSON parsing is hand-rolled** (`indexOf` on known keys), not a real parser. It works for these specific responses only.
- **Carbon Intensity API** (https://carbonintensity.org.uk): free, no key. Run by the National Energy System Operator (NESO, formerly National Grid ESO); data under CC BY 4.0, so credit them if you republish figures. The device polls twice an hour.
- **ip-api.com** (https://ip-api.com): the free tier is **for non-commercial use only**, limited to 45 requests/minute per IP, and is **plain HTTP** (HTTPS needs a paid plan). The call sends nothing but your public IP (which it sees anyway) and your location comes back in the clear. Set `MANUAL_POSTCODE` to avoid it altogether. If you sell or deploy this commercially, drop the lookup or buy a plan.
- GB only: the Carbon Intensity API covers Great Britain.

## The ghost art

The six ghosts in `energy_ghost/ghost_art.h` were AI-generated locally with Draw Things (Stable Diffusion) from one shared style prompt, thresholded to 1-bit and baked in as 104x104 bitmaps. Draw Things is not needed to build or run the firmware. `tools/gen_ghost_art.py` is the optional generator (needs Pillow and Draw Things' API server; image generation is not deterministic, so rerunning it gives a fresh look rather than identical art)

## Optional test tool: the relay

`relay/energy_relay.py` is a stdlib-only Python server with the same decision logic, handy for testing rule changes without reflashing (`python3 relay/energy_relay.py`, port 8790, set `ENERGY_POSTCODE` to pin a location). The firmware does not use it.

## Make it your own

- **Thresholds and timing:** `energy_ghost/energy_config.h` (wake interval, lookahead, timeout).
- **Rules:** `decide_energy()` in `energy_ghost/carbon_api.cpp`.
- **Pins:** `energy_ghost/user_config.h`.

## Licence

MIT for this project's code (see `LICENSE`). Third-party code and data are listed in `NOTICE.md`.
