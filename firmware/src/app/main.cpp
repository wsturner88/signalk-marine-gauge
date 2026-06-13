// SignalK Marine Gauge — boat-health panel (BIJOUX)
// WiFi -> SignalK WebSocket (delta JSON) -> LVGL text panel of live boat data.
//
// Hardware: Waveshare ESP32-S3-Touch-LCD-4 REV4. Display init (ST7701 RGB + TCA9554
// expander @0x24 + GT911) is lifted verbatim from the proven Waveshare V4 demo.
// SignalK values are SI (rad / m/s / m / K / ratio) — converted for display here.

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <lvgl.h>
#include "Arduino_GFX_Library.h"
#include "lv_conf.h"
#include "secrets.h"

// ---------- Display (from Waveshare V4 demo) ----------
Arduino_DataBus *bus = new Arduino_SWSPI(
  GFX_NOT_DEFINED /* DC */, 42 /* CS */, 2 /* SCK */, 1 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40 /* DE */, 39 /* VSYNC */, 38 /* HSYNC */, 41 /* PCLK */,
  46 /* R0 */, 3 /* R1 */, 8 /* R2 */, 18 /* R3 */, 17 /* R4 */,
  14 /* G0 */, 13 /* G1 */, 12 /* G2 */, 11 /* G3 */, 10 /* G4 */, 9 /* G5 */,
  5 /* B0 */, 45 /* B1 */, 48 /* B2 */, 47 /* B3 */, 21 /* B4 */,
  1, 10, 8, 50, 1, 10, 8, 20);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  480, 480, rgbpanel, 0 /* rotation */, true /* auto_flush */,
  bus, GFX_NOT_DEFINED, st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

static uint32_t W, H;
static lv_disp_draw_buf_t draw_buf;
#define TICK_MS 2

static void disp_flush(lv_disp_drv_t *d, const lv_area_t *a, lv_color_t *px) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full, w, h);
  lv_disp_flush_ready(d);
}
static void tick_cb(void *) { lv_tick_inc(TICK_MS); }

// ---------- Live values + UI rows ----------
// One row = label + value; value text updated as SignalK deltas arrive.
struct Metric {
  const char *path;     // SignalK path to match
  const char *label;    // row label
  const char *unit;     // displayed unit
  float scale;          // multiply raw SI value
  float offset;         // then add (for K->C etc.)
  int decimals;
  bool isText;          // value is a string, not a number
  lv_obj_t *valObj;     // LVGL value label (filled at build time)
  bool seen;
};

static Metric M[] = {
  {"environment.depth.belowTransducer",                 "Depth",      "ft",  3.28084f, 0,        1, false, nullptr, false},
  {"environment.wind.speedApparent",                    "Wind (A)",   "kn",  1.94384f, 0,        1, false, nullptr, false},
  {"environment.wind.angleApparent",                    "Wind ang",   "\xC2\xB0", 57.2958f, 0,   0, false, nullptr, false},
  {"navigation.speedThroughWater",                      "STW",        "kn",  1.94384f, 0,        1, false, nullptr, false},
  {"electrical.batteries.288.capacity.stateOfCharge",   "Battery",    "%",   100.0f,   0,        0, false, nullptr, false},
  {"electrical.batteries.288.voltage",                  "Batt V",     "V",   1.0f,     0,        2, false, nullptr, false},
  {"tanks.fuel.20.currentLevel",                        "Fuel",       "%",   100.0f,   0,        0, false, nullptr, false},
  {"environment.venus.28.temperature",                  "Cabin",      "\xC2\xB0""F", 1.8f,  -459.67f, 0, false, nullptr, false},
  {"electrical.venus.acSource",                         "Shore pwr",  "",    1.0f,     0,        0, true,  nullptr, false},
};
static const int N_METRIC = sizeof(M) / sizeof(M[0]);

static lv_obj_t *statusBar;

static void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x05080F), 0);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "BIJOUX");
  lv_obj_set_style_text_color(title, lv_color_hex(0x4FC3F7), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

  statusBar = lv_label_create(scr);
  lv_label_set_text(statusBar, "connecting WiFi...");
  lv_obj_set_style_text_color(statusBar, lv_color_hex(0xFFB300), 0);
  lv_obj_align(statusBar, LV_ALIGN_BOTTOM_MID, 0, -6);

  // Rows
  const int top = 40, rowH = (480 - top - 28) / N_METRIC;
  for (int i = 0; i < N_METRIC; i++) {
    int y = top + i * rowH;
    lv_obj_t *lab = lv_label_create(scr);
    lv_label_set_text(lab, M[i].label);
    lv_obj_set_style_text_color(lab, lv_color_hex(0x90A4AE), 0);
    lv_obj_align(lab, LV_ALIGN_TOP_LEFT, 24, y);

    lv_obj_t *val = lv_label_create(scr);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, lv_color_hex(0xECEFF1), 0);
    lv_obj_align(val, LV_ALIGN_TOP_RIGHT, -24, y);
    M[i].valObj = val;
  }
}

