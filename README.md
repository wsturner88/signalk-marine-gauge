# SignalK Marine Gauge

A standalone 4-inch touchscreen boat gauge that shows live SignalK data over
WiFi — wind, speed, depth, batteries, solar, tanks, anchor watch with a
top-down swing plot, and onboard weather with a barometer trend. Power it with
USB-C, onboard it from your phone in two minutes, and it finds your SignalK
server by itself.

**Status: working, actively developed, not yet release-ready.** It runs
day-and-night on a real boat, but a fresh clone does not build without manual
vendor-library patches (see below). Releases with a browser-based flasher are
planned.

## Highlights

- **Zero-config onboarding** — the gauge starts its own hotspot and shows a QR
  code; scan it with your phone, pick your boat WiFi, done. The SignalK server
  is discovered automatically via mDNS (works out of the box with a SignalK
  node server or a Victron GX device running Venus OS Large).
- **7 swipeable pages** — wind dial with rotating compass, speed+depth,
  boat status, power, **anchor watch** (drop point, swing circle, GPS trail,
  wind arrow, drag + staleness alarms on the onboard buzzer), **weather**
  (barometer with 3-hour trend and 24-hour history), system.
- **Display modes** — day / night-dim / sleep (touch to wake) / automatic at
  civil twilight computed from GPS position. Alarms force the screen on.
- **Remote gauges** — mirror your boat's SignalK server at home over a VPN
  (Tailscale) with the Docker kit in `deploy/signalk-mirror/`; a desk gauge
  shows the boat from anywhere.

## Hardware

Waveshare **ESP32-S3-Touch-LCD-4** (4", 480×480, capacitive touch, ~$35).
The board's undocumented quirks (CH32V003 expander registers, inverted
backlight PWM, RGB scan-out behavior) are mapped in
[`firmware/docs/BOARD_REFERENCE.md`](firmware/docs/BOARD_REFERENCE.md).

## Building (developers, for now)

1. PlatformIO. The platform/toolchain is pinned in
   [`firmware/platformio.ini`](firmware/platformio.ini).
2. Clone Waveshare's demo repo to `vendor/waveshare-demo`
   (github.com/waveshareteam/ESP32-S3-Touch-LCD-4) and apply the two patches in
   [`firmware/patches/`](firmware/patches/) — automation of this is TODO #1.
3. `cd firmware && pio run -e app -t upload`
4. **Power-cycle after flashing** (warm reset leaves the panel black), then
   onboard via the QR code.

## Architecture

The gauge is a thin SignalK delta-stream client; all intelligence lives
server-side (anchor alarm plugin, weather plugins, derived data). One config
value — the server address — and mDNS usually fills it in for you.

Roadmap: [`TODO.md`](TODO.md). License: [MIT](LICENSE).
