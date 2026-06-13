// SignalK Marine Gauge — BIJOUX.  Swipeable multi-page LVGL UI over live SignalK.
//   Page 0: Apparent wind (rotating compass, boat outline, rim arrow, AWS, STW/TWS)
//   Page 1: Speed + Depth (two huge numbers)
//   Page 2: Boat status cards (battery / fuel / 3 water tanks / cabin)
//   Page 3: Power (battery SoC ring, charge/discharge box, solar, AC source)
// Footer: SignalK logo (left) + WiFi icon (right), green=connected / red=not.
// Swipe horizontally (GT911 touch). Data = SignalK delta JSON over WebSocket.

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

LV_FONT_DECLARE(montserrat_80);
LV_FONT_DECLARE(montserrat_150);
LV_IMG_DECLARE(sk_logo);
LV_IMG_DECLARE(boat_img);

#define C_BG     0x05080F
#define C_CARD   0x0E1620
#define C_ACCENT 0x4FC3F7
#define C_TEXT   0xECEFF1
#define C_DIM    0x78909C
#define C_GREEN  0x66BB6A
#define C_YELLOW 0xFFB300
#define C_RED    0xEF5350

// ---------------- Display ----------------
Arduino_DataBus *bus = new Arduino_SWSPI(GFX_NOT_DEFINED, 42, 2, 1, GFX_NOT_DEFINED);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 39, 38, 41, 46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9, 5, 45, 48, 47, 21,
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

// ---------------- Touch ----------------
TouchDrvGT911 touch;
static bool touchOK = false;
static int16_t tpx[5], tpy[5];
static void touch_read(lv_indev_drv_t *, lv_indev_data_t *data) {
  if (touchOK && touch.getPoint(tpx, tpy, 1) > 0) {
    data->state = LV_INDEV_STATE_PR; data->point.x = tpx[0]; data->point.y = tpy[0];
  } else data->state = LV_INDEV_STATE_REL;
}

// ---------------- Expander / buzzer ----------------
static void exReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x24); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
// Two candidate beep mechanisms (config 0x0A keeps P4/P5 as outputs):
static void beepDrive(int ms) {  // drive P4/P5 LOW (active-low buzzer), then back HIGH
  exReg(0x01, 0xCF); delay(ms); exReg(0x01, 0xFF);
}
static void beepFloat(int ms) {  // float P4/P5 (config 0x3A), then drive again
  exReg(0x03, 0x3A); delay(ms); exReg(0x03, 0x0A);
}

// ---------------- Live data ----------------
struct Boat {
  float windAngleApp = NAN, windSpeedApp = NAN;
  float windAngleAppAvg = NAN, windSpeedTrue = NAN;
  float heading = NAN;
  float depth = NAN, stw = NAN;
  float battSoc = NAN, battV = NAN, battA = NAN, battW = NAN;
  float fuel = NAN, cabinTemp = NAN;
  float waterPort = NAN, waterStbd = NAN, waterAft = NAN;
  float solarW = NAN, solarYield = NAN;
  char  shore[20] = "--";
} boat;
static bool dataDirty = false;

// ---------------- UI handles ----------------
#define NTILES 5
static lv_obj_t *tiles[NTILES], *pageDots, *skIcon, *wifiIcon;
// wind
static lv_obj_t *windMeter, *windArrow, *windSpeedLbl, *windAngleLbl, *stwLbl, *twsLbl;
static lv_obj_t *cardinals[8];
static lv_point_t arrowPts[4];
static const int  cardBearing[8] = {0, 45, 90, 135, 180, 225, 270, 315};
static const char *cardName[8]   = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
#define COMPASS_R 185           // cardinals near the rim, clear of the center content
#define MC 230.0f               // wind meter center (460/2)
// speed/depth
static lv_obj_t *sdSpeed, *sdDepth;
// status cards
static lv_obj_t *battBar, *battLbl, *fuelBar, *fuelLbl;
static lv_obj_t *wPortBar, *wPortLbl, *wStbdBar, *wStbdLbl, *wAftBar, *wAftLbl, *cabinLbl;
// power
static lv_obj_t *socArc, *socLbl, *battVLbl, *chgBox, *chgLbl, *solarLbl, *yieldLbl, *srcLbl;

