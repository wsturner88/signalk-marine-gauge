# ESP32 Marine Gauge — TODO

> Working list. Check things off as they land; add detail in commit messages / BOARD_REFERENCE.md.
> Status: 7 pages live (Wind, Speed+Depth, Status, Power, Anchor, Weather, System),
> display/touch issues resolved 2026-07-03, on-boat and responsive.

## 🚢 Make it distributable (others flash it onto their boats)

- [x] **WiFi provisioning — captive portal** (done 2026-07-03). Open AP `Gauge-XXXX` +
      wildcard DNS + phone portal (scan list, password, SignalK manual-or-auto); on-screen
      setup page with join-my-AP QR code; SignalK auto-discovery via mDNS (`_signalk-http`);
      config in NVS, secrets.h now only a dev seed. Entry: no config / SETUP button on
      System page (two-tap) / finger held at power-on / WiFi never up 45 s after boot.
      **Field-tested end-to-end 2026-07-03** (factory-fresh wipe -> QR -> portal -> mDNS
      found OpenPlot -> live data): "worked perfectly." secrets seed now opt-in
      (SEED_FROM_SECRETS). Still open: `secrets.h.example` + a bad-password message on
      the setup screen before repo goes public.
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
- [x] **Staleness watchdog** (done 2026-07-03). Anchored + no position/radius delta for
      30s -> status "STALE Ns" yellow with age counting, plot greys + STALE DATA stamp,
      double-chirp every 30s (drag pulse takes priority). Self-clears when data resumes.
- [ ] **Subscribe y2k's extra alarms:** `notifications.environment.wind.speed`,
      `wind.directionChange`, `ais.proximity` → distinct buzzer cadences + status lines.
- [ ] **`alert` notification state → yellow** (pre-warning stage before alarm/emergency).
- [ ] **Buttons → SignalK (PUT):** DROP / RAISE anchor via y2k, SILENCE ALARM (ack the
      notification). Makes the gauge a full anchor station, not just a display.
- [x] Top-down "boat on a tether" plot: drop point + swing circle + 15-min position
      trail + boat/heading + wind arrow, with stats column (done 2026-07-03).
- [ ] Decide y2k vs classic signalk-anchoralarm-plugin (leaning y2k: wind/AIS extras).
      Never run both at once.

## 🌤 Weather page

- [ ] **Outside temperature source.** Recommended: third RuuviTag (~$40, IP67) paired to
      the Cerbo — CABIN and Freezer sensors are already Ruuvis, so it's plug-and-play;
      map its path when it appears. (`environment.outside.temperature` already subscribed.)
- [ ] **Forecast feed — DECIDED 2026-07-03:** `signalk-virtual-weather-sensors` on
      openplot with the **Open-Meteo source (free, keyless)** — 30+ values as plain
      deltas on standard paths (environment.outside.*/wind.*/weather.*) + severe-weather
      notifications; flows through the home mirror automatically. Bill installs from the
      Appstore; then map the real paths onto the weather page (Outside card already
      subscribed and lights up untouched). Sanity-check its pressure vs the Ruuvi baro.
- [ ] Baro-fall buzzer alarm (e.g. ≤ −3 mb/3h while anchored).
- [ ] Persist the 24h pressure history across reboots (periodic NVS/LittleFS save).

## 🔔 Alarms & night use

- [ ] Alarm framework: per-alarm buzzer cadences + on-screen banner regardless of page
      (drag, wind, AIS, baro-fall, freezer-warm, low SoC, shore-power loss).
- [x] Display modes DAY / NIGHT / SLEEP (tap-to-wake 20s) / AUTO (sunrise-sunset from
      GPS pos+time), bottom-left cycle button, persisted to NVS, alarms force full
      brightness (done 2026-07-03). Still open: **red night skin** (night-vision), and
      per-mode brightness/timeout settings on the future web config page.
- [ ] Measure real current draw per mode (inline USB power meter or Victron DC-load
      delta) → power table for the README.
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
- [ ] Phase 2: home/work gauges over Tailscale — **plan upgraded from MQTT bridge to
      SignalK mirrors in Docker** (see `deploy/signalk-mirror/`): mserver at home pulls
      openplot upstream (THROTTLED — 2-3 GB/mo vs 15-25 unthrottled on 100 GB Starlink);
      work chains off the home mirror so the boat pays once; gauges mDNS-discover their
      local mirror. Needs: containers stood up + throttle measured + a 2nd gauge unit.
