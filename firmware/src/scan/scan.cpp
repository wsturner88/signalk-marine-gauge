// I2C bus scanner — board variant/revision probe for Waveshare ESP32-S3-Touch-LCD-4.
// Scans both documented I2C buses and prints responding addresses so we can identify
// which chips are populated (IO expander, touch, RTC, IMU, PMIC) without initializing
// the display. Safe to run on any board revision.
//
// Bus pinout from Waveshare wiki:
//   I2C0: SDA = GPIO8,  SCL = GPIO9   (drives the TCA9554/PCA9554 IO expander)
//   I2C1: SDA = GPIO15, SCL = GPIO7   (touch GT911 + other I2C slaves)
//
// Address cheat-sheet for interpreting results:
//   0x20-0x27  TCA9554/PCA9554 IO expander (strap-dependent; -4 uses this family)
//   0x14 / 0x5D  GT911 capacitive touch
//   0x51       PCF85063 RTC          (present on -4B / some revisions)
//   0x6A / 0x6B  QMI8658 IMU         (present on -4B)
//   0x34       AXP2101 PMIC          (present on -4B)

#include <Arduino.h>
#include <Wire.h>

static void scanBus(TwoWire &bus, const char *name, int sda, int scl) {
  bus.begin(sda, scl, 100000);
  Serial.printf("\n== %s  (SDA=%d, SCL=%d) ==\n", name, sda, scl);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    if (bus.endTransmission() == 0) {
      Serial.printf("   device @ 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.println("   (no devices responded)");
}

void setup() {
  Serial.begin(115200);
  delay(3000);  // give the USB CDC host time to reconnect after reset
  Serial.println("\n\n========== I2C VARIANT SCAN ==========");
  Serial.println("Board: Waveshare ESP32-S3-Touch-LCD-4");
}

void loop() {
  scanBus(Wire,  "I2C0 (expander bus)", 8, 9);
  scanBus(Wire1, "I2C1 (touch bus)",    15, 7);
  Serial.println("\n-------- scan complete, repeating in 5s --------");
  delay(5000);
}