static void setSk(bool ok)   { lv_obj_set_style_img_recolor(skIcon, lv_color_hex(ok ? C_GREEN : C_RED), 0); }
static void setWifi(bool ok) { lv_obj_set_style_text_color(wifiIcon, lv_color_hex(ok ? C_GREEN : C_RED), 0); }

// ---- helpers ----
static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, const char *title) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, w, h); lv_obj_set_pos(c, x, y);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_set_style_pad_all(c, 10, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = lv_label_create(c);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
  lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
  return c;
}
static lv_obj_t *bigVal(lv_obj_t *c, const lv_font_t *font) {
  lv_obj_t *v = lv_label_create(c);
  lv_label_set_text(v, "--");
  lv_obj_set_style_text_color(v, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(v, font, 0);
  lv_obj_align(v, LV_ALIGN_LEFT_MID, 0, -6);
  return v;
}
static lv_obj_t *makeBar(lv_obj_t *c, uint32_t color) {
  lv_obj_t *b = lv_bar_create(c);
  lv_obj_set_size(b, lv_pct(96), 12);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1E2A38), LV_PART_MAIN);
  lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(b, 6, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_INDICATOR);
  return b;
}
static void titleLabel(lv_obj_t *tile, const char *txt) {
  lv_obj_t *t = lv_label_create(tile);
  lv_label_set_text(t, txt);
  lv_obj_set_style_text_color(t, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);
}
static lv_obj_t *centerNum(lv_obj_t *parent, const lv_font_t *f, lv_align_t al, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, "--");
  lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(l, al, x, y);
  return l;
}
static lv_obj_t *dimLbl(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_align_t al, int x, int y) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_align(l, al, x, y);
  return l;
}

