# SignalK WiFi Marine Gauge — Project Brief

> Handoff doc for Claude Code. Drop this in the project root as `CLAUDE.md`.
> It captures every decision, dead-end, and trap worked out in the planning phase so we don't re-derive them.

---

## Goal

A standalone 4" ESP32 touchscreen that displays live boat data (wind, heading, SOG, depth, etc.) pulled **over WiFi from a SignalK server** — no NMEA2000/CAN wiring. Powers and mounts anywhere with USB-C + WiFi. Phase 2: viewable from home over Tailscale.

---

## Build Ladder (current status)

- [x] **1. Board enumerates over USB** — currently `/dev/cu.usbmodem1101` on this Mac (was `…21201` on ShopMini; the suffix is per-machine/port, so check `ls /dev/cu.usbmodem*` each time)
- [x] **2. Toolchain can flash** — esptool read the chip cleanly, auto-reset works
- [x] **3. OUR code lights the panel** — ✅ **DONE 2026-06-13.** Built Waveshare V4 LVGL-widgets demo via PlatformIO (pioarduino core 3.3.2), flashed over USB. Panel lit, LVGL rendering on the PSRAM framebuffer, **GT911 touch reporting live coordinates over serial.** Build: `pio run -e display -t upload` from `firmware/`.
- [x] **4. Board joins WiFi** — ✅ **DONE 2026-06-13.** Joins SSID `bijoux` (lowercase — case-sensitive! "Bijoux" gave `WL_NO_SSID_AVAIL`). Got DHCP `192.168.8.32`. ESP32-S3 is 2.4GHz-only.
- [x] **5. Pulls live SignalK data** — ✅ **DONE 2026-06-13.** App connects `ws://192.168.8.96/signalk/v1/stream?subscribe=self`, parses delta JSON (ArduinoJson), updates LVGL labels. Confirmed `seenData=1` over serial heartbeat. Firmware: `firmware/` env `app`.
- [~] **6. Gauge layout** ← in progress (boat-health text panel rendering live values; richer dials/dashboard next)
- [ ] **Boat day:** mount + point at the real `openplot` server

---

## Hardware

