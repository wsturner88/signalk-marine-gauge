// Emergency buzzer-silence: drive every IO-expander pin LOW.
// The buzzer (EXIO_PWM via transistor) is ON by default when the expander pin is
// left unconfigured/high-Z. Forcing all expander outputs low pulls the transistor
// base low -> buzzer off. Backlight/LCD-reset also go low (screen dark) — fine, we
// just want silence here. Covers the buzzer bit whichever of P0..P7 it is.

#include <Arduino.h>
#include <Wire.h>

static void writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x24);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(15, 7);
  delay(50);
  // TCA9554/PCA9554 @0x24: reg0x03 = config (0=output), reg0x01 = output value.
  writeReg(0x03, 0x00);  // all pins = outputs
  writeReg(0x01, 0x00);  // all outputs = LOW  -> buzzer off
  Serial.println("Expander 0x24 driven all-LOW. Buzzer should be silent.");
}

void loop() {
  // Re-assert periodically in case of any glitch.
  writeReg(0x01, 0x00);
  delay(1000);
}
