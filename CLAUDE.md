# SignalK Marine Gauge — project guide for Claude Code

A standalone 4" ESP32-S3 touchscreen gauge that displays live boat data (wind,
depth, battery, tanks, anchor watch, weather) from any SignalK server over
WiFi. Zero-config onboarding: the gauge starts its own hotspot with a QR code,
takes WiFi credentials from a phone, and auto-discovers the SignalK server via
mDNS. No NMEA wiring — power (USB-C) + WiFi only.

> Maintainer-private operational notes (boat specifics, network details, live
> test targets) live in `BOAT_NOTES.private.md` — gitignored, never commit it.
> Claude sessions: read that file first if it exists.

## Hardware

Waveshare **ESP32-S3-Touch-LCD-4** (plain `-4`, REV 4): 480×480 ST7701S over
RGB parallel, GT911 touch, CH32V003 IO expander @0x24 (backlight PWM, buzzer,
LCD reset — quirks documented in `firmware/docs/BOARD_REFERENCE.md`, READ IT
before touching expander/display code), 8MB octal PSRAM, 16MB flash.
Merged I2C bus: SDA=GPIO15, SCL=GPIO7.

## Build

- PlatformIO with the **pioarduino** platform pinned to `55.03.32`
  (Arduino core 3.3.2 / IDF 5.5.1) — see `firmware/platformio.ini`.
- Main firmware: `pio run -e app -t upload` from `firmware/`.
- ⚠️ The build needs Waveshare's demo libraries cloned to
  `vendor/waveshare-demo` (github.com/waveshareteam/ESP32-S3-Touch-LCD-4) plus
  two hand-applied patches recorded in `firmware/patches/vendor-gfx-patches.md`.
  Making these self-applying is an open TODO.
- ⚠️ **Power-cycle after every flash** — a warm reset leaves the panel black.
- Dev-only: copy `firmware/src/app/secrets.h.example` to `secrets.h` and build
  with `-DSEED_FROM_SECRETS` to skip the onboarding portal.

## Architecture rules (hard-won — do not re-derive)

- **SignalK is the horsepower; the gauge is a dumb delta client.** All data
  arrives as delta JSON over one WebSocket (`sendSubscription()` in
  `firmware/src/app/main.cpp`). Server-side plugins do the heavy lifting
  (anchor alarm, weather feeds). Remote viewing = a mirror SignalK server
  (`deploy/signalk-mirror/`), never a VPN client on the ESP32.
- **SignalK units are SI** (radians, m/s, Kelvin, ratios) — convert only in
  the display layer. Wind/rudder angles are signed (negative = port).
- **Display scan-out slips under PSRAM pressure** (WiFi bursts, flash writes,
  blocking network calls). Any flash write or blocking op must be followed by
  `fbResync()`. Never retry network connections aggressively. The full story
  and the fix framework are in `BOARD_REFERENCE.md` and main.cpp comments.
- **Single framebuffer, partial mode, bounce buffer ON, PCLK 10 MHz** — the
  stable display config. Double-buffering is a proven dead end; do not retry.
- **Full-page objects must clear `LV_OBJ_FLAG_SCROLLABLE`** or they eat the
  page-flick gesture.
- Static dial artwork is pre-rendered once via `lv_snapshot` into PSRAM;
  labels/objects repaint only on real value changes (`setTxt`, change guards).
- **Silence must never look like calm**: data-driven pages age out (see the
  anchor staleness watchdog) — apply the same principle to new alarm surfaces.

## Repo map

- `firmware/src/app/` — the product (`main.cpp`, `portal.cpp` onboarding)
- `firmware/src/{scan,silence,buzztest,bltest}/` — hardware diagnostic mini-apps
- `firmware/docs/BOARD_REFERENCE.md` — board truth: expander registers,
  backlight PWM (INVERTED duty; never write 0), display recovery checklist
- `firmware/include/lv_conf.h` — LVGL 8.4 config (snapshot + qrcode enabled)
- `deploy/signalk-mirror/` — Docker kit for home/work mirror servers
- `TODO.md` — the roadmap; check items off as they land

## Working agreements

- Commit at every working checkpoint (this repo has lived in cloud-synced
  folders that can revert files).
- Flash → power-cycle → visual report is the test loop; there is no emulator.
- Keep the data source a single config value; transports must stay swappable.
