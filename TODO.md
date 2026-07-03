# ESP32 Marine Gauge — TODO

> Working list. Check things off as they land; add detail in commit messages / BOARD_REFERENCE.md.
> Status: 7 pages live (Wind, Speed+Depth, Status, Power, Anchor, Weather, System),
> display/touch issues resolved 2026-07-03, on-boat and responsive.

## 🚢 Make it distributable (others flash it onto their boats)

- [ ] **WiFi provisioning — captive portal.** On first boot (or long-press reset gesture),
      gauge starts its own AP (`SignalK-Gauge-XXXX`); user joins from a phone, a captive
      portal asks for their WiFi + SignalK server address; saved to NVS; reboot & connect.
      Candidate: WiFiManager (tzapu) or ESPAsyncWebServer + DNS captive portal.
      Kills `secrets.h` — no recompile for end users. **(biggest blocker for sharing)**
- [ ] **On-device settings page.** Web UI at the gauge's IP once on the network:
      SignalK host/port, units (ft/m, °F/°C, kn), brightness, alarm thresholds.
      Persist to NVS (`Preferences`). Same web stack as the captive portal.
- [ ] **Firmware install for non-developers.** GitHub release with prebuilt .bin +
      a browser-based flasher (ESP Web Tools / esptool-js on a GitHub Pages site) —
      plug in USB, click Install, no PlatformIO required.
- [ ] **OTA updates.** Once mounted, USB is awkward — ArduinoOTA or web-upload OTA
      so updates go over WiFi.
- [ ] **Vendor patches must self-apply.** `vendor/` is gitignored but the build needs two
      hand-edits to GFX_Library_for_Arduino (documented in `firmware/patches/`).
      Vendored copy in `firmware/lib/` or a pre-build patch script — a fresh clone must build.
- [ ] SignalK auth/token support (open servers work today; some boats lock theirs down).
- [ ] Make repo public once the above land.

## ⚓ Anchor page — make it a trustworthy V-berth alarm

- [ ] **Plugin-agnostic anchored detection** (y2k publishes `navigation.anchor.state`;
      the classic plugin signals via `navigation.anchor.position` existing — accept either).
- [ ] **Staleness watchdog.** If anchor/position data stops updating while anchored
      (GPS loss, plugin crash, WiFi drop), show a dim STALE warning + chirp. A silent
      failure must never look like "all is well" at 3am.
- [ ] **Subscribe y2k's extra alarms:** `notifications.environment.wind.speed`,
      `wind.directionChange`, `ais.proximity` → distinct buzzer cadences + status lines.
- [ ] **`alert` notification state → yellow** (pre-warning stage before alarm/emergency).
- [ ] **Buttons → SignalK (PUT):** DROP / RAISE anchor via y2k, SILENCE ALARM (ack the
      notification). Makes the gauge a full anchor station, not just a display.
- [ ] Top-down "boat on a tether" plot: drop point + swing circle + position track
      (feasible now that rendering is cheap).
- [ ] Decide y2k vs classic signalk-anchoralarm-plugin (leaning y2k: wind/AIS extras).
      Never run both at once.

## 🌤 Weather page

- [ ] **Outside temperature source.** Recommended: third RuuviTag (~$40, IP67) paired to
      the Cerbo — CABIN and Freezer sensors are already Ruuvis, so it's plug-and-play;
      map its path when it appears. (`environment.outside.temperature` already subscribed.)
- [ ] **Forecast feed:** install `@signalk/open-meteo-provider` on openplot (free, no key);
      gauge polls the v2 REST Weather API hourly → outside temp + forecast line.
      (Alt: signalk-openweather-plugin streams `environment.forecast.*` deltas, needs key.)
- [ ] Baro-fall buzzer alarm (e.g. ≤ −3 mb/3h while anchored).
- [ ] Persist the 24h pressure history across reboots (periodic NVS/LittleFS save).

## 🔔 Alarms & night use

- [ ] Alarm framework: per-alarm buzzer cadences + on-screen banner regardless of page
      (drag, wind, AIS, baro-fall, freezer-warm, low SoC, shore-power loss).
- [ ] **Night mode:** dim/red skin + backlight dimming (CH32V003 reg 0x05 = backlight PWM)
      — schedule or button. Essential for the V-berth.
- [ ] Buttons → SignalK PUTs for Cerbo relays (`electrical.switches.venus-0/1`) — deck /
      anchor light switching from the gauge.

## 📊 More data (confirmed live on BIJOUX)

- [ ] Heel & pitch (`navigation.attitude`) + rudder angle (`steering.rudderAngle`) — sailing page.
- [ ] Power page upgrades: shore draw W/V/limit (`inverters.276.acin.*`), charge stage
      (`chargers.276.chargingMode`), DC house load (`venus.dcPower`), battery time-remaining.
- [ ] Freezer-warm alarm threshold.
- [ ] Later (needs sailing): polar performance targets (`signalk-polar-performance-plugin`),
      race timer (`signalk-racer`).
- [ ] Sanity-clamp rules for bogus data (water temp reads 393 K / 120°C — never display).

## 🧰 Housekeeping / boat-side

- [ ] Fix SignalK depth offsets on openplot (belowSurface/belowKeel inconsistent with draft).
- [ ] DHCP reservation for the gauge (MAC `58:e6:c5:72:3b:c0`).
- [ ] Mount the gauge (USB-C power run).
- [ ] Water thermometer: find why MiniPlex emits 120°C (disconnected sensor / MTW misparse).
- [ ] Phase 2: home gauge over Tailscale (MQTT broker bridge per CLAUDE.md plan).
