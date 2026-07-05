# Waveshare ESP32-S3-Touch-LCD-4 — board reference (everything we've learned)

Practical, hard-won notes for THIS board + our SignalK gauge firmware. If you do
one thing, read the ⚠️ gotchas first — they cost us the most time.

---

## ⚠️ Gotchas that will waste your time (read first)

1. **POWER-CYCLE after flashing before judging the display.** A warm reset from
   `pio run -t upload` (or the reset button) leaves the LCD panel in a stuck
   state and the screen comes up **black**. A true power cycle (unplug/replug or
   the power switch) fully resets the panel and it lights correctly. Black right
   after a flash is NORMAL — power-cycle, then look.
2. **The I2C IO-expander is a CH32V003, NOT a TCA9554.** The published schematic
   shows a TCA9554; the physical chip is different and has a different register
   map (see below). Driving it as a TCA9554 silently fails. Always trust a live
   I2C scan over the schematic.
3. **Serial reads from a Python script reset the board / come back empty.** The
   native USB-CDC re-enumerates on port open. To reliably see logs, open the port
   with **DTR asserted** (our capture scripts do `s.dtr = True`) or use
   `pio device monitor`. A plain `cat /dev/cu.usbmodem*` shows nothing.
4. **This repo lives in a OneDrive folder** — OneDrive can silently revert files
   (it has tried). Commit early and often.

---

## Board identity

- **Waveshare ESP32-S3-Touch-LCD-4**, 480×480, capacitive touch. Silkscreen **Rev4.0**.
- Module: **ESP32-S3-WROOM-1 N16R8** — 16 MB flash, 8 MB **OPI** PSRAM.
- Native USB-Serial/JTAG → `/dev/cu.usbmodem*` (no BOOT dance).
- Display driver: **ST7701S** over **RGB parallel** (+ ST7701 init over software SPI).
- Touch: **GT911** (I2C). RTC: **PCF85063** (I2C). Expander: **CH32V003** (I2C).

## Build / flash (PlatformIO)

- `firmware/` env **`app`** is the product. From `firmware/`:
  - Build: `pio run -e app`
  - Flash: `pio run -e app -t upload`  → **then power-cycle the board.**
  - Other envs: `scan` (I2C scanner), `display` (Waveshare LVGL demo — known-good
    reference for bringing up the panel).
- Platform: **pioarduino 55.03.32** (Arduino core 3.3.2 / IDF 5.5.1). Stock
  `platformio/espressif32` is too old (missing esp_lcd fields) — don't use it.
- `board_build.arduino.memory_type = qio_opi` (OPI PSRAM); USB-CDC-on-boot flags set.
- Building inside this OneDrive path (spaces + `&`) works fine for the Arduino build.

## I2C bus & devices

- **Single merged bus: SDA = GPIO15, SCL = GPIO7.** (The documented GPIO8/9 bus is
  empty on this rev.) Use 15/7 for everything.
- Live scan result (`pio run -e scan -t upload`): **`0x24`** (CH32V003 expander),
  **`0x51`** (PCF85063 RTC), **`0x5D`** (GT911 touch). Nothing at 0x20.

## CH32V003 IO expander @ 0x24 — THE key chip

A little RISC-V micro acting as a "smart" expander. Register map (from the ESPHome
`waveshare_io_ch32v003` component):

| Reg | Function | Notes |
|-----|----------|-------|
| 0x02 | Direction | **1 = output**, 0 = input |
| 0x03 | Output | **1 = high** |
| 0x04 | Input (read) | |
| 0x05 | Backlight PWM — **INVERTED duty** | low = bright, 255 = OFF, **0 = TRAP** |
| 0x06 | ADC (read) | |
| 0x07 | Interrupt status | |