**Board:** Waveshare ESP32-S3-Touch-LCD-4 (4", 480×480 capacitive touch).
> ✅ **VARIANT RESOLVED 2026-06-13:** plain `-4`, **PCB REV 4 (V4)** — confirmed by both the silkscreen ("REV 4") and the I2C scan. NOT the `-4B` (its QMI8658 IMU `0x6A/0x6B` and AXP2101 PMIC `0x34` are absent from the bus). → Use Waveshare's **V4** demo: `github.com/waveshareteam/ESP32-S3-Touch-LCD-4` (cloned to `vendor/waveshare-demo`).

- **MCU:** ESP32-S3 rev v0.2, dual-core LX7 @ 240 MHz
- **PSRAM:** 8 MB (required for the RGB framebuffer)
- **Flash:** 16 MB
- **USB:** native USB-Serial/JTAG — esptool auto-reset works (no BOOT-button dance)
- **MAC:** `58:e6:c5:72:3b:c0` (use for a DHCP reservation / finding it on the LAN)
- **Display driver:** ST7701S over **RGB parallel** interface
- **Touch:** GT911 over I2C — confirmed at `0x5D`
- **IO expander:** **TCA9554/PCA9554 family at `0x24`** (NOT CH422G — the brief's earlier guess was wrong). Drives LCD reset, touch reset, backlight. The `esp32_io_expander` library supports TCA95xx.
- **RTC:** PCF85063 present at `0x51`.
- **No IMU, no AXP2101 PMIC** (would be on the `-4B`). Battery charging on the `-4` is via an SW6106 (not on the scanned bus).
- **I2C topology (this rev):** single **merged** bus — **SDA = GPIO15, SCL = GPIO7**. The documented I2C0 pins (GPIO8/9) are empty, matching Waveshare's "newer revisions merge the two I2C channels" note. **Use GPIO15/7 for all I2C** (expander, touch, RTC).
- **Ports present:** USB-C, battery, CAN, RS485, I2C — **we only use USB-C** for now

---

## Toolchain Decision — READ THIS (don't repeat the dead-end)

**DECISION: Arduino framework via PlatformIO. NOT ESPHome.**

**Why NOT ESPHome:** The ST7701S init sequence is sent over SPI, and on this board those SPI lines run **through the IO expander**. ESPHome's software-SPI-through-port-expander does not actually work here — it compiles, but the display stays dark (open ESPHome bug #11748, late 2025). This board is also **not** in ESPHome's predefined RGB board list (only the Waveshare 4.3" and 7" are, and those are simpler no-init "RPI" panels).

**Why Arduino/PlatformIO:** Confirmed working on this exact board by others — panel lights, backlight controllable, GT911 touch reporting coordinates. The catch: it needs a **specific (older) version of the `esp32_io_expander` library** to compile. PlatformIO is chosen over Arduino IDE specifically to **pin library versions**.

> 🔧 **REALITY (2026-06-13, building the V4 demo):** The V4 demo does **NOT** use the ESP_Panel / `esp32_io_expander` stack the brief anticipated — it uses **Arduino_GFX (`GFX_Library_for_Arduino` 1.5.3) + LVGL 8.4.0 + SensorLib** (GT911). The ST7701 init SPI runs on **direct GPIO** (CS=42, SCK=2, MOSI=1), *not* through the expander — which is precisely why the ESPHome SPI-through-expander problem doesn't bite here. So the "old esp32_io_expander" worry is **moot for the V4 demo**. The expander (TCA9554 @0x24) is driven by two plain register writes (`0x02←0xFF`, `0x03←0x3A`) over I2C — no special library.
>
> 🔧 **TOOLCHAIN GOTCHA — platform version:** The V4 demo targets **Arduino core 3.3.2 / IDF 5.5.1**. The stock `platformio/espressif32 @ 7.0.1` ships core **3.0.17 / IDF 5.1**, whose `esp_lcd`/SPI structs lack fields Arduino_GFX 1.5.3 needs (`clock_source`, `refresh_on_demand`) → **compile errors**. Fix: use the **pioarduino** platform pinned to `55.03.32` (= Arduino 3.3.2). Set in `firmware/platformio.ini`.

---

## Step 3 Plan — Light the Panel

Start from **Waveshare's official demo for this exact board** (their tested bundle handles IO-expander init + ST7701S init + GT911). Get it to build & flash from our machine first — that proves we own the toolchain for this board — **then** fork it into our app. Do **not** hand-write RGB timings / pin maps from scratch.

- **Pin/timing reference (values only, if ever needed):** the Guition ESP32-S3-4848S040 is the same ST7701S 480×480 panel with a complete known-good config — but its GPIO pins differ from ours. Source our pins from Waveshare's demo/schematic.
- **Libraries (pin exact versions in `platformio.ini`):**
  - Waveshare `ESP32_Display_Panel` / `ESP_Panel` ecosystem
  - `esp32_io_expander` — **use the older version**; newer breaks the compile
  - LVGL (Waveshare demo targets 8.4)
  - LovyanGFX / ESP_Panel as the graphics layer
- **Flash:** `pio run -t upload --upload-port /dev/cu.usbmodem21201`
- **Serial monitor:** `pio device monitor -p /dev/cu.usbmodem21201 -b 115200`

---

## Data Architecture

SignalK is the data source (already running on `openplot`, the boat Pi, via OpenPlotter). Three ways to feed the gauge:

1. **MQTT — preferred for production.** `signalk-mqtt-push` plugin on openplot publishes selected paths to an MQTT broker; the ESP32 subscribes. Reuses the existing bilge-monitoring MQTT infra; clean scalar per topic. Requires installing the plugin **on openplot** (can't do from home).
2. **WebSocket — best for home dev.** ESP32 connects to the SignalK `ws://` stream and parses delta JSON. Only needs a reachable SignalK server — run one on **Forge** for home testing, or hit real openplot over Tailscale if the boat's online.
3. **UDP NMEA0183.** SignalK broadcasts NMEA0183 over UDP; ESP32 listens + parses sentences. Simplest transport, more parsing code. (Everything on the boat already receives data this way.)

**Plan:** develop at home over **WebSocket** (against Forge, or real openplot over Tailscale), switch to **MQTT** once at the boat / plugin installed. Architecturally it's just a server address — changing source = change one config line.

**Target SignalK paths — ✅ CONFIRMED live against openplot 2026-06-13:**

All the brief's planned paths exist on the real model. Units are **SI**, exactly as SignalK spec dictates:

| Path | Live? | Units (raw) | Convert for display |
|---|---|---|---|
| `environment.wind.angleApparent` | ✅ | **rad** (signed: − = port) | × 57.2958 → ° |
| `environment.wind.speedApparent` | ✅ | **m/s** | × 1.94384 → kn |
| `environment.wind.angleTrueWater` | ✅ | rad (signed) | → ° |
| `environment.wind.speedTrue` | ✅ | m/s | → kn |
| `navigation.headingMagnetic` | ✅ | rad | → ° |
| `navigation.headingTrue` | ✅ (bonus) | rad | → ° |
| `navigation.speedThroughWater` | ✅ (0 at dock) | m/s | → kn |
| `environment.depth.belowTransducer` | ✅ (2.47) | m | → ft |
| `environment.depth.belowKeel` / `belowSurface` | ✅ (bonus) | m | → ft |
| `navigation.speedOverGround` | ⚠️ **null** | m/s | needs GPS fix |
| `navigation.courseOverGroundTrue` | ⚠️ **null** | rad | needs GPS fix |
| `navigation.position` | ⚠️ **null** | lat/lon | needs GPS fix |

> ⚠️ **GPS is not fixed right now** (position/SOG/COG all null; `navigation.datetime` is 5 days stale). Boat is at the dock on shore power. **Two consequences:** (1) the SOG/COG gauges will read blank until there's a fix — handle null gracefully, don't show 0; (2) **the anchor-watch feature depends on `navigation.position`** — it can't work without a live GPS fix, so that's a hard prerequisite to verify before building it.

> 🔑 **Implementation rule: SignalK is all SI.** Angles = **radians**, speed = **m/s**, depth = **m**, temperature = **Kelvin**, tank/battery levels = **ratio (0–1)**. The gauge layer must convert (rad→°, m/s→kn, K→°C/°F, ratio→%). Wind/rudder angles are **signed** — preserve sign for port/starboard.

**Bonus data already flowing (great for the "boat health" panel):**
- **Victron/Cerbo:** `electrical.batteries.288.*` (voltage, current, power, `capacity.stateOfCharge`), `electrical.solar.278.*` (panel power, yield), `electrical.inverters.276.*`, `electrical.chargers.276.*`, `electrical.venus.*` (AC source = grid/shore).
- **Tanks:** `tanks.fuel.20` (DIESEL ~99%), `tanks.freshWater.21/22/23` (AFT/STARBOARD/PORT); names exposed under `electrical.tanks.20–23.name`.
- **Environment:** `environment.venus.28.*` (CABIN temp/humidity/pressure), `environment.water.temperature`, fridge/freezer temp.
- **Motion:** `navigation.attitude` (pitch/roll), `navigation.rateOfTurn`, `steering.rudderAngle`.

---

## Distributable Build — UI, Config Backend & WiFi Provisioning (planned 2026-06-13)

This is meant to be an **open-source project others flash onto their own boats** (repo `wsturner88/signalk-marine-gauge`, private until further along). That goal drives three pieces — `secrets.h` is **dev-only**; real users must never recompile.

1. **Swipeable multi-page UI.** LVGL `lv_tileview`, one tile per page, swipe horizontally. Page 1 = **wind dial** (`lv_meter` needle for apparent wind angle, speed in center), Page 2 = **boat status** (battery/depth/tanks/shore power). Requires GT911 **touch** wired into an LVGL input device (the first `app` build omitted touch — added when tileview lands).
2. **On-device config backend.** Web server on the ESP32 (candidate: `ESPAsyncWebServer` + `AsyncTCP`) serving a settings page at the gauge's LAN IP. Persist to **NVS** (`Preferences`) or LittleFS+JSON. Settings: SignalK host/port/path, per-page path selection, units, brightness, later anchor/alarm boundaries. This is the "make changes once it's live" backend + the Baileys-style WebUI.
3. **WiFi provisioning (no hardcoded creds).** Candidate: **WiFiManager** (tzapu) captive portal. On first boot / when it can't connect, the gauge starts its own AP (`SignalK-Gauge-XXXX`); user joins from a phone, picks their WiFi + enters password **and the SignalK server address**, saved to flash, reconnect. Same web stack as #2. A reset-to-AP gesture (long-press / boot button) lets it move between boats.

> Architecture note: #2 and #3 share the web-server stack; the UI (#1) just renders whatever the stored config points at. Keep the data source a single config value (server address) so MQTT-vs-WebSocket stays a one-line change (see Data Architecture).

---

## Remote Access (Phase 2) — Tailscale

**Goal:** see live boat data on a gauge at home.

**DECISION: Do NOT put Tailscale on the ESP32.** It's technically possible (MicroLink), but it fights the LVGL memory budget and the Arduino build. Keep the gauge a dumb local client.

**Design:** MQTT **broker bridge** over Tailscale — openplot's mosquitto ↔ a home mosquitto on Forge, bridged over their Tailscale IPs. The home gauge talks only to the local Forge broker; the bridge carries the boat topics. Auto-reconnecting, tolerant of flaky links.

**Caveat:** live data only flows when the boat is **powered AND online** (Starlink up, openplot running). The real at-home value is a "boat health" panel (battery, bilge, shore power, temp, position) more than sailing gauges.

---

## Home vs Boat Work Split

- **Steps 3 (panel), 4 (WiFi), 6 (gauge layout):** all doable at **home**.
- **Step 5 (real data):** the only step the boat helps with — but the laptop is on Tailscale, so real openplot data can be pulled from home **if the boat is online**.
- **Strategy:** do the fiddly bring-up at home; the boat trip becomes plug-in → point at real openplot → mount → watch real data. A victory lap, not a debug session in a cramped cabin.

---

## Machines

- **ShopMini** (Mac, workshop): where the planning-phase setup ran — esptool 5.3.0 and ESPHome 2026.5.3 installed (ESPHome now unused).
- **MacBook Pro:** intended build/boat machine. Needs Claude Code + PlatformIO installed there (Claude Code self-bootstraps pio; minimal setup since ESPHome is abandoned).
- **Forge** (home Mac Mini, Tailscale): home SignalK test server / future home MQTT broker + bridge endpoint.
- **openplot** (boat Raspberry Pi, Tailscale): the real SignalK server. Boat network `192.168.8.x` — see Boat Network & Devices below.
- **Networks:** Office `192.168.2.x`, Home `192.168.7.x`, Boat `192.168.8.x` — all on Tailscale.

---

## Boat Network & Devices — ⛵ BIJOUX (`192.168.8.x`)

> Fill in the blank IPs once on the boat / from the openplot LAN. Reserve DHCP leases so they're stable.

| Device | Role | IP | Notes |
|---|---|---|---|
| **openplot** (mDNS: `OpenPlot.local`) | RPi 4 (8GB) — OpenPlotter, SignalK, MQTT, Node-RED | **`192.168.8.96`** ✅ | The SignalK server. Also where `signalk-mqtt-push` / anchor-alarm plugins get installed. |
| **Victron Cerbo GX** | Battery/power monitoring — MQTT + Modbus-TCP on LAN | `192.168.8.___` | Named `Cerbo GX HQ2248XZ9UZ` in SignalK; its data already flows into openplot (see data model). Standalone MQTT (1883) **not** answering on openplot itself. |
| **Starlink** | Internet uplink | — | **CGNAT — no inbound port forwarding.** Remote access is **Tailscale only** (confirms the brief's Tailscale-bridge design). |
| **ESP32-S3 WiFi gauge** | Our display | serial `/dev/cu.usbmodem1101` | **Arrived early — in hand & connected 2026-06-13.** Chip confirmed via esptool: ESP32-S3 QFN56 rev v0.2, 8MB PSRAM, 16MB flash, MAC `58:e6:c5:72:3b:c0` (matches brief). Variant 4-vs-4B still needs a silkscreen check (chip-id can't tell). |

**SignalK server (confirmed live 2026-06-13):**
- HTTP API: `http://192.168.8.96/signalk/v1/api/` — **served on port 80, NOT the default 3000.**
- **WebSocket stream (home-dev transport): `ws://192.168.8.96/signalk/v1/stream`**
- Raw TCP (NMEA0183): `tcp://192.168.8.96:8375`; UDP NMEA0183 broadcast on `10110`.
- Server version **2.27.0**. No auth needed for read (open).
- **MQTT (1883) is closed on openplot** — the production MQTT plan needs a broker stood up / `signalk-mqtt-push` installed first. WebSocket works now; use it.

**Coming hardware:** **VM-3P75CT** energy meter → feeds the **Cerbo GX** (more power data into SignalK/MQTT downstream).

---

## Tools Installed So Far

- **esptool 5.3.0** — note: v5 dropped the `.py` suffix. Command is `esptool` or `python3 -m esptool`; `flash_id` is now `flash-id`.
- **ESPHome 2026.5.3** — installed but **NOT used** (see Toolchain Decision).

---

## Immediate Next Steps (for Claude Code)

1. **Confirm board variant** (4 vs 4B) and the IO-expander chip from the silkscreen/schematic.
2. **Pull Waveshare's official demo** for this exact board; identify the required library versions (the older `esp32_io_expander`!).
3. **Scaffold a PlatformIO project**; pin library versions in `platformio.ini`.
4. **Build + flash the demo** to confirm panel + touch work from our toolchain (ladder step 3).
5. **Fork into our app:** add WiFi join → SignalK WebSocket client + JSON parse → LVGL gauge.

---

## Open Questions / To Confirm

- ~~Board variant 4 vs 4B~~ ✅ **RESOLVED: plain `-4`, newer rev (V2+). Expander = TCA9554/PCA9554 @ 0x24 on merged I2C bus SDA15/SCL7. See Hardware.**
- ~~Is the boat powered + online now?~~ ✅ **Yes — on the boat LAN 2026-06-13; openplot live at `192.168.8.96`.**
- ~~Exact SignalK paths + units on openplot~~ ✅ **Confirmed — see Data Architecture table. All SI units; convert in gauge layer.**
- ~~Final transport~~ ✅ **WebSocket now (`ws://192.168.8.96/signalk/v1/stream`); MQTT later (broker not yet running on openplot).**
- First gauge: single round dial (wind/heading) vs multi-data dashboard
- **NEW — GPS fix:** position/SOG/COG are null at the dock. Confirm a live fix exists (boat underway, or GPS antenna) before relying on nav data or building anchor-watch.
- ✅ **RESOLVED — constant tone was a floating-pin buzzer (not backlight whine):** Waveshare's demo sets the expander (@0x24, TCA9554-style) config reg `0x03 = 0x3A`, which leaves pins **P1/P3/P4/P5 as floating INPUTS**. The onboard active buzzer hangs off one of P4/P5, and floating drove it → constant tone. **FIX (in `app`): config `0x03 = 0x00` (all pins outputs) + output `0x01 = 0xFF` (all high).** Backlight (P0) on, LCD/touch reset (P3/P1) released, buzzer pin driven HIGH = silent. Verified on-boat 2026-06-13. (Earlier "backlight whine / CH422G" diagnoses were wrong — the chip is a plain expander at 0x24 only; 0x23/0x38 NACK.)

---

## Future Feature Ideas (post-MVP)

### Anchor watch / anchor-riding monitor — *wanted*
A dedicated gauge mode showing how the boat is riding at anchor, tied back to SignalK. Use case: mount a gauge **up in the V-berth** so at night you can glance at how the boat is lying/swinging on the hook without going topside.

- **Likely needs a SignalK server-side app/plugin** to do the anchor-watch logic (drop point, swing radius, drift/drag alarm) — the gauge would then just subscribe to those computed paths. Candidate plugin to evaluate: **`signalk-anchoralarm-plugin`** (sets anchor position, computes range/bearing, raises a drag alarm; publishes paths under `navigation.anchor.*`).
- **SignalK paths to confirm:** `navigation.anchor.position`, `navigation.anchor.currentRadius`, `navigation.anchor.maxRadius`, plus `navigation.position`, `navigation.headingMagnetic`, and wind for context.
- **Gauge view ideas:** a top-down "boat-on-a-tether" plot (drop point + swing circle + current position track), current range vs. max-set radius, and a clear drag-alarm state. At night, a dim/red night-mode skin for the V-berth.
- **Ties into existing decisions:** this is exactly the kind of "boat health at rest" panel the Remote Access section already flags as the real at-home value. A second gauge unit in the V-berth fits the "it's just a server address" multi-client architecture — no extra wiring.
- **Open question:** does openplot already run an anchor-alarm plugin, or do we install one? (Plugin install is an at-boat / openplot task, like `signalk-mqtt-push`.)
- **🔔 Alarm output — the onboard buzzer (wanted, 2026-06-13):** the board has a dedicated, separately-controllable active buzzer (schematic "Bee" block, driven via the `EXIO_PWM` expander pin + SS8050 transistor) — *distinct* from the always-on backlight whine. It's silent by default. **Plan: drive it from on-device alarm logic** so the gauge can physically wake the skipper in the V-berth on: anchor drag / boundary breach, low battery SoC, high bilge, shore-power loss, etc. This makes the gauge a standalone monitor+alarm, not just a display — the boundaries can live on the ESP32 so it alarms even if SignalK/openplot hiccups (as long as data is flowing). **Buzzer control (learned 2026-06-13):** the buzzer is on expander pin **P4 or P5** (not yet narrowed); it is **active-low** — driving its pin HIGH = silent (current default state), LOW = sounds. **TODO:** 5-sec test to confirm which pin (drive P4 low, then P5 low, hear which beeps), then add `buzzer.on()/off()` + beep-cadence patterns per alarm type. No extra hardware needed — the onboard buzzer is the alarm output.

---

## Key References

- **Waveshare wiki:** ESP32-S3-Touch-LCD-4 / -4B
- **ESPHome ST7701S RGB driver** + Guition 4848S040 config (pin/timing reference only)
- **ESPHome bug #11748** (why ESPHome won't work on this board)
- **Arduino forum:** Waveshare ESP32-S3-Touch-LCD-4 working panel test (needs old esp32_io_expander)
- **SignalK display projects:**
  - `batuakan/ESPHomeSignalK` (WebSocket; ESPHome — not for us, but good path reference)
  - `timhughes/esp32-yacht-wind-instrument` (WebSocket, clean C++ reference, pluggable sources)
  - `Boatingwiththebaileys/Marine-Displays` (C, SignalK + N2K, has YouTube walkthroughs)
  - `FrancisSailor/NMEA2000-DISPLAY-on-ESP32S3` (N2K-direct — original inspiration)
- **SignalK plugins:** `signalk-mqtt-push` (preferred), `signalk-mqtt-gw`
- **MicroLink** (Tailscale-on-ESP32 — noted, deliberately NOT used)
