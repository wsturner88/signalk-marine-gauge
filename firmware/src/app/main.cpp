// SignalK Marine Gauge — BIJOUX
// Swipeable multi-page LVGL UI over live SignalK (WebSocket delta JSON).
//   Page 0: Apparent wind dial (lv_meter needle + speed)
//   Page 1: Boat status (battery / depth / speed / fuel / cabin / shore power)
// Swipe horizontally (GT911 touch) to change pages.
//
// Hardware: Waveshare ESP32-S3-Touch-LCD-4 REV4. Display = ST7701 RGB (Arduino_GFX),
// touch = GT911 @0x5D, IO expander @0x24 (merged I2C bus SDA15/SCL7).
// SignalK values are SI (rad / m/s / m / K / ratio); converted at display time.

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "lv_conf.h"
#include "TouchDrvGT911.hpp"
#include "secrets.h"

// ---------------- Display (from Waveshare V4 demo) ----------------
Arduino_DataBus *bus = new Arduino_SWSPI(
  GFX_NOT_DEFINED, 42 /* CS */, 2 /* SCK */, 1 /* MOSI */, GFX_NOT_DEFINED);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 39, 38, 41,
  46, 3, 8, 18, 17,
  14, 13, 12, 11, 10, 9,
  5, 45, 48, 47, 21,
  1, 10, 8, 50, 1, 10, 8, 20);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  480, 480, rgbpanel, 0, true, bus, GFX_NOT_DEFINED,
  st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

static uint32_t W, H;
static lv_disp_draw_buf_t draw_buf;
#define TICK_MS 2

static void disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full,
                          a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
  lv_disp_flush_ready(d);
}
static void tick_cb(void *) { lv_tick_inc(TICK_MS); }

// ---------------- Touch (GT911) ----------------
TouchDrvGT911 touch;
static bool touchOK = false;
static int16_t tpx[5], tpy[5];

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (touchOK && touch.getPoint(tpx, tpy, 1) > 0) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = tpx[0];
    data->point.y = tpy[0];
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// ---------------- Live data model ----------------
struct Boat {
  float windAngleApp = NAN;  // rad (signed: - = port)
  float windSpeedApp = NAN;  // m/s
  float depth = NAN;         // m (below transducer)
  float stw = NAN;           // m/s
  float battSoc = NAN;       // ratio 0..1
  float battV = NAN;         // V
  float fuel = NAN;          // ratio 0..1
  float cabinTemp = NAN;     // K
  char  shore[20] = "--";
} boat;
static bool dataDirty = false;

// ---------------- UI objects ----------------
static lv_obj_t *tileview;
static lv_obj_t *statusBar, *pageDots;
// wind page
static lv_obj_t *windMeter, *windSpeedLbl, *windAngleLbl;
static lv_meter_indicator_t *windNeedle;
// status page value labels (order matches statusRows[])
enum { ST_BATT, ST_DEPTH, ST_STW, ST_FUEL, ST_CABIN, ST_SHORE, ST_COUNT };
static lv_obj_t *stVal[ST_COUNT];

static const char *statusLabels[ST_COUNT] = {
  "Battery", "Depth", "Speed", "Fuel", "Cabin", "Shore"
};

static void setStatus(const char *s, uint32_t color) {
  if (!statusBar) return;
  lv_label_set_text(statusBar, s);
  lv_obj_set_style_text_color(statusBar, lv_color_hex(color), 0);
}

