# SignalK mirror — boat data at home / work over Tailscale

A gauge anywhere in the world is "just another SignalK client." The ESP32 can't
join Tailscale itself, so a Docker container on an always-on tailnet machine
(home: `mserver`, work: any Linux box) runs a local SignalK server that mirrors
the boat's server upstream. Gauges on that LAN auto-discover the mirror via
mDNS exactly like they discover openplot on the boat.

```
openplot (boat, source of truth)
   │  Tailscale WS, throttled subscription  (~2-3 GB/month)
   ▼
mserver mirror (home)  ──►  home gauges (mDNS auto-discover)
   │  Tailscale WS (home internet, unmetered)
   ▼
work mirror  ──►  desk gauge          ← chain from HOME, not the boat,
                                        so the boat pays bandwidth ONCE
```

## 1. Start the container

```bash
mkdir -p ~/signalk-mirror && cd ~/signalk-mirror
# copy docker-compose.yml here, then:
docker compose up -d
```

Open `http://<host>:3000`, create the admin account.

## 2. Connect it to the boat

Admin UI → **Server → Data Connections → Add**:

- Data Type: **SignalK**
- Host: `100.109.101.58`  (openplot's Tailscale IP)  Port: `80`, no TLS
- ID: `bijoux-upstream`

## 3. THROTTLE IT (do not skip — Starlink data budget)

Default subscriptions can pull every delta at native rates (heading alone can
be 10/s) — 15-25 GB/month. If the connection settings expose a subscription /
period control, use it; otherwise set subscription to `none` and add a custom
subscription. Target rates that match what gauges actually display:

| Paths | Period |
|---|---|
| wind angle/speed, heading | 1000 ms |
| position, STW, depth, attitude, rudder | 2000 ms |
| batteries, solar, inverter, venus.* | 5000 ms |
| tanks, temperatures, pressure, gusts | 10000 ms |
| navigation.anchor.*, notifications.* | 1000 ms |

(The firmware's `sendSubscription()` in `firmware/src/app/main.cpp` is the
reference path list.) After a day, check actual usage: `tailscale status` on
openplot shows per-peer tx/rx, and the Starlink app shows the meter. Tune until
boring — target is ~1 KB/s or less.

## 4. Checks

- **mDNS ON**: Server → Settings → mDNS enabled (this is what gauges discover).
  Verify from a Mac on the same LAN: `dns-sd -B _signalk-http._tcp local.`
- **Onboard a gauge**: power on → QR → join local WiFi → leave SignalK field
  EMPTY → it should find the mirror on its own (port 3000 comes via mDNS).
- **Boat offline behavior**: mirror stays up, data goes stale — the gauge's
  SignalK logo/staleness handling shows the truth. Live data requires the boat
  powered + Starlink up.

## Work location

Same compose file, but point the upstream connection at the HOME mirror
(`mserver` = `100.64.104.106`, port `3000`) instead of the boat — chaining
means the boat's metered link serves one consumer no matter how many
locations mirror it.
