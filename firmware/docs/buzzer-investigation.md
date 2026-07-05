# Buzzer / audio alarm — investigation log

Goal: make the onboard buzzer beep on demand so the gauge can sound alarms
(anchor drag, low battery, shore-power loss, etc.).

Status: ✅ **SOLVED 2026-06-14.** The expander @0x24 is a **CH32V003**, NOT a
TCA9554 — completely different register map. Buzzer = output reg `0x03`, **bit 6,
active-HIGH**. On top of the working display state `0x3A`: silent=`0x3A`,
beep=`0x7A`. Confirmed beeping on hardware with display + touch intact. Full
working details in **`firmware/docs/BOARD_REFERENCE.md`**. (Credit: a separate
research session + the Waveshare demo identified the CH32V003.)

Everything below is the historical investigation (why TCA9554 assumptions failed),
kept for the record.

---

## Hardware facts (CONFIRMED)

### From the schematic PDF (Waveshare ESP32-S3-Touch-LCD-4, Rev4.0)
The buzzer circuit ("Bee" block):
- **B1** = buzzer (powered from **VCC3V3**, switched to make sound).
- **Q1 = SS8050** (NPN). Base fed by **`BEE_EN`** through **R17 (1K)**, with
  **R18 (4.7K) pulldown** from base to GND → default OFF.
- **Q7 = DC008B** drives the buzzer from VCC3V3, controlled by Q1.
- Net analysis ⇒ **active-HIGH enable**: `BEE_EN` HIGH → Q1 on → buzzer on;
  `BEE_EN` LOW or floating → off (R18 pulldown).

### Expander + pin mapping (CONFIRMED by schematic + live I2C scan)
- Expander = **TCA9554PWR @ I2C 0x24**, on the merged bus **SDA=15, SCL=7**.
- I2C scan (env `scan`): bus 15/7 → `0x24`, `0x51` (RTC), `0x5D` (touch).
  Bus 8/9 → **nothing**. **There is NO device at 0x20, and 8/9 are empty.**
- EXIO→port mapping (from the TCA9554 pinout on the schematic):
  EXIO1=P0, EXIO2=P1, EXIO3=P2, EXIO4=P3, EXIO5=P4, EXIO6=P5, EXIO7=P6, EXIO8=P7.
- Function legend (from schematic net labels):
  - EXIO2 (P1) = **BL_EN** (backlight)
  - EXIO3 (P2) = **LCD_RST**
  - EXIO4 (P3) = SDCS
  - EXIO6 (P5) = **BEE_EN** ← **the buzzer enable**
  - EXIO7 (P6) = RTC_INT, EXIO8 (P7) = IRQ
- ⇒ **The buzzer is `BEE_EN` = EXIO6 = P5 = bit 5 of the 0x24 expander.**

### Known-good control facts
- Original "unstoppable tone" early in the project proves the buzzer CAN be
  loud → hardware/power are fine; it's a drive-signal problem.
- Our normal silent state: expander config `0x03 = 0x0A` (P1,P3 inputs; rest
  outputs), output `0x01 = 0xFF`.

---

## Things TRIED that did NOT work (all silent unless noted)

Pin numbers are TCA9554 port bits (P5 = bit5 = the buzzer per above).

| # | Test | Result |
|---|------|--------|
| 1 | Drive P4 / P5 HIGH (steady) | silent |
| 2 | Drive P4 / P5 LOW (steady) | silent |
| 3 | Square-wave toggle output reg on P4 / P5 (~1.5–6 kHz via I2C) | silent |
| 4 | Frequency sweep ~1.7–5 kHz on **both** P4 and P5 | silent |
| 5 | Sustained FLOAT (config=input) of P4, P5, and "most pins" (2 s) | silent |
| 6 | FLOAT ↔ drive-LOW config toggling (pull-up AC test) on P4/P5 | silent |
| 7 | Reproduce Waveshare init `0x02=0xFF, 0x03=0x3A` (floats P1,P3,P4,P5) | silent |
| 8 | Drive P1 HIGH, drive P3 HIGH (as outputs) | silent |
| 9 | User snippet: addr **0x20**, pins **8/9**, bit5 HIGH | N/A — that addr/pins **don't exist** on this board (scan); snippet is for a different Waveshare board |

### Notes / corrected assumptions
- ❌ Early theory "passive buzzer needs a clean PWM the I2C can't make" — the
  schematic shows an **active buzzer with a simple enable**, so steady HIGH on
  `BEE_EN` *should* sound it. It doesn't, which is the core puzzle.
- ❌ Early theory "floating pin makes the tone" — contradicted by R18 pulldown
  (float → pulled low → OFF). Float tests were indeed silent.
- ⚠️ I2C register **read-back is unreliable** here: `exRead()` (repeated-start)
  returned `cfg=0x00, out=0xAA, in=0xFF`, which is inconsistent with the working
  display (would imply LCD_RST held low). Do NOT trust read-back values; rely on
  observable behavior (screen/sound) instead.

---

## Current open diagnostic (in `main.cpp`, TEST page + auto-run ~3 s after boot)

`buzzDiag()`:
1. **Backlight blink ×3** via P1 (BL_EN) — a *visible* test of whether expander
   writes work at all. If the screen blinks, writes are fine and the problem is
   buzzer-specific; if not, the bug is in how we drive the expander.
2. Buzzer P5: HIGH 1 s, LOW 1 s, PWM ~2.7 kHz 1.5 s.

**Waiting to learn:** does the backlight blink (writes OK?) and does any drive
make sound?

---

## Leading hypotheses / next steps
- If backlight blinks but buzzer stays silent on P5: re-examine whether the
  enable polarity/stage is what we think (Q7 = "DC008B" is an unusual marking —
  verify it's a transistor and its type), or whether the buzzer needs a specific
  drive we haven't produced.
- If backlight does NOT blink: our expander writes aren't landing — fix the I2C
  write path (clock left at 400 kHz by a prior op? bus contention with touch?).
- Fallback if the onboard buzzer truly can't be driven: external buzzer on a
  broken-out pin (hardware add), or visual-only alarms.