// ---- build wind page ----
static void buildWindPage(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x05080F), 0);

  lv_obj_t *title = lv_label_create(tile);
  lv_label_set_text(title, "APPARENT WIND");
  lv_obj_set_style_text_color(title, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  windMeter = lv_meter_create(tile);
  lv_obj_set_size(windMeter, 400, 400);
  lv_obj_center(windMeter);
  lv_obj_set_style_bg_color(windMeter, lv_color_hex(0x0A1018), 0);
  lv_obj_set_style_border_width(windMeter, 0, 0);

  lv_meter_scale_t *scale = lv_meter_add_scale(windMeter);
  // 0 at top (bow), clockwise: 90=stbd, 180=stern, 270=port
  lv_meter_set_scale_range(windMeter, scale, 0, 360, 360, 270);
  lv_meter_set_scale_ticks(windMeter, scale, 37, 2, 10, lv_color_hex(0x37474F));
  lv_meter_set_scale_major_ticks(windMeter, scale, 3, 4, 16, lv_color_hex(0x90A4AE), 18);

  windNeedle = lv_meter_add_needle_line(windMeter, scale, 6, lv_color_hex(0xEF5350), -10);
  lv_meter_set_indicator_value(windMeter, windNeedle, 0);

  windSpeedLbl = lv_label_create(windMeter);
  lv_label_set_text(windSpeedLbl, "--");
  lv_obj_set_style_text_color(windSpeedLbl, lv_color_hex(0xECEFF1), 0);
  lv_obj_set_style_text_font(windSpeedLbl, &lv_font_montserrat_48, 0);
  lv_obj_align(windSpeedLbl, LV_ALIGN_CENTER, 0, -6);

  windAngleLbl = lv_label_create(windMeter);
  lv_label_set_text(windAngleLbl, "--");
  lv_obj_set_style_text_color(windAngleLbl, lv_color_hex(0x90A4AE), 0);
  lv_obj_set_style_text_font(windAngleLbl, &lv_font_montserrat_20, 0);
  lv_obj_align(windAngleLbl, LV_ALIGN_CENTER, 0, 40);
}

// ---- build status page ----
static void buildStatusPage(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x05080F), 0);

  lv_obj_t *title = lv_label_create(tile);
  lv_label_set_text(title, "BOAT STATUS");
  lv_obj_set_style_text_color(title, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  const int top = 56, rowH = (480 - top - 40) / ST_COUNT;
  for (int i = 0; i < ST_COUNT; i++) {
    int y = top + i * rowH;
    lv_obj_t *lab = lv_label_create(tile);
    lv_label_set_text(lab, statusLabels[i]);
    lv_obj_set_style_text_color(lab, lv_color_hex(0x78909C), 0);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_22, 0);
    lv_obj_align(lab, LV_ALIGN_TOP_LEFT, 30, y);

    lv_obj_t *val = lv_label_create(tile);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, lv_color_hex(0xECEFF1), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_28, 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -30, y - 4);
    stVal[i] = val;
  }
}

static void tileChanged(lv_event_t *e) {
  lv_obj_t *act = lv_tileview_get_tile_act(tileview);
  // page dots: ● for active
  uint32_t idx = (lv_obj_get_x(act) > 0) ? 1 : 0;
  lv_label_set_text(pageDots, idx == 0 ? "#4FC3F7 \xE2\x97\x8F# #37474F \xE2\x97\x8F#"
                                       : "#37474F \xE2\x97\x8F# #4FC3F7 \xE2\x97\x8F#");
}

