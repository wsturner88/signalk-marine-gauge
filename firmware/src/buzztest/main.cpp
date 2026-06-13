// Buzzer-identification test (TCA9554-style expander @0x24, register-pointer protocol).
// Keeps P0 (backlight) HIGH the whole time so the SCREEN STAYS LIT, and toggles each
// OTHER output pin LOW one at a time. Watch for which TEST number goes SILENT while the
// screen is still on -> that pin drives the buzzer, and we can silence it independently.
// If NONE go silent with the screen lit, the buzzer shares P0 with the backlight (a
// hardware coupling, not software-fixable).
//
// Expander @0x24 regs: 0x01=output, 0x03=config(0=output). P1/P3 = TP_RST/LCD_RST kept
// HIGH in every state so touch + display stay up.

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "lv_conf.h"

Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 42, 2, 1, GFX_NOT_DEFINED);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 39, 38, 41, 46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9, 5, 45, 48, 47, 21,
  1, 10, 8, 50, 1, 10, 8, 20);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  480, 480, rgbpanel, 0, true, bus, GFX_NOT_DEFINED,
  st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

static lv_disp_draw_buf_t draw_buf;
static lv_obj_t *lbl;

static void disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full,
                          a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
  lv_disp_flush_ready(d);
}
static void tick_cb(void *) { lv_tick_inc(2); }
static void wrReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x24); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}

// reg 0x01 (output) values. bit0 (P0/backlight) = 1 in ALL states -> screen stays lit.
// bit1 (P1/TP_RST) and bit3 (P3/LCD_RST) = 1 in ALL states -> touch/display not reset.
struct Test { uint8_t out; const char *pin; };
static Test tests[] = {
  {0xFF, "all high (ref)"},  // 1
  {0xFB, "P2 low"},          // 2
  {0xBF, "P6 low"},          // 3
  {0x7F, "P7 low"},          // 4
  {0xCF, "P4+P5 low"},       // 5
};
static const int N = sizeof(tests) / sizeof(tests[0]);

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n=== BUZZER TEST v2 (P0 stays high) ===");

  Wire.begin(15, 7);
  // proven display init, then make all pins outputs and start all-high
  Wire.beginTransmission(0x24); Wire.write(0x02); Wire.write(0xff); Wire.endTransmission();
  Wire.beginTransmission(0x24); Wire.write(0x03); Wire.write(0x3a); Wire.endTransmission();
  Wire.beginTransmission(0x24); Wire.write(0x01); Wire.write(0xff); Wire.endTransmission();
  delay(120);
  wrReg(0x03, 0x00);  // all 8 pins = outputs
  wrReg(0x01, 0xFF);  // all high

  gfx->begin();
  uint32_t W = gfx->width(), H = gfx->height();
  lv_init();
  uint32_t bp = W * 40;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(bp * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(bp * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&draw_buf, b1, b2, bp);
  static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
  dd.hor_res = W; dd.ver_res = H; dd.flush_cb = disp_flush; dd.draw_buf = &draw_buf;
  lv_disp_drv_register(&dd);
  const esp_timer_create_args_t ta = {.callback = &tick_cb, .name = "t"};
  esp_timer_handle_t t; esp_timer_create(&ta, &t); esp_timer_start_periodic(t, 2000);

  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x102010), 0);
  lbl = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(lbl, "BUZZER TEST v2\nscreen should stay ON");
  lv_obj_center(lbl);
}

void loop() {
  static int idx = -1;
  static uint32_t last = 0;
  if (millis() - last > 3000) {
    last = millis();
    idx = (idx + 1) % N;
    wrReg(0x01, tests[idx].out);
    char buf[80];
    snprintf(buf, sizeof(buf), "TEST %d\n%s\nout=0x%02X\n\nsilent + lit?",
             idx + 1, tests[idx].pin, tests[idx].out);
    lv_label_set_text(lbl, buf);
    Serial.printf("TEST %d: out=0x%02X (%s)\n", idx + 1, tests[idx].out, tests[idx].pin);
  }
  lv_timer_handler();
  delay(5);
}