**Required init (proven — exactly what the Waveshare demo does, lights the panel):**
```cpp
exReg(0x02, 0xFF);   // direction: all pins outputs
exReg(0x03, 0x3A);   // output: working display/backlight/reset state, buzzer off
```
- `0x3A` (not `0xFF`, not `0x0A`) is the magic output value. All-high (`0xFF`,
  the chip's power-on default) gives a black screen + buzzer screaming.
- Backlight is full-on by default (no 0x05 write needed for plain operation).
- **Backlight dimming (empirical, bltest 2026-07-03):** reg 0x05 duty is INVERTED —
  reg ~8-40 = full bright, 100 = medium, 200 = dim, **255 = backlight OFF
  (recoverable — write a lower value to relight)**. **NEVER write reg 0x05 = 0:**
  it latches the backlight dead until the expander loses power (full unplug).
  The app maps brightness v(0-255) -> reg = v==0 ? 255 : max(8, 255-v).
- **Output reg 0x03 bit map (empirical):** bit3 = **LCD RESET** (clearing it at
  runtime kills the panel until a full ST7701 re-init — never toggle); bit6 =
  buzzer; bits 0/1/2/4/5/7 showed no visible effect. The backlight is NOT on the
  output register — only the 0x05 PWM controls it.
- Diagnostic tool: `pio run -e bltest -t upload` — white screen + beep-counted
  register experiments (edit `src/bltest/main.cpp` candidates).
- Do this **before** `gfx->begin()` so the LCD/touch reset lines are released first.

## Buzzer

- **Active buzzer, enable on CH32V003 output bit 6 (active-HIGH).** No PWM/tone needed.
- On top of the `0x3A` display state: **silent = `0x3A`, beep = `0x7A`** (`0x3A | bit6`).
- Code: `static void buzzer(bool on){ exReg(0x03, on ? 0x7A : 0x3A); }`
- A real ALARM should pulse this (e.g., 200 ms on / 200 ms off patterns); a steady
  on is a continuous tone. Keep beeps non-blocking in the main loop for production.

## Display (ST7701 RGB)

- Arduino_GFX stack: `Arduino_SWSPI` (CS=42, SCK=2, MOSI=1) for the ST7701 init,
  `Arduino_ESP32RGBPanel` for the RGB pixels. Pin map + timings are in
  `app/main.cpp` (copied from the Waveshare demo — don't hand-edit).
- **Single framebuffer, partial mode** (`num_fbs=1`, `draw16bitRGBBitmap` flush).
  This is the correct, stable architecture. Double-buffering / `double_fb` /
  removing the bounce buffer all made things WORSE (blink, jiggle, flaky boot) —
  do not retry. See the dead-end notes in CLAUDE.md.
- **Bounce buffer ON** (`bounce_buffer_size_px = 40*w`, vendor patch) for reliable
  boot. Trade-off: the image can drift up under heavy WiFi after minutes.
- **Boot vertical offset fix:** one-shot `esp_lcd_rgb_panel_restart(panelHandle)`
  ~800 ms after boot (needs the `getPanelHandle()` vendor getter). Realigns the
  scan-out (top-wraps-to-bottom offset).
- Vendor patches (gitignored lib) are recorded in `firmware/patches/`.

## Touch (GT911)

- I2C, address probe **0x5D then 0x14**. Polled (no INT/RST pins used by the driver).
- Touch only inits if the CH32V003 reset lines are released — i.e. after the
  correct `0x03=0x3A` expander init. Heartbeat prints `touch=1` when OK.

## Connectivity (our app)

- WiFi STA → SignalK WebSocket (`ws://192.168.8.96/signalk/v1/stream`), delta JSON.
- Heartbeat on serial every 4 s: `[beat] up=.. wifi=.. sk=.. heap=.. touch=..`
  (`wifi=3` = connected, `sk=1` = SignalK WebSocket up). Use the DTR capture method to see it.

## Display issues — RESOLVED 2026-07-03 (keep the fixes!)

1. **Vertical offset / roll (~14–20 lines)** — FIXED, confirmed on-boat. Two causes,
   one mechanism: the bounce-buffer refill misses its deadline under peak PSRAM
   traffic (WiFi bursts, heavy redraws) and the scan-out slips circularly.
   Layered fix in `app/main.cpp`: (a) PCLK lowered 12→10 MHz (`prefer_speed` arg
   to `Arduino_ESP32RGBPanel`) for ~20% more refill headroom; (b) `fbResync()`
   (`esp_lcd_rgb_panel_restart`) fired 4x at boot, on every page-change settle,
   and every 2 min as a safety net. Serial logs `[fb] resync (boot|tile|periodic)`.
2. **Wind-page swipe sluggish / post-swipe shift** — FIXED, confirmed on-boat.
   Three stacked fixes:
   - Page changes are **flick gestures** (LVGL gesture, ~50 px) → instant
     `gotoTile()`; tileview finger-drag is disabled, so the view can never sit
     between tiles. NOTE: any full-page object must have `LV_OBJ_FLAG_SCROLLABLE`
     cleared or it claims the drag and the flick dies (this is why the wind page
     alone wouldn't flick — the meter kept the default flag).
   - Data-driven repaints pause while a finger is down (`touchDown`) so heavy
     redraws can't starve touch sampling mid-flick.
   - The wind dial's static content (ticks, arcs, degree labels, boat, captions)
     is **pre-rendered once at boot** into a PSRAM image via `lv_snapshot`
     (`LV_USE_SNAPSHOT 1` in `include/lv_conf.h`) — the live `lv_meter` is
     deleted after the snapshot. The rim arrow is a small ~55x55 object, and
     labels/compass only repaint on real changes. Steady-state render cost
     dropped ~100x. Boot serial logs `[wind] dial snapshot ok (460x460, ...)`.

## Quick recovery checklist if the screen is black
1. Did you **power-cycle** after the last flash? Do that first.
2. I2C scan shows `0x24/0x51/0x5D`? If not, check the bus.
3. Expander init is `0x02=0xFF` then `0x03=0x3A`? (Not 0xFF/0x0A on 0x03.)
4. As a reference, flash env `display` (Waveshare demo) + power-cycle — it should
   light. If it does, diff your app's display setup against it.