static void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x05080F), 0);

  tileview = lv_tileview_create(scr);
  lv_obj_set_style_bg_color(tileview, lv_color_hex(0x05080F), 0);
  lv_obj_t *t0 = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_HOR);
  lv_obj_t *t1 = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_HOR);
  buildWindPage(t0);
  buildStatusPage(t1);
  lv_obj_add_event_cb(tileview, tileChanged, LV_EVENT_VALUE_CHANGED, NULL);

  // overlay: page dots + status (drawn on scr, fixed above tileview)
  pageDots = lv_label_create(scr);
  lv_label_set_recolor(pageDots, true);
  lv_label_set_text(pageDots, "#4FC3F7 \xE2\x97\x8F# #37474F \xE2\x97\x8F#");
  lv_obj_align(pageDots, LV_ALIGN_BOTTOM_MID, 0, -22);

  statusBar = lv_label_create(scr);
  lv_label_set_text(statusBar, "connecting WiFi...");
  lv_obj_set_style_text_color(statusBar, lv_color_hex(0xFFB300), 0);
  lv_obj_set_style_text_font(statusBar, &lv_font_montserrat_14, 0);
  lv_obj_align(statusBar, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void updateUI() {
  char buf[32];
  // Wind dial
  if (!isnan(boat.windAngleApp)) {
    float deg = boat.windAngleApp * 57.2958f;
    while (deg < 0) deg += 360;
    while (deg >= 360) deg -= 360;
    lv_meter_set_indicator_value(windMeter, windNeedle, (int)(deg + 0.5f));
    int disp = (int)(deg + 0.5f);
    const char *side = (disp == 0 || disp == 180) ? "" : (disp < 180 ? " STBD" : " PORT");
    int shown = disp <= 180 ? disp : 360 - disp;
    snprintf(buf, sizeof(buf), "%d\xC2\xB0%s", shown, side);
    lv_label_set_text(windAngleLbl, buf);
  }
  if (!isnan(boat.windSpeedApp)) {
    snprintf(buf, sizeof(buf), "%.1f", boat.windSpeedApp * 1.94384f);
    lv_label_set_text(windSpeedLbl, buf);
  }
  // Status page
  if (!isnan(boat.battSoc) || !isnan(boat.battV)) {
    char soc[12] = "--", v[12] = "--";
    if (!isnan(boat.battSoc)) snprintf(soc, sizeof(soc), "%d%%", (int)(boat.battSoc * 100 + 0.5f));
    if (!isnan(boat.battV)) snprintf(v, sizeof(v), "%.1fV", boat.battV);
    snprintf(buf, sizeof(buf), "%s  %s", soc, v);
    lv_label_set_text(stVal[ST_BATT], buf);
  }
  if (!isnan(boat.depth)) { snprintf(buf, sizeof(buf), "%.1f ft", boat.depth * 3.28084f); lv_label_set_text(stVal[ST_DEPTH], buf); }
  if (!isnan(boat.stw))   { snprintf(buf, sizeof(buf), "%.1f kn", boat.stw * 1.94384f);   lv_label_set_text(stVal[ST_STW], buf); }
  if (!isnan(boat.fuel))  { snprintf(buf, sizeof(buf), "%d%%", (int)(boat.fuel * 100 + 0.5f)); lv_label_set_text(stVal[ST_FUEL], buf); }
  if (!isnan(boat.cabinTemp)) { snprintf(buf, sizeof(buf), "%d\xC2\xB0""F", (int)(boat.cabinTemp * 1.8f - 459.67f + 0.5f)); lv_label_set_text(stVal[ST_CABIN], buf); }
  lv_label_set_text(stVal[ST_SHORE], boat.shore);
}

// ---------------- SignalK ----------------
WebSocketsClient ws;

static void applyDelta(const char *path, JsonVariantConst v) {
  if (v.isNull()) return;
  if      (!strcmp(path, "environment.wind.angleApparent")) boat.windAngleApp = v.as<float>();
  else if (!strcmp(path, "environment.wind.speedApparent")) boat.windSpeedApp = v.as<float>();
  else if (!strcmp(path, "environment.depth.belowTransducer")) boat.depth = v.as<float>();
  else if (!strcmp(path, "navigation.speedThroughWater")) boat.stw = v.as<float>();
  else if (!strcmp(path, "electrical.batteries.288.capacity.stateOfCharge")) boat.battSoc = v.as<float>();
  else if (!strcmp(path, "electrical.batteries.288.voltage")) boat.battV = v.as<float>();
  else if (!strcmp(path, "tanks.fuel.20.currentLevel")) boat.fuel = v.as<float>();
  else if (!strcmp(path, "environment.venus.28.temperature")) boat.cabinTemp = v.as<float>();
  else if (!strcmp(path, "electrical.venus.acSource")) { const char *s = v.as<const char *>(); if (s) strlcpy(boat.shore, s, sizeof(boat.shore)); }
  else return;
  dataDirty = true;
}

static void onWsText(uint8_t *payload, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  for (JsonObjectConst u : doc["updates"].as<JsonArrayConst>())
    for (JsonObjectConst val : u["values"].as<JsonArrayConst>()) {
      const char *path = val["path"];
      if (path) applyDelta(path, val["value"]);
    }
}

static void wsEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:    Serial.println("WS connected"); setStatus("SignalK connected", 0x66BB6A); break;
    case WStype_DISCONNECTED: Serial.println("WS disconnected"); setStatus("SignalK lost - retrying", 0xEF5350); break;
    case WStype_TEXT:         onWsText(payload, len); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n\n=== BIJOUX gauge boot ===");

  // IO expander @0x24 (TCA9554-style, register-pointer protocol).
  // ROOT CAUSE OF THE TONE: Waveshare's demo sets config reg (0x03) = 0x3A, which leaves
  // pins P1/P3/P4/P5 as floating INPUTS. One of those floating pins drives the onboard
  // buzzer -> constant tone. FIX: config = 0x00 (ALL pins outputs) + output (0x01) = 0xFF.
  // That drives every pin solidly HIGH: backlight (P0) on, LCD/touch reset (P3/P1)
  // released, and the buzzer pin no longer floats -> SILENT. (Verified on-boat 2026-06-13.)
  Wire.begin(15, 7);
  Wire.beginTransmission(0x24); Wire.write(0x03); Wire.write(0x00); Wire.endTransmission();  // config: all outputs
  Wire.beginTransmission(0x24); Wire.write(0x01); Wire.write(0xff); Wire.endTransmission();  // output: all high
  delay(120);  // let GT911 come out of reset before we probe it

  // GT911 touch (enables page swiping)
  touch.setPins(-1, -1);
  touchOK = touch.begin(Wire, 0x5D, 15, 7);
  if (touchOK) { touch.setMaxTouchPoint(1); Serial.println("GT911 ok"); }
  else Serial.println("GT911 init FAILED (no swipe)");

  gfx->begin();
  W = gfx->width(); H = gfx->height();

  lv_init();
  const uint32_t bufPx = W * 40;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&draw_buf, b1, b2, bufPx);

  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = W; dd.ver_res = H;
  dd.flush_cb = disp_flush;
  dd.draw_buf = &draw_buf;
  lv_disp_drv_register(&dd);

  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = touch_read;
  lv_indev_drv_register(&id);

  const esp_timer_create_args_t targs = {.callback = &tick_cb, .name = "tick"};
  esp_timer_handle_t t; esp_timer_create(&targs, &t);
  esp_timer_start_periodic(t, TICK_MS * 1000);

  buildUI();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  ws.begin(SK_HOST, SK_PORT, SK_PATH);
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(3000);
  Serial.println("setup done");
}

void loop() {
  static bool wifiUp = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) { wifiUp = true; Serial.printf("WiFi IP %s\n", WiFi.localIP().toString().c_str()); setStatus("WiFi OK - linking SignalK", 0xFFB300); }
    ws.loop();
  } else wifiUp = false;

  static uint32_t lastUI = 0;
  if (dataDirty && millis() - lastUI > 150) { lastUI = millis(); dataDirty = false; updateUI(); }

  lv_timer_handler();

  static uint32_t beat = 0;
  if (millis() - beat > 3000) {
    beat = millis();
    Serial.printf("[beat] up=%lus wifi=%d ip=%s heap=%u touch=%d\n",
                  millis() / 1000, WiFi.status(), WiFi.localIP().toString().c_str(),
                  ESP.getFreeHeap(), touchOK);
  }
  delay(5);
}