static void setStatus(const char *s, uint32_t color) {
  if (!statusBar) return;
  lv_label_set_text(statusBar, s);
  lv_obj_set_style_text_color(statusBar, lv_color_hex(color), 0);
}

// ---------- SignalK ----------
WebSocketsClient ws;

static void applyDelta(const char *path, JsonVariantConst value) {
  for (int i = 0; i < N_METRIC; i++) {
    if (strcmp(path, M[i].path) != 0) continue;
    char buf[40];
    if (M[i].isText) {
      const char *s = value.as<const char *>();
      snprintf(buf, sizeof(buf), "%s", s ? s : "--");
    } else if (value.isNull()) {
      snprintf(buf, sizeof(buf), "--");
    } else {
      float v = value.as<float>() * M[i].scale + M[i].offset;
      snprintf(buf, sizeof(buf), "%.*f %s", M[i].decimals, v, M[i].unit);
    }
    lv_label_set_text(M[i].valObj, buf);
    M[i].seen = true;
    return;
  }
}

static void onWsText(uint8_t *payload, size_t len) {
  // SignalK delta: {"updates":[{"values":[{"path":..,"value":..}, ...]}, ...]}
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  JsonArrayConst updates = doc["updates"].as<JsonArrayConst>();
  if (updates.isNull()) return;
  for (JsonObjectConst u : updates) {
    for (JsonObjectConst v : u["values"].as<JsonArrayConst>()) {
      const char *path = v["path"];
      if (path) applyDelta(path, v["value"]);
    }
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
  delay(1500);
  Serial.println("\n\n=== BIJOUX gauge boot ===");
  Serial.printf("free heap: %u, free internal DMA: %u\n",
                ESP.getFreeHeap(), heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

  // IO expander @0x24: release LCD reset + enable backlight (per V4 demo)...
  Wire.begin(15, 7);
  Wire.beginTransmission(0x24); Wire.write(0x02); Wire.write(0xff); Wire.endTransmission();
  Wire.beginTransmission(0x24); Wire.write(0x03); Wire.write(0x3a); Wire.endTransmission();
  // NOTE: EXIO0/P0 turned out to be the BACKLIGHT enable (driving it low blanks the
  // screen) and the audible "tone" tracks it 1:1 -> the noise is backlight-driver whine,
  // not a separate buzzer. So keep all outputs HIGH (backlight + LCD/touch reset on).
  // Whine mitigation (PCLK / brightness) is handled elsewhere, not by killing P0.
  Wire.beginTransmission(0x24); Wire.write(0x01); Wire.write(0xff); Wire.endTransmission();

  Serial.println("gfx->begin()...");
  if (!gfx->begin()) Serial.println("WARNING: gfx->begin() returned false");
  W = gfx->width(); H = gfx->height();
  Serial.printf("panel %ux%u\n", W, H);

  lv_init();
  // Partial draw buffers (40 lines each) in internal DMA RAM. Far smaller than the
  // demo's full-quarter buffers so they coexist with the WiFi/WebSocket stack.
  const uint32_t bufLines = 40;
  const uint32_t bufPx = W * bufLines;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  Serial.printf("lvgl buffers: b1=%p b2=%p (%u px each)\n", b1, b2, bufPx);
  if (!b1) { Serial.println("FATAL: LVGL buffer alloc failed"); }
  lv_disp_draw_buf_init(&draw_buf, b1, b2, bufPx);
  static lv_disp_drv_t drv;
  lv_disp_drv_init(&drv);
  drv.hor_res = W; drv.ver_res = H;
  drv.flush_cb = disp_flush;
  drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&drv);

  const esp_timer_create_args_t targs = {.callback = &tick_cb, .name = "lvgl_tick"};
  esp_timer_handle_t t;
  esp_timer_create(&targs, &t);
  esp_timer_start_periodic(t, TICK_MS * 1000);

  buildUI();
  Serial.println("UI built");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi.begin(\"%s\")...\n", WIFI_SSID);

  ws.begin(SK_HOST, SK_PORT, SK_PATH);
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(3000);
  Serial.printf("WS target: %s:%d%s\n", SK_HOST, SK_PORT, SK_PATH);
  Serial.println("setup done");
}

void loop() {
  static bool wifiUp = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiUp) {
      wifiUp = true;
      Serial.printf("WiFi connected, IP %s\n", WiFi.localIP().toString().c_str());
      setStatus("WiFi OK - linking SignalK", 0xFFB300);
    }
    ws.loop();
  } else {
    wifiUp = false;
  }
  lv_timer_handler();

  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 2000) {
    lastBeat = millis();
    Serial.printf("[beat] up=%lus wifi=%d ip=%s heap=%u seenData=%d\n",
                  millis() / 1000, WiFi.status(),
                  WiFi.localIP().toString().c_str(), ESP.getFreeHeap(),
                  M[0].seen || M[4].seen);
  }
  delay(5);
}