// ---- page 0: wind ----
static void buildWind(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  windMeter = lv_meter_create(tile);
  lv_obj_set_size(windMeter, 460, 460);
  lv_obj_center(windMeter);
  lv_obj_set_style_bg_color(windMeter, lv_color_hex(0x0A1018), 0);
  lv_obj_set_style_border_width(windMeter, 0, 0);
  lv_meter_scale_t *sc = lv_meter_add_scale(windMeter);
  lv_meter_set_scale_range(windMeter, sc, 0, 360, 360, 270);
  lv_meter_set_scale_ticks(windMeter, sc, 73, 1, 7, lv_color_hex(0x263038));
  lv_meter_set_scale_major_ticks(windMeter, sc, 18, 2, 13, lv_color_hex(0x52606D), 4);
  lv_meter_indicator_t *stbd = lv_meter_add_arc(windMeter, sc, 7, lv_color_hex(C_GREEN), 0);
  lv_meter_set_indicator_start_value(windMeter, stbd, 0);
  lv_meter_set_indicator_end_value(windMeter, stbd, 180);
  lv_meter_indicator_t *port = lv_meter_add_arc(windMeter, sc, 7, lv_color_hex(C_RED), 0);
  lv_meter_set_indicator_start_value(windMeter, port, 180);
  lv_meter_set_indicator_end_value(windMeter, port, 360);

  // smooth boat bow image, large, upper-center (recolored accent)
  lv_obj_t *boat = lv_img_create(windMeter);
  lv_img_set_src(boat, &boat_img);
  lv_obj_set_style_img_recolor(boat, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_img_recolor_opa(boat, LV_OPA_COVER, 0);
  lv_obj_align(boat, LV_ALIGN_CENTER, 0, -56);

  // wind arrow that clocks around the rim (set in updateUI)
  windArrow = lv_line_create(windMeter);
  lv_obj_set_size(windArrow, 460, 460); lv_obj_set_pos(windArrow, 0, 0);
  lv_obj_set_style_line_width(windArrow, 5, 0);
  lv_obj_set_style_line_color(windArrow, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_line_rounded(windArrow, true, 0);

  lv_obj_t *bow = lv_label_create(windMeter);
  lv_label_set_text(bow, LV_SYMBOL_UP);
  lv_obj_set_style_text_color(bow, lv_color_hex(C_ACCENT), 0);
  lv_obj_align(bow, LV_ALIGN_TOP_MID, 0, 6);

  for (int i = 0; i < 8; i++) {
    cardinals[i] = lv_label_create(windMeter);
    lv_label_set_text(cardinals[i], cardName[i]);
    bool major = (i % 2 == 0);
    lv_obj_set_style_text_color(cardinals[i], lv_color_hex(major ? C_TEXT : C_DIM), 0);
    lv_obj_set_style_text_font(cardinals[i], major ? &lv_font_montserrat_22 : &lv_font_montserrat_14, 0);
    lv_obj_align(cardinals[i], LV_ALIGN_CENTER, 0, -COMPASS_R);
  }
  // AWA + big AWS in the lower-center, below the boat (no overlap)
  windAngleLbl = centerNum(windMeter, &lv_font_montserrat_22, LV_ALIGN_CENTER, 0, 50);
  lv_obj_set_style_text_color(windAngleLbl, lv_color_hex(C_ACCENT), 0);
  windSpeedLbl = centerNum(windMeter, &montserrat_80, LV_ALIGN_CENTER, 0, 108);
  dimLbl(windMeter, "kts AWS", &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 160);

  stwLbl = centerNum(windMeter, &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, 6, 6);
  dimLbl(windMeter, "kn STW", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 6, 38);
  twsLbl = centerNum(windMeter, &lv_font_montserrat_28, LV_ALIGN_TOP_RIGHT, -6, 6);
  dimLbl(windMeter, "kn TWS", &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 38);
}

// ---- page 1: speed + depth ----
static void buildSpeedDepth(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  dimLbl(tile, "SPEED  (kn STW)", &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 28);
  sdSpeed = centerNum(tile, &montserrat_150, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_t *div = lv_obj_create(tile);
  lv_obj_set_size(div, 380, 2); lv_obj_align(div, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(div, lv_color_hex(0x1E2A38), 0);
  lv_obj_set_style_border_width(div, 0, 0);
  dimLbl(tile, "DEPTH  (ft)", &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 250);
  sdDepth = centerNum(tile, &montserrat_150, LV_ALIGN_TOP_MID, 0, 284);
}

// ---- page 2: status cards ----
static void buildStatus(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  titleLabel(tile, "BOAT STATUS");
  const int M = 14, top = 48, cw = (480 - 3 * M) / 2, ch = 122;
  lv_obj_t *cb = card(tile, M, top, cw, ch, "Battery");
  battLbl = bigVal(cb, &lv_font_montserrat_28); battBar = makeBar(cb, C_GREEN);
  lv_obj_t *cf = card(tile, M * 2 + cw, top, cw, ch, "Fuel");
  fuelLbl = bigVal(cf, &lv_font_montserrat_28); fuelBar = makeBar(cf, C_ACCENT);
  int y1 = top + ch + M;
  lv_obj_t *cp = card(tile, M, y1, cw, ch, "Water PORT");
  wPortLbl = bigVal(cp, &lv_font_montserrat_28); wPortBar = makeBar(cp, C_ACCENT);
  lv_obj_t *cs = card(tile, M * 2 + cw, y1, cw, ch, "Water STBD");
  wStbdLbl = bigVal(cs, &lv_font_montserrat_28); wStbdBar = makeBar(cs, C_ACCENT);
  int y2 = y1 + ch + M;
  lv_obj_t *ca = card(tile, M, y2, cw, ch, "Water AFT");
  wAftLbl = bigVal(ca, &lv_font_montserrat_28); wAftBar = makeBar(ca, C_ACCENT);
  lv_obj_t *cc = card(tile, M * 2 + cw, y2, cw, ch, "Cabin");
  cabinLbl = bigVal(cc, &lv_font_montserrat_36);
}

// ---- page 3: power ----
static void buildPower(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  titleLabel(tile, "POWER");
  socArc = lv_arc_create(tile);
  lv_obj_set_size(socArc, 184, 184);
  lv_obj_align(socArc, LV_ALIGN_TOP_MID, 0, 36);
  lv_arc_set_rotation(socArc, 135);
  lv_arc_set_bg_angles(socArc, 0, 270);
  lv_arc_set_range(socArc, 0, 100);
  lv_arc_set_value(socArc, 0);
  lv_obj_remove_style(socArc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(socArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(socArc, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_width(socArc, 16, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(socArc, lv_color_hex(0x1E2A38), LV_PART_MAIN);
  lv_obj_set_style_arc_color(socArc, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
  socLbl = lv_label_create(tile);
  lv_label_set_text(socLbl, "--");
  lv_obj_set_style_text_color(socLbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(socLbl, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_align(socLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(socLbl, socArc, LV_ALIGN_CENTER, 0, -10);
  battVLbl = lv_label_create(tile);
  lv_label_set_text(battVLbl, "--");
  lv_obj_set_style_text_color(battVLbl, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_text_font(battVLbl, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(battVLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(battVLbl, socArc, LV_ALIGN_CENTER, 0, 28);

  chgBox = lv_obj_create(tile);
  lv_obj_set_size(chgBox, 92, 92);
  lv_obj_align(chgBox, LV_ALIGN_TOP_MID, 0, 226);
  lv_obj_set_style_radius(chgBox, 14, 0);
  lv_obj_set_style_border_width(chgBox, 0, 0);
  lv_obj_set_style_bg_color(chgBox, lv_color_hex(0x37474F), 0);
  lv_obj_clear_flag(chgBox, LV_OBJ_FLAG_SCROLLABLE);
  chgLbl = lv_label_create(chgBox);
  lv_label_set_text(chgLbl, "--");
  lv_obj_set_style_text_color(chgLbl, lv_color_hex(0x05080F), 0);
  lv_obj_set_style_text_font(chgLbl, &lv_font_montserrat_22, 0);
  lv_obj_center(chgLbl);

  const int M = 14, cw = (480 - 3 * M) / 2, ch = 96, y = 330;
  lv_obj_t *csol = card(tile, M, y, cw, ch, "Solar");
  solarLbl = bigVal(csol, &lv_font_montserrat_36); lv_obj_align(solarLbl, LV_ALIGN_LEFT_MID, 0, -2);
  yieldLbl = lv_label_create(csol);
  lv_label_set_text(yieldLbl, "--");
  lv_obj_set_style_text_color(yieldLbl, lv_color_hex(C_DIM), 0);
  lv_obj_set_style_text_font(yieldLbl, &lv_font_montserrat_16, 0);
  lv_obj_align(yieldLbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_t *csrc = card(tile, M * 2 + cw, y, cw, ch, "AC SOURCE");
  srcLbl = bigVal(csrc, &lv_font_montserrat_28); lv_obj_align(srcLbl, LV_ALIGN_LEFT_MID, 0, 0);
}

// ---- page 4: temporary test page (buzzer button) ----
static void beepBtnCb(lv_event_t *) { beepDrive(250); }
static void buildTest(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  titleLabel(tile, "TEST");
  lv_obj_t *btn = lv_btn_create(tile);
  lv_obj_set_size(btn, 230, 120);
  lv_obj_center(btn);
  lv_obj_set_style_bg_color(btn, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_radius(btn, 18, 0);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, LV_SYMBOL_VOLUME_MAX "  BEEP");
  lv_obj_set_style_text_color(l, lv_color_hex(0x05080F), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(btn, beepBtnCb, LV_EVENT_CLICKED, NULL);
  dimLbl(tile, "temporary buzzer test", &lv_font_montserrat_16, LV_ALIGN_BOTTOM_MID, 0, -60);
}

static void updateDots() {
  lv_obj_t *act = lv_tileview_get_tile_act(lv_obj_get_parent(tiles[0]));
  int idx = 0;
  for (int i = 0; i < NTILES; i++) if (act == tiles[i]) idx = i;
  char buf[120] = "";
  for (int i = 0; i < NTILES; i++) strcat(buf, i == idx ? "#4FC3F7 \xE2\x97\x8F# " : "#37474F \xE2\x97\x8F# ");
  lv_label_set_text(pageDots, buf);
}
static void onTile(lv_event_t *) { updateDots(); lv_obj_invalidate(lv_scr_act()); }

static void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_t *tv = lv_tileview_create(scr);
  lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(tv, LV_OPA_COVER, 0);
  for (int i = 0; i < NTILES; i++) tiles[i] = lv_tileview_add_tile(tv, i, 0, LV_DIR_HOR);
  buildWind(tiles[0]); buildSpeedDepth(tiles[1]); buildStatus(tiles[2]); buildPower(tiles[3]); buildTest(tiles[4]);
  lv_obj_add_event_cb(tv, onTile, LV_EVENT_VALUE_CHANGED, NULL);

  skIcon = lv_img_create(scr);
  lv_img_set_src(skIcon, &sk_logo);
  lv_obj_set_style_img_recolor_opa(skIcon, LV_OPA_COVER, 0);
  lv_obj_align(skIcon, LV_ALIGN_BOTTOM_LEFT, 8, -2);
  setSk(false);
  wifiIcon = lv_label_create(scr);
  lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_font(wifiIcon, &lv_font_montserrat_20, 0);
  lv_obj_align(wifiIcon, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
  setWifi(false);
  pageDots = lv_label_create(scr);
  lv_label_set_recolor(pageDots, true);
  lv_label_set_text(pageDots, "#4FC3F7 \xE2\x97\x8F# #37474F \xE2\x97\x8F# #37474F \xE2\x97\x8F# #37474F \xE2\x97\x8F# #37474F \xE2\x97\x8F# ");
  lv_obj_align(pageDots, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static uint32_t socColor(float soc) { return soc < 0.20f ? C_RED : (soc < 0.50f ? C_YELLOW : C_GREEN); }
static void setTank(lv_obj_t *bar, lv_obj_t *lbl, float ratio) {
  if (isnan(ratio)) return;
  char b[12]; int pct = (int)(ratio * 100 + 0.5f);
  snprintf(b, sizeof(b), "%d%%", pct); lv_label_set_text(lbl, b);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(pct < 15 ? C_RED : (pct < 30 ? C_YELLOW : C_ACCENT)), LV_PART_INDICATOR);
}

static void updateUI() {
  char b[40];
  // --- wind (page 0): rim arrow uses 3s-smoothed angle ---
  if (!isnan(boat.windAngleAppAvg)) {
    float deg = boat.windAngleAppAvg * 57.2958f;
    while (deg < 0) deg += 360; while (deg >= 360) deg -= 360;
    float th = deg * 0.0174533f;
    float dx = sinf(th), dy = -cosf(th), px = cosf(th), py = sinf(th);
    const float Ri = 198, Ro = 220, w = 12;  // outer rim, clear of compass labels
    arrowPts[0] = {(lv_coord_t)(MC + Ri * dx),            (lv_coord_t)(MC + Ri * dy)};            // inner tip
    arrowPts[1] = {(lv_coord_t)(MC + Ro * dx + w * px),   (lv_coord_t)(MC + Ro * dy + w * py)};   // base1
    arrowPts[2] = {(lv_coord_t)(MC + Ro * dx - w * px),   (lv_coord_t)(MC + Ro * dy - w * py)};   // base2
    arrowPts[3] = arrowPts[0];
    lv_line_set_points(windArrow, arrowPts, 4);
    int d = (int)(deg + 0.5f);
    bool stbd = (d > 0 && d < 180);
    lv_obj_set_style_line_color(windArrow, lv_color_hex(d == 0 || d == 180 ? C_ACCENT : (stbd ? C_GREEN : C_RED)), 0);
    const char *side = (d == 0 || d == 180) ? "" : (stbd ? " STBD" : " PORT");
    snprintf(b, sizeof(b), "%d\xC2\xB0%s", d <= 180 ? d : 360 - d, side);
    lv_label_set_text(windAngleLbl, b);
  }
  if (!isnan(boat.windSpeedApp))  { snprintf(b, sizeof(b), "%.1f", boat.windSpeedApp * 1.94384f);  lv_label_set_text(windSpeedLbl, b); }
  if (!isnan(boat.stw))           { snprintf(b, sizeof(b), "%.1f", boat.stw * 1.94384f);           lv_label_set_text(stwLbl, b); lv_label_set_text(sdSpeed, b); }
  if (!isnan(boat.windSpeedTrue)) { snprintf(b, sizeof(b), "%.1f", boat.windSpeedTrue * 1.94384f); lv_label_set_text(twsLbl, b); }
  {
    float hd = isnan(boat.heading) ? 0 : boat.heading * 57.2958f;
    for (int i = 0; i < 8; i++) {
      float th = (cardBearing[i] - hd) * 0.0174533f;
      lv_obj_align(cardinals[i], LV_ALIGN_CENTER, (int)(COMPASS_R * sinf(th) + 0.5f), (int)(-COMPASS_R * cosf(th) + 0.5f));
    }
  }
  // --- speed/depth (page 1) ---
  if (!isnan(boat.depth)) {
    snprintf(b, sizeof(b), "%.1f", boat.depth * 3.28084f); lv_label_set_text(sdDepth, b);
    lv_obj_set_style_text_color(sdDepth, lv_color_hex(boat.depth < 1.5f ? C_RED : (boat.depth < 3.0f ? C_YELLOW : C_TEXT)), 0);
  }
  // --- status cards (page 2) ---
  if (!isnan(boat.battSoc)) {
    snprintf(b, sizeof(b), "%d%%  %.1fV", (int)(boat.battSoc * 100 + 0.5f), isnan(boat.battV) ? 0 : boat.battV);
    lv_label_set_text(battLbl, b);
    lv_bar_set_value(battBar, (int)(boat.battSoc * 100 + 0.5f), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(battBar, lv_color_hex(socColor(boat.battSoc)), LV_PART_INDICATOR);
  }
  if (!isnan(boat.fuel)) { snprintf(b, sizeof(b), "%d%%", (int)(boat.fuel * 100 + 0.5f)); lv_label_set_text(fuelLbl, b); lv_bar_set_value(fuelBar, (int)(boat.fuel * 100 + 0.5f), LV_ANIM_OFF); }
  setTank(wPortBar, wPortLbl, boat.waterPort);
  setTank(wStbdBar, wStbdLbl, boat.waterStbd);
  setTank(wAftBar,  wAftLbl,  boat.waterAft);
  if (!isnan(boat.cabinTemp)) { snprintf(b, sizeof(b), "%d\xC2\xB0""F", (int)(boat.cabinTemp * 1.8f - 459.67f + 0.5f)); lv_label_set_text(cabinLbl, b); }
  // --- power (page 3) ---
  if (!isnan(boat.battSoc)) {
    int s = (int)(boat.battSoc * 100 + 0.5f);
    snprintf(b, sizeof(b), "%d%%", s); lv_label_set_text(socLbl, b);
    lv_obj_align_to(socLbl, socArc, LV_ALIGN_CENTER, 0, -10);
    lv_arc_set_value(socArc, s);
    lv_obj_set_style_arc_color(socArc, lv_color_hex(socColor(boat.battSoc)), LV_PART_INDICATOR);
  }
  if (!isnan(boat.battV)) { snprintf(b, sizeof(b), "%.2f V", boat.battV); lv_label_set_text(battVLbl, b); lv_obj_align_to(battVLbl, socArc, LV_ALIGN_CENTER, 0, 28); }
  if (!isnan(boat.battW)) {
    bool chg = boat.battW >= 0;
    snprintf(b, sizeof(b), "%dW", (int)fabsf(boat.battW));
    lv_label_set_text(chgLbl, b);
    lv_obj_set_style_bg_color(chgBox, lv_color_hex(chg ? C_GREEN : C_RED), 0);
  }
  if (!isnan(boat.solarW)) { snprintf(b, sizeof(b), "%d W", (int)boat.solarW); lv_label_set_text(solarLbl, b); }
  if (!isnan(boat.solarYield)) { snprintf(b, sizeof(b), "%.0f Wh today", boat.solarYield / 3600.0f); lv_label_set_text(yieldLbl, b); }
  // AC source: friendly name + icon, colored
  {
    const char *s = boat.shore;
    if (!strcmp(s, "grid"))           { lv_label_set_text(srcLbl, LV_SYMBOL_CHARGE " Shore");  lv_obj_set_style_text_color(srcLbl, lv_color_hex(C_GREEN), 0); }
    else if (!strcmp(s, "battery"))   { lv_label_set_text(srcLbl, LV_SYMBOL_BATTERY_FULL " Battery"); lv_obj_set_style_text_color(srcLbl, lv_color_hex(C_YELLOW), 0); }
    else if (!strcmp(s, "shore"))     { lv_label_set_text(srcLbl, LV_SYMBOL_CHARGE " Shore");  lv_obj_set_style_text_color(srcLbl, lv_color_hex(C_GREEN), 0); }
    else if (s[0] && strcmp(s, "--")) { lv_label_set_text(srcLbl, s); lv_obj_set_style_text_color(srcLbl, lv_color_hex(C_TEXT), 0); }
  }
}

// ---------------- SignalK ----------------
WebSocketsClient ws;
static void sendSubscription() {
  static const char *sub =
    "{\"context\":\"vessels.self\",\"subscribe\":["
    "{\"path\":\"environment.wind.angleApparent\",\"period\":400},"
    "{\"path\":\"environment.wind.speedApparent\",\"period\":400},"
    "{\"path\":\"environment.wind.speedTrue\",\"period\":400},"
    "{\"path\":\"navigation.headingMagnetic\",\"period\":500},"
    "{\"path\":\"environment.depth.belowTransducer\",\"period\":1000},"
    "{\"path\":\"navigation.speedThroughWater\",\"period\":1000},"
    "{\"path\":\"electrical.batteries.288.capacity.stateOfCharge\",\"period\":2000},"
    "{\"path\":\"electrical.batteries.288.voltage\",\"period\":2000},"
    "{\"path\":\"electrical.batteries.288.current\",\"period\":2000},"
    "{\"path\":\"electrical.batteries.288.power\",\"period\":2000},"
    "{\"path\":\"tanks.fuel.20.currentLevel\",\"period\":5000},"
    "{\"path\":\"tanks.freshWater.21.currentLevel\",\"period\":5000},"
    "{\"path\":\"tanks.freshWater.22.currentLevel\",\"period\":5000},"
    "{\"path\":\"tanks.freshWater.23.currentLevel\",\"period\":5000},"
    "{\"path\":\"environment.venus.28.temperature\",\"period\":5000},"
    "{\"path\":\"electrical.solar.278.panelPower\",\"period\":2000},"
    "{\"path\":\"electrical.solar.278.yieldToday\",\"period\":5000},"
    "{\"path\":\"electrical.venus.acSource\",\"period\":5000}"
    "]}";
  ws.sendTXT(sub);
}
static void applyDelta(const char *p, JsonVariantConst v) {
  if (v.isNull()) return;
  if      (!strcmp(p, "environment.wind.angleApparent")) {
    boat.windAngleApp = v.as<float>();
    static float es = NAN, ec = NAN;
    float s = sinf(boat.windAngleApp), c = cosf(boat.windAngleApp);
    if (isnan(es)) { es = s; ec = c; }
    else { const float A = 0.13f; es = A * s + (1 - A) * es; ec = A * c + (1 - A) * ec; }
    boat.windAngleAppAvg = atan2f(es, ec);
  }
  else if (!strcmp(p, "environment.wind.speedApparent")) boat.windSpeedApp = v.as<float>();
  else if (!strcmp(p, "environment.wind.speedTrue")) boat.windSpeedTrue = v.as<float>();
  else if (!strcmp(p, "navigation.headingMagnetic")) boat.heading = v.as<float>();
  else if (!strcmp(p, "environment.depth.belowTransducer")) boat.depth = v.as<float>();
  else if (!strcmp(p, "navigation.speedThroughWater")) boat.stw = v.as<float>();
  else if (!strcmp(p, "electrical.batteries.288.capacity.stateOfCharge")) boat.battSoc = v.as<float>();
  else if (!strcmp(p, "electrical.batteries.288.voltage")) boat.battV = v.as<float>();
  else if (!strcmp(p, "electrical.batteries.288.current")) boat.battA = v.as<float>();
  else if (!strcmp(p, "electrical.batteries.288.power")) boat.battW = v.as<float>();
  else if (!strcmp(p, "tanks.fuel.20.currentLevel")) boat.fuel = v.as<float>();
  else if (!strcmp(p, "tanks.freshWater.21.currentLevel")) boat.waterAft = v.as<float>();
  else if (!strcmp(p, "tanks.freshWater.22.currentLevel")) boat.waterStbd = v.as<float>();
  else if (!strcmp(p, "tanks.freshWater.23.currentLevel")) boat.waterPort = v.as<float>();
  else if (!strcmp(p, "environment.venus.28.temperature")) boat.cabinTemp = v.as<float>();
  else if (!strcmp(p, "electrical.solar.278.panelPower")) boat.solarW = v.as<float>();
  else if (!strcmp(p, "electrical.solar.278.yieldToday")) boat.solarYield = v.as<float>();
  else if (!strcmp(p, "electrical.venus.acSource")) { const char *s = v.as<const char *>(); if (s) strlcpy(boat.shore, s, sizeof(boat.shore)); }
  else return;
  dataDirty = true;
}
static void onWsText(uint8_t *payload, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  for (JsonObjectConst u : doc["updates"].as<JsonArrayConst>())
    for (JsonObjectConst val : u["values"].as<JsonArrayConst>()) {
      const char *p = val["path"]; if (p) applyDelta(p, val["value"]);
    }
}
static void wsEvent(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:    setSk(true);  sendSubscription(); break;
    case WStype_DISCONNECTED: setSk(false); break;
    case WStype_TEXT:         onWsText(payload, len); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200); delay(800);
  Serial.println("\n\n=== BIJOUX gauge boot ===");
  Wire.begin(15, 7);
  exReg(0x03, 0x0A);  // P1/P3 inputs (touch/LCD reset), rest outputs
  exReg(0x01, 0xFF);  // outputs high: backlight on, buzzer silent
  delay(200);
  uint8_t gtAddr = 0;
  for (uint8_t a : {(uint8_t)0x5D, (uint8_t)0x14}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { gtAddr = a; break; }
  }
  touch.setPins(-1, -1);
  if (gtAddr) touchOK = touch.begin(Wire, gtAddr, 15, 7);
  if (touchOK) touch.setMaxTouchPoint(1);
  Serial.printf("GT911 %s (addr 0x%02X)\n", touchOK ? "ok" : "FAILED", gtAddr);

  gfx->begin();
  W = gfx->width(); H = gfx->height();
  lv_init();
  uint32_t bp = W * 40;
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(bp * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(bp * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&draw_buf, b1, b2, bp);
  static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
  dd.hor_res = W; dd.ver_res = H; dd.flush_cb = disp_flush; dd.draw_buf = &draw_buf;
  lv_disp_drv_register(&dd);
  static lv_indev_drv_t id; lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER; id.read_cb = touch_read; lv_indev_drv_register(&id);
  const esp_timer_create_args_t ta = {.callback = &tick_cb, .name = "tick"};
  esp_timer_handle_t t; esp_timer_create(&ta, &t); esp_timer_start_periodic(t, TICK_MS * 1000);

  buildUI();
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  ws.begin(SK_HOST, SK_PORT, SK_PATH); ws.onEvent(wsEvent); ws.setReconnectInterval(3000);
  Serial.println("setup done");
}

void loop() {
  // boot beep diagnostic: try BOTH methods so we learn which one sounds the buzzer.
  // You should hear: [beep A = drive-low] ... pause ... [beep B = float]. Tell me which.
  static bool beeped = false;
  if (!beeped && millis() > 1200) { beeped = true; beepDrive(250); delay(500); beepFloat(250); }
  static bool up = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!up) { up = true; Serial.printf("WiFi IP %s\n", WiFi.localIP().toString().c_str()); setWifi(true); }
    ws.loop();
  } else if (up) { up = false; setWifi(false); }
  static uint32_t lastUI = 0;
  if (dataDirty && millis() - lastUI > 150) { lastUI = millis(); dataDirty = false; updateUI(); }
  lv_timer_handler();
  static uint32_t beat = 0;
  if (millis() - beat > 4000) { beat = millis(); Serial.printf("[beat] up=%lus wifi=%d heap=%u touch=%d\n", millis()/1000, WiFi.status(), ESP.getFreeHeap(), touchOK); }
  delay(5);
}
