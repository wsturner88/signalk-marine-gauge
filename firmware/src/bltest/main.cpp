// Backlight bit hunt v2 — Waveshare ESP32-S3-Touch-LCD-4, CH32V003 @0x24.
//
// v1 lesson: an UNINITIALIZED panel glows too faintly to judge the backlight.
// v2 initializes the display and paints it WHITE with a huge step number, so
// backlight state is unmistakable: white screen = BL on, sudden black = BL cut.
//
// Reg 0x05 is a TRAP (any write latches the backlight dead until the expander
// loses power) — never written here. We hunt the BL bit in output reg 0x03:
//   draw "STEP n" -> n beeps -> candidate 0x03 value 5 s -> restore 0x3A 2.5 s.
//
// ⚠ Power-cycle after flashing (warm reset leaves the panel black).
#include <Arduino.h>
#include <Wire.h>
#include "Arduino_GFX_Library.h"

Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 42, 2, 1, GFX_NOT_DEFINED);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 39, 38, 41, 46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9, 5, 45, 48, 47, 21,
  1, 10, 8, 50, 1, 10, 8, 20,
  0, 10000000L);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  480, 480, rgbpanel, 0, true, bus, GFX_NOT_DEFINED,
  st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

static void exReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x24); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
static void beep(int n) {
  for (int i = 0; i < n; i++) {
    exReg(0x03, 0x3A | 0x40); delay(120);
    exReg(0x03, 0x3A);        delay(180);
  }
}
struct Cand { uint8_t v; const char *what; };
// v3 findings: NO output bit controls the backlight (bit3=LCD reset, bit6=buzzer,
// others no effect). v4: retry PWM reg 0x05 with NONZERO values only — the very
// first write ever made to it was 0, which may have latched it dead and poisoned
// every later observation. If this dims in steps, 0x05 works (just never write 0).
static const Cand cands[] = {
  {200, "pwm 200"}, {100, "pwm 100"}, {40, "pwm 40"}, {255, "pwm 255"},
};

void setup() {
  Serial.begin(115200); delay(600);
  Serial.println("\n=== backlight bit hunt v2 (white screen) ===");
  Wire.begin(15, 7);
  exReg(0x02, 0xFF);
  exReg(0x03, 0x3A);
  delay(200);
  gfx->begin();
  gfx->fillScreen(WHITE);
  delay(1500);
}

void loop() {
  for (unsigned i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
    gfx->fillScreen(WHITE);
    gfx->setTextColor(BLACK);
    gfx->setTextSize(8);
    gfx->setCursor(120, 200);
    gfx->printf("STEP %u", i + 1);
    Serial.printf("[step %u] 0x03=0x%02X (%s) for 5s\n", i + 1, cands[i].v, cands[i].what);
    beep(i + 1);
    delay(400);
    exReg(0x05, cands[i].v);   // v4: PWM duty, never zero
    delay(6000);
  }
  Serial.println("--- cycle repeats ---");
}
