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
// prefer_speed 10MHz (default with OPI PSRAM is 12MHz): each bounce-buffer refill
// gets ~20% more deadline, so scan-out slips (the rolled/shifted image) under
// WiFi + heavy redraw are less likely. Refresh drops ~42Hz -> ~35Hz (fine on LCD).
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  40, 39, 38, 41, 46, 3, 8, 18, 17, 14, 13, 12, 11, 10, 9, 5, 45, 48, 47, 21,
  1, 10, 8, 50, 1, 10, 8, 20,
  0 /*pclk_active_neg*/, 10000000L /*prefer_speed*/);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  480, 480, rgbpanel, 0, true, bus, GFX_NOT_DEFINED,
  st7701_type1_init_operations, sizeof(st7701_type1_init_operations));

static uint32_t W, H;
static lv_disp_draw_buf_t draw_buf;
static esp_lcd_panel_handle_t panelHandle = nullptr;
// Realign the RGB scan-out DMA to the frame start. Cures the circular offset
// (rolled image, top lines wrapped to the bottom) whenever the bounce-buffer
// refill missed a deadline and the picture slipped. Brief flicker per call.
static uint32_t lastResync = 0;
static void fbResync(const char *why) {
  if (!panelHandle) return;
  lastResync = millis();
  esp_lcd_rgb_panel_restart(panelHandle);
  Serial.printf("[fb] resync (%s) at %lu ms\n", why, lastResync);
}
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
static bool touchDown = false;  // gates data-driven redraws during a flick
static void touch_read(lv_indev_drv_t *, lv_indev_data_t *data) {
  if (touchOK && touch.getPoint(tpx, tpy, 1) > 0) {
    data->state = LV_INDEV_STATE_PR; data->point.x = tpx[0]; data->point.y = tpy[0];
  } else data->state = LV_INDEV_STATE_REL;
  touchDown = (data->state == LV_INDEV_STATE_PR);
}

// ---------------- Expander / buzzer ----------------
static void exReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(0x24); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
static uint8_t exRead(uint8_t reg) {
  Wire.beginTransmission(0x24); Wire.write(reg); Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)0x24, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0xEE;
}
// The expander @0x24 is a CH32V003 (NOT a TCA9554!). Registers:
//   0x02 = direction (1=output), 0x03 = output (1=high).
// PROVEN display init = Waveshare's own demo: dir 0x02=0xFF, output 0x03=0x3A
// (that demo lit this exact panel). Buzzer = bit 6 on top of that:
//   silent = 0x3A ; beep = 0x7A (0x3A | bit6).
static void expanderInit() {
  exReg(0x02, 0xFF);   // direction: all outputs (exactly the Waveshare demo)
  exReg(0x03, 0x3A);   // output: Waveshare demo's working display state; bit6 LOW (buzzer silent)
}
static void buzzer(bool on) { exReg(0x03, on ? 0x7A : 0x3A); }   // bit6 high = beep

// ---------------- Live data ----------------
struct Boat {
  float windAngleApp = NAN, windSpeedApp = NAN;
  float windAngleAppAvg = NAN, windSpeedTrue = NAN;
  float gust5m = NAN, gust1h = NAN;   // true wind gust, 5-min & 1-hr (m/s)
  float heading = NAN;
  float depth = NAN, stw = NAN;
  float anchorRadius = NAN, anchorMaxRadius = NAN;   // m (from y2k-anchor-alarm)
  char  anchorOn[8] = "";                            // navigation.anchor.state: "on"/"off"
  char  anchorState[16] = "";                        // notification state: normal/alert/alarm/emergency
  float battSoc = NAN, battV = NAN, battA = NAN, battW = NAN;
  float fuel = NAN, cabinTemp = NAN;
  float waterPort = NAN, waterStbd = NAN, waterAft = NAN;
  float solarW = NAN, solarYield = NAN;
  char  shore[20] = "--";
} boat;
static bool dataDirty = false;

// ---------------- UI handles ----------------
#define NTILES 6
static lv_obj_t *tiles[NTILES], *pageDots, *tileView;
static int curTile = 0;
// wind
static lv_obj_t *windDial, *windArrow, *windSpeedLbl, *windAngleLbl, *stwLbl, *twsLbl;
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
// system / connection page
static lv_obj_t *sysWifiState, *sysWifiInfo, *sysSkState, *sysSkInfo, *gust5Lbl, *gust1hLbl;
// anchor page
static lv_obj_t *anchorStateLbl, *anchorRangeLbl, *anchorBar;

static bool skUp = false;
static void setSk(bool ok) {
  skUp = ok;
  if (!sysSkState) return;
  lv_label_set_text(sysSkState, ok ? "Connected" : "Disconnected");
  lv_obj_set_style_text_color(sysSkState, lv_color_hex(ok ? C_GREEN : C_RED), 0);
}
static void setWifi(bool ok) {
  if (!sysWifiState) return;
  lv_label_set_text(sysWifiState, ok ? "Connected" : "Disconnected");
  lv_obj_set_style_text_color(sysWifiState, lv_color_hex(ok ? C_GREEN : C_RED), 0);
}

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
// The dial background (ticks, degree labels, port/stbd arcs, boat image, bow
// mark, unit captions) never changes at runtime, so it is rendered ONCE at
// boot — a throwaway lv_meter snapshotted into a PSRAM image — and shown as a
// plain lv_img. Repainting dial pixels is then a blit. The old live lv_meter
// re-ran all its tick/arc/label drawing on every wind update, because the
// full-dial 460x460 windArrow line invalidated the whole page ~7x/sec.
static lv_img_dsc_t windDialImg;
static void buildWind(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);

  // throwaway meter carrying all the STATIC content, exactly as the old live one
  lv_obj_t *m = lv_meter_create(tile);
  lv_obj_set_size(m, 460, 460);
  lv_obj_center(m);
  lv_obj_set_style_bg_color(m, lv_color_hex(0x0A1018), 0);
  lv_obj_set_style_border_width(m, 0, 0);
  lv_meter_scale_t *sc = lv_meter_add_scale(m);
  lv_meter_set_scale_range(m, sc, 0, 360, 360, 270);
  lv_meter_set_scale_ticks(m, sc, 73, 1, 7, lv_color_hex(0x263038));
  lv_meter_set_scale_major_ticks(m, sc, 18, 2, 13, lv_color_hex(0x52606D), 4);
  lv_meter_indicator_t *stbd = lv_meter_add_arc(m, sc, 7, lv_color_hex(C_GREEN), 0);
  lv_meter_set_indicator_start_value(m, stbd, 0);
  lv_meter_set_indicator_end_value(m, stbd, 180);
  lv_meter_indicator_t *port = lv_meter_add_arc(m, sc, 7, lv_color_hex(C_RED), 0);
  lv_meter_set_indicator_start_value(m, port, 180);
  lv_meter_set_indicator_end_value(m, port, 360);
  // smooth boat bow image, large, upper-center (recolored accent)
  lv_obj_t *boat = lv_img_create(m);
  lv_img_set_src(boat, &boat_img);
  lv_obj_set_style_img_recolor(boat, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_img_recolor_opa(boat, LV_OPA_COVER, 0);
  lv_obj_align(boat, LV_ALIGN_CENTER, 0, -56);
  lv_obj_t *bow = lv_label_create(m);
  lv_label_set_text(bow, LV_SYMBOL_UP);
  lv_obj_set_style_text_color(bow, lv_color_hex(C_ACCENT), 0);
  lv_obj_align(bow, LV_ALIGN_TOP_MID, 0, 6);
  dimLbl(m, "kts AWS", &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 160);
  dimLbl(m, "kn STW", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 6, 38);
  dimLbl(m, "kn TWS", &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 38);
  dimLbl(m, "GUST 5m", &lv_font_montserrat_14, LV_ALIGN_BOTTOM_LEFT, 6, -36);
  dimLbl(m, "GUST 1h", &lv_font_montserrat_14, LV_ALIGN_BOTTOM_RIGHT, -6, -36);

  // render once into PSRAM, then swap the live meter for a plain image
  lv_obj_update_layout(m);
  uint32_t sz = lv_snapshot_buf_size_needed(m, LV_IMG_CF_TRUE_COLOR_ALPHA);
  void *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  lv_res_t sr = buf ? lv_snapshot_take_to_buf(m, LV_IMG_CF_TRUE_COLOR_ALPHA, &windDialImg, buf, sz)
                    : LV_RES_INV;
  Serial.printf("[wind] dial snapshot %s (%ux%u, %u bytes)\n", sr == LV_RES_OK ? "ok" : "FAILED",
                sr == LV_RES_OK ? windDialImg.header.w : 0, sr == LV_RES_OK ? windDialImg.header.h : 0, sz);
  lv_obj_t *dial;
  if (sr == LV_RES_OK) {
    lv_obj_del(m);
    windDial = lv_img_create(tile);
    lv_img_set_src(windDial, &windDialImg);
    lv_obj_center(windDial);
    dial = windDial;
  } else {
    dial = m;  // fall back to the live meter — page still works, just heavier
  }
  // Must not claim the page-flick: the compass labels overhang the edge, which
  // would otherwise make this object scrollable and eat the drag as a scroll.
  lv_obj_clear_flag(dial, LV_OBJ_FLAG_SCROLLABLE);

  // dynamic content over the pre-rendered dial
  // wind arrow that clocks around the rim — a SMALL object moved along the rim
  // (points set in updateUI), so each move invalidates ~55x55 px, not the dial
  windArrow = lv_line_create(dial);
  lv_obj_set_style_line_width(windArrow, 5, 0);
  lv_obj_set_style_line_color(windArrow, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_line_rounded(windArrow, true, 0);

  for (int i = 0; i < 8; i++) {
    cardinals[i] = lv_label_create(dial);
    lv_label_set_text(cardinals[i], cardName[i]);
    bool major = (i % 2 == 0);
    lv_obj_set_style_text_color(cardinals[i], lv_color_hex(major ? C_TEXT : C_DIM), 0);
    lv_obj_set_style_text_font(cardinals[i], major ? &lv_font_montserrat_22 : &lv_font_montserrat_14, 0);
    lv_obj_align(cardinals[i], LV_ALIGN_CENTER, 0, -COMPASS_R);
  }
  // AWA + big AWS in the lower-center, below the boat (no overlap)
  windAngleLbl = centerNum(dial, &lv_font_montserrat_22, LV_ALIGN_CENTER, 0, 50);
  lv_obj_set_style_text_color(windAngleLbl, lv_color_hex(C_ACCENT), 0);
  windSpeedLbl = centerNum(dial, &montserrat_80, LV_ALIGN_CENTER, 0, 108);
  stwLbl = centerNum(dial, &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, 6, 6);
  twsLbl = centerNum(dial, &lv_font_montserrat_28, LV_ALIGN_TOP_RIGHT, -6, 6);
  // true-wind gusts in the bottom corners (knots), 5-min (left) / 1-hr (right)
  gust5Lbl = centerNum(dial, &lv_font_montserrat_28, LV_ALIGN_BOTTOM_LEFT, 6, -6);
  lv_obj_set_style_text_color(gust5Lbl, lv_color_hex(C_YELLOW), 0);
  gust1hLbl = centerNum(dial, &lv_font_montserrat_28, LV_ALIGN_BOTTOM_RIGHT, -6, -6);
  lv_obj_set_style_text_color(gust1hLbl, lv_color_hex(C_YELLOW), 0);
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

// ---- page 4: anchor watch (y2k-anchor-alarm) ----
// Shows the anchor alarm state + swing range vs the max-set radius; the loop()
// sounds the buzzer when the plugin reports a drag. "test alarm" button confirms sound.
static void anchorTestCb(lv_event_t *) { buzzer(true); delay(400); buzzer(false); }
static void buildAnchor(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  titleLabel(tile, "ANCHOR");
  anchorStateLbl = centerNum(tile, &lv_font_montserrat_48, LV_ALIGN_CENTER, 0, -72);
  lv_label_set_text(anchorStateLbl, "NO ANCHOR");
  lv_obj_set_style_text_color(anchorStateLbl, lv_color_hex(C_DIM), 0);
  anchorRangeLbl = centerNum(tile, &lv_font_montserrat_36, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(anchorRangeLbl, "--");
  dimLbl(tile, "RANGE / MAX  (ft)", &lv_font_montserrat_16, LV_ALIGN_CENTER, 0, 40);
  anchorBar = lv_bar_create(tile);
  lv_obj_set_size(anchorBar, 380, 18);
  lv_obj_align(anchorBar, LV_ALIGN_CENTER, 0, 78);
  lv_bar_set_range(anchorBar, 0, 100);
  lv_obj_set_style_bg_color(anchorBar, lv_color_hex(0x1E2A38), LV_PART_MAIN);
  lv_obj_set_style_radius(anchorBar, 9, LV_PART_MAIN);
  lv_obj_set_style_radius(anchorBar, 9, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(anchorBar, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
  // discreet "test alarm" button to confirm the buzzer sound
  lv_obj_t *tb = lv_btn_create(tile);
  lv_obj_set_size(tb, 120, 38);
  lv_obj_align(tb, LV_ALIGN_BOTTOM_MID, 0, -48);
  lv_obj_set_style_bg_color(tb, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(tb, LV_OPA_60, 0);
  lv_obj_set_style_radius(tb, 10, 0);
  lv_obj_t *tl = lv_label_create(tb);
  lv_label_set_text(tl, "test alarm");
  lv_obj_set_style_text_color(tl, lv_color_hex(C_DIM), 0);
  lv_obj_center(tl);
  lv_obj_add_event_cb(tb, anchorTestCb, LV_EVENT_CLICKED, NULL);
}

// ---- page 5: system / connection (WiFi + SignalK status, gusts in corners) ----
static void buildSystem(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  titleLabel(tile, "SYSTEM");

  // WiFi card
  lv_obj_t *cw = card(tile, 24, 96, 432, 150, "WiFi");
  sysWifiState = lv_label_create(cw);
  lv_label_set_text(sysWifiState, "Disconnected");
  lv_obj_set_style_text_font(sysWifiState, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sysWifiState, lv_color_hex(C_RED), 0);
  lv_obj_align(sysWifiState, LV_ALIGN_TOP_LEFT, 0, 30);
  sysWifiInfo = lv_label_create(cw);
  lv_label_set_text(sysWifiInfo, "--");
  lv_obj_set_style_text_font(sysWifiInfo, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sysWifiInfo, lv_color_hex(C_DIM), 0);
  lv_obj_align(sysWifiInfo, LV_ALIGN_TOP_LEFT, 0, 72);

  // SignalK card
  lv_obj_t *cs = card(tile, 24, 262, 432, 150, "SignalK data");
  sysSkState = lv_label_create(cs);
  lv_label_set_text(sysSkState, "Disconnected");
  lv_obj_set_style_text_font(sysSkState, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sysSkState, lv_color_hex(C_RED), 0);
  lv_obj_align(sysSkState, LV_ALIGN_TOP_LEFT, 0, 30);
  sysSkInfo = lv_label_create(cs);
  lv_label_set_text(sysSkInfo, "--");
  lv_obj_set_style_text_font(sysSkInfo, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sysSkInfo, lv_color_hex(C_DIM), 0);
  lv_obj_align(sysSkInfo, LV_ALIGN_TOP_LEFT, 0, 72);
}

// refresh WiFi/SignalK detail text (called periodically + on connect)
static void updateSysInfo() {
  if (!sysWifiInfo) return;
  char b[80];
  if (WiFi.status() == WL_CONNECTED)
    snprintf(b, sizeof(b), "%s\n%s   %ld dBm", WIFI_SSID, WiFi.localIP().toString().c_str(), (long)WiFi.RSSI());
  else
    snprintf(b, sizeof(b), "%s\n(joining...)", WIFI_SSID);
  lv_label_set_text(sysWifiInfo, b);
  snprintf(b, sizeof(b), "%s : %d\n%s", SK_HOST, SK_PORT, SK_PATH);
  lv_label_set_text(sysSkInfo, b);
}

static void updateDots() {
  lv_obj_t *act = lv_tileview_get_tile_act(lv_obj_get_parent(tiles[0]));
  int idx = 0;
  for (int i = 0; i < NTILES; i++) if (act == tiles[i]) idx = i;
  char buf[120] = "";
  for (int i = 0; i < NTILES; i++) strcat(buf, i == idx ? "#4FC3F7 \xE2\x97\x8F# " : "#37474F \xE2\x97\x8F# ");
  lv_label_set_text(pageDots, buf);
}
// On a page change, schedule ONE clean repaint shortly after the swipe settles.
// The actual repaint runs in loop() (see tileRefreshPending) — never inside the
// scroll event, since heavy work there makes the swipe stutter and can hang.
static uint32_t tileChangedAt = 0;
static bool tileRefreshPending = false;
static void onTile(lv_event_t *) {
  lv_obj_t *act = lv_tileview_get_tile_act(tileView);
  for (int i = 0; i < NTILES; i++) if (act == tiles[i]) curTile = i;
  updateDots();
  lv_obj_invalidate(lv_scr_act());
  tileChangedAt = millis();
  tileRefreshPending = true;
}
// Instant page switch (no scroll animation -> no heavy frame-by-frame redraw of
// the wind page). Used by the on-screen ◀ ▶ nav buttons.
static void gotoTile(int idx) {
  if (idx < 0 || idx >= NTILES) return;
  curTile = idx;
  lv_obj_set_tile_id(tileView, idx, 0, LV_ANIM_OFF);
  updateDots();
  lv_obj_invalidate(lv_scr_act());
  // Schedule the settle repaint + scan-out resync ourselves — a programmatic
  // no-anim tile jump doesn't reliably fire the tileview's VALUE_CHANGED.
  tileChangedAt = millis();
  tileRefreshPending = true;
}
// Swipe = LVGL flick gesture (~50px), not a tileview drag. The wind page is too
// heavy to render frame-by-frame under a finger: the drag stuttered, needed most
// of the screen width to commit, and a swipe that snapped back never fired the
// settle handler — leaving the page shifted. A flick anywhere now just jumps.
static void onGesture(lv_event_t *) {
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (d == LV_DIR_LEFT)       gotoTile(curTile + 1);
  else if (d == LV_DIR_RIGHT) gotoTile(curTile - 1);
}
static void navPrevCb(lv_event_t *) { gotoTile(curTile - 1); }
static void navNextCb(lv_event_t *) { gotoTile(curTile + 1); }
static void navBtn(lv_obj_t *parent, const char *sym, int xoff, lv_event_cb_t cb) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, 54, 42);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, xoff, -2);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_60, 0);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, sym);
  lv_obj_set_style_text_color(l, lv_color_hex(C_ACCENT), 0);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
}

static void buildUI() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  tileView = lv_tileview_create(scr);
  lv_obj_set_style_bg_color(tileView, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(tileView, LV_OPA_COVER, 0);
  for (int i = 0; i < NTILES; i++) {
    tiles[i] = lv_tileview_add_tile(tileView, i, 0, LV_DIR_HOR);
    lv_obj_clear_flag(tiles[i], LV_OBJ_FLAG_SCROLLABLE);  // page content must never claim the flick
  }
  buildWind(tiles[0]); buildSpeedDepth(tiles[1]); buildStatus(tiles[2]); buildPower(tiles[3]);
  buildAnchor(tiles[4]); buildSystem(tiles[5]);
  lv_obj_add_event_cb(tileView, onTile, LV_EVENT_VALUE_CHANGED, NULL);
  // No finger-dragging of the tileview (see onGesture) — pages change only via
  // instant jumps, so a partial drag can never leave the view between tiles.
  lv_obj_clear_flag(tileView, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, onGesture, LV_EVENT_GESTURE, NULL);

  pageDots = lv_label_create(scr);
  lv_label_set_recolor(pageDots, true);
  lv_obj_align(pageDots, LV_ALIGN_BOTTOM_MID, 0, -8);
  updateDots();         // render the correct number of dots for NTILES
  navBtn(scr, LV_SYMBOL_LEFT, -120, navPrevCb);    // instant prev/next page (snappier than swipe)
  navBtn(scr, LV_SYMBOL_RIGHT, 120, navNextCb);
  setSk(false); setWifi(false);   // init the status labels on the system page
}

static uint32_t socColor(float soc) { return soc < 0.20f ? C_RED : (soc < 0.50f ? C_YELLOW : C_GREEN); }
static void setTank(lv_obj_t *bar, lv_obj_t *lbl, float ratio) {
  if (isnan(ratio)) return;
  char b[12]; int pct = (int)(ratio * 100 + 0.5f);
  snprintf(b, sizeof(b), "%d%%", pct); lv_label_set_text(lbl, b);
  lv_bar_set_value(bar, pct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(pct < 15 ? C_RED : (pct < 30 ? C_YELLOW : C_ACCENT)), LV_PART_INDICATOR);
}

// set a label only when the text actually changed — lv_label_set_text always
// invalidates (and redraws), even for identical text
static void setTxt(lv_obj_t *l, const char *s) {
  if (strcmp(lv_label_get_text(l), s)) lv_label_set_text(l, s);
}
static void updateUI() {
  char b[40];
  // --- wind (page 0): rim arrow uses ~1s-smoothed angle ---
  if (!isnan(boat.windAngleAppAvg)) {
    float deg = boat.windAngleAppAvg * 57.2958f;
    while (deg < 0) deg += 360; while (deg >= 360) deg -= 360;
    int d = (int)(deg + 0.5f); if (d == 360) d = 0;
    static int lastD = -1;
    if (d != lastD) {              // move only on whole-degree changes
      lastD = d;
      float th = d * 0.0174533f;
      float dx = sinf(th), dy = -cosf(th), px = cosf(th), py = sinf(th);
      const float Ri = 198, Ro = 220, w = 12;  // outer rim, clear of compass labels
      float X[3] = {MC + Ri * dx, MC + Ro * dx + w * px, MC + Ro * dx - w * px};   // tip, base1, base2
      float Y[3] = {MC + Ri * dy, MC + Ro * dy + w * py, MC + Ro * dy - w * py};
      // wrap the small line object tightly around the points (dial coords -> local)
      lv_coord_t x0 = (lv_coord_t)floorf(fminf(X[0], fminf(X[1], X[2]))) - 4;
      lv_coord_t y0 = (lv_coord_t)floorf(fminf(Y[0], fminf(Y[1], Y[2]))) - 4;
      lv_coord_t x1 = (lv_coord_t)ceilf(fmaxf(X[0], fmaxf(X[1], X[2]))) + 4;
      lv_coord_t y1 = (lv_coord_t)ceilf(fmaxf(Y[0], fmaxf(Y[1], Y[2]))) + 4;
      for (int i = 0; i < 3; i++) arrowPts[i] = {(lv_coord_t)(X[i] - x0), (lv_coord_t)(Y[i] - y0)};
      arrowPts[3] = arrowPts[0];
      lv_obj_set_pos(windArrow, x0, y0);
      lv_obj_set_size(windArrow, x1 - x0, y1 - y0);
      lv_line_set_points(windArrow, arrowPts, 4);
      bool stbd = (d > 0 && d < 180);
      lv_obj_set_style_line_color(windArrow, lv_color_hex(d == 0 || d == 180 ? C_ACCENT : (stbd ? C_GREEN : C_RED)), 0);
      const char *side = (d == 0 || d == 180) ? "" : (stbd ? " STBD" : " PORT");
      snprintf(b, sizeof(b), "%d\xC2\xB0%s", d <= 180 ? d : 360 - d, side);
      setTxt(windAngleLbl, b);
    }
  }
  if (!isnan(boat.windSpeedApp))  { snprintf(b, sizeof(b), "%.1f", boat.windSpeedApp * 1.94384f);  setTxt(windSpeedLbl, b); }
  if (!isnan(boat.stw))           { snprintf(b, sizeof(b), "%.1f", boat.stw * 1.94384f);           setTxt(stwLbl, b); setTxt(sdSpeed, b); }
  if (!isnan(boat.windSpeedTrue)) { snprintf(b, sizeof(b), "%.1f", boat.windSpeedTrue * 1.94384f); setTxt(twsLbl, b); }
  // --- gusts (wind page bottom corners) ---
  if (!isnan(boat.gust5m)) { snprintf(b, sizeof(b), "%.0f", boat.gust5m * 1.94384f); setTxt(gust5Lbl, b); }
  if (!isnan(boat.gust1h)) { snprintf(b, sizeof(b), "%.0f", boat.gust1h * 1.94384f); setTxt(gust1hLbl, b); }
  {
    float hd = isnan(boat.heading) ? 0 : boat.heading * 57.2958f;
    static int lastHdg = -1000;
    int hi = (int)(hd + 0.5f);
    if (hi != lastHdg) {           // realign the compass ring only when the heading moved
      lastHdg = hi;
      for (int i = 0; i < 8; i++) {
        float th = (cardBearing[i] - hd) * 0.0174533f;
        lv_obj_align(cardinals[i], LV_ALIGN_CENTER, (int)(COMPASS_R * sinf(th) + 0.5f), (int)(-COMPASS_R * cosf(th) + 0.5f));
      }
    }
  }
  // --- speed/depth (page 1) ---
  if (!isnan(boat.depth)) {
    snprintf(b, sizeof(b), "%.1f", boat.depth * 3.28084f); setTxt(sdDepth, b);
    uint32_t col = boat.depth < 1.5f ? C_RED : (boat.depth < 3.0f ? C_YELLOW : C_TEXT);
    static uint32_t lastCol = 0;
    if (col != lastCol) { lastCol = col; lv_obj_set_style_text_color(sdDepth, lv_color_hex(col), 0); }
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
  // --- anchor watch (page 4) ---
  {
    // Anchored = navigation.anchor.state "on". Dragging = currentRadius beyond
    // maxRadius (robust — doesn't depend on the notification streaming), OR the
    // notification escalating to alarm/emergency.
    bool radDrag = (!isnan(boat.anchorRadius) && !isnan(boat.anchorMaxRadius) && boat.anchorRadius > boat.anchorMaxRadius);
    bool drag = radDrag || !strcmp(boat.anchorState, "alarm") || !strcmp(boat.anchorState, "emergency");
    bool anchored = !strcmp(boat.anchorOn, "on");
    const char *txt; uint32_t col;
    if (drag)          { txt = "DRAGGING!"; col = C_RED; }
    else if (anchored) { txt = "ANCHORED";  col = C_GREEN; }
    else               { txt = "NO ANCHOR"; col = C_DIM; }
    lv_label_set_text(anchorStateLbl, txt);
    lv_obj_set_style_text_color(anchorStateLbl, lv_color_hex(col), 0);
    if (!isnan(boat.anchorMaxRadius)) {
      int cur = isnan(boat.anchorRadius) ? 0 : (int)(boat.anchorRadius * 3.28084f + 0.5f);
      int mx  = (int)(boat.anchorMaxRadius * 3.28084f + 0.5f);
      snprintf(b, sizeof(b), "%d / %d", cur, mx);
      lv_label_set_text(anchorRangeLbl, b);
      int pct = (mx > 0) ? (cur * 100 / mx) : 0; if (pct > 100) pct = 100;
      lv_bar_set_value(anchorBar, pct, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(anchorBar, lv_color_hex(col), LV_PART_INDICATOR);
    } else {
      lv_label_set_text(anchorRangeLbl, "--");
      lv_bar_set_value(anchorBar, 0, LV_ANIM_OFF);
    }
  }
}

// ---------------- SignalK ----------------
WebSocketsClient ws;
static void sendSubscription() {
  static const char *sub =
    "{\"context\":\"vessels.self\",\"subscribe\":["
    "{\"path\":\"environment.wind.angleApparent\",\"period\":250},"
    "{\"path\":\"environment.wind.speedApparent\",\"period\":250},"
    "{\"path\":\"environment.wind.speedTrue\",\"period\":400},"
    "{\"path\":\"environment.wind.fiveMinutes.gustTrue\",\"period\":5000},"
    "{\"path\":\"environment.wind.oneHour.gustTrue\",\"period\":10000},"
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
    "{\"path\":\"electrical.venus.acSource\",\"period\":5000},"
    "{\"path\":\"navigation.anchor.state\",\"period\":2000},"
    "{\"path\":\"navigation.anchor.currentRadius\",\"period\":1000},"
    "{\"path\":\"navigation.anchor.maxRadius\",\"period\":5000},"
    "{\"path\":\"notifications.navigation.anchor\",\"period\":1000}"
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
    // A=0.4 at the ~400ms wind update rate -> ~1s smoothing (was 0.13 = ~3s).
    else { const float A = 0.4f; es = A * s + (1 - A) * es; ec = A * c + (1 - A) * ec; }
    boat.windAngleAppAvg = atan2f(es, ec);
  }
  // Wind SPEED shown raw (no smoothing) so gusts/peaks read live, like a real
  // wind instrument — only the direction arrow is smoothed (see angleApparent).
  else if (!strcmp(p, "environment.wind.speedApparent")) boat.windSpeedApp = v.as<float>();
  else if (!strcmp(p, "environment.wind.speedTrue")) boat.windSpeedTrue = v.as<float>();
  else if (!strcmp(p, "environment.wind.fiveMinutes.gustTrue")) boat.gust5m = v.as<float>();
  else if (!strcmp(p, "environment.wind.oneHour.gustTrue")) boat.gust1h = v.as<float>();
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
  else if (!strcmp(p, "navigation.anchor.state")) { const char *s = v.as<const char *>(); if (s) strlcpy(boat.anchorOn, s, sizeof(boat.anchorOn)); }
  else if (!strcmp(p, "navigation.anchor.currentRadius")) boat.anchorRadius = v.as<float>();
  else if (!strcmp(p, "navigation.anchor.maxRadius")) boat.anchorMaxRadius = v.as<float>();
  else if (!strcmp(p, "notifications.navigation.anchor")) {
    const char *s = v["state"].as<const char *>();
    if (s) strlcpy(boat.anchorState, s, sizeof(boat.anchorState));
  }
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
  expanderInit();     // CH32V003: all outputs HIGH (release LCD/touch resets), backlight on, buzzer off
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
  panelHandle = rgbpanel->getPanelHandle();  // for the post-boot scan-out resync
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
  buzzer(true); delay(150); buzzer(false);   // short boot beep so you know it powered up
  Serial.println("setup done");
}

void loop() {
  // Scan-out resync to clear the boot vertical offset (GDMA starts mid-frame on a
  // cold boot). Fire a few times in the first few seconds — a single one didn't
  // clear it on a true cold boot, repeating does. Each is a brief flicker.
  static int fbResyncs = 0;
  if (fbResyncs < 4 && millis() > 800 && millis() - lastResync > 900) {
    fbResyncs++; fbResync("boot");
  }
  // Safety net for runtime drift: whatever slips between event-driven resyncs
  // gets realigned within 2 minutes.
  if (millis() - lastResync > 120000) fbResync("periodic");
  static bool up = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!up) { up = true; Serial.printf("WiFi IP %s\n", WiFi.localIP().toString().c_str()); setWifi(true); updateSysInfo(); }
    ws.loop();
  } else if (up) { up = false; setWifi(false); }
  static uint32_t lastUI = 0;
  // Hold data-driven repaints while a finger is down: the wind page's compass
  // rotation invalidates huge areas, and a redraw mid-flick starves touch
  // sampling so the gesture gets missed. Data catches up on release.
  if (dataDirty && !touchDown && millis() - lastUI > 150) { lastUI = millis(); dataDirty = false; updateUI(); }
  // ~400ms after a swipe lands, recompute wind positions + repaint once (clean
  // restore of the wind page). Runs here in loop context, so it can't stutter the swipe.
  if (tileRefreshPending && millis() - tileChangedAt > 400) {
    tileRefreshPending = false;
    // Hard-snap to the active tile: a swipe can settle a few px off an exact
    // tile boundary, leaving the whole page shifted left/right. No-op if aligned.
    lv_obj_set_tile_id(tileView, curTile, 0, LV_ANIM_OFF);
    // Swipes on heavy pages are peak PSRAM traffic — exactly when the scan-out
    // slips. Realign now; the flicker hides inside the settle repaint.
    fbResync("tile");
    updateUI(); lv_obj_invalidate(lv_scr_act());
  }
  lv_timer_handler();
  // Anchor drag alarm: pulse the buzzer (~350ms on/off) whenever the y2k plugin
  // reports alarm/emergency. Non-blocking; sounds regardless of the visible page.
  {
    static uint32_t beepAt = 0; static bool alarmOn = false;
    bool dragging = (!strcmp(boat.anchorState, "alarm") || !strcmp(boat.anchorState, "emergency")) ||
                    (!isnan(boat.anchorRadius) && !isnan(boat.anchorMaxRadius) && boat.anchorRadius > boat.anchorMaxRadius);
    if (dragging) {
      if (millis() - beepAt > 350) { beepAt = millis(); alarmOn = !alarmOn; buzzer(alarmOn); }
    } else if (alarmOn) { alarmOn = false; buzzer(false); }
  }
  static uint32_t beat = 0;
  if (millis() - beat > 4000) { beat = millis(); updateSysInfo(); Serial.printf("[beat] up=%lus wifi=%d sk=%d heap=%u touch=%d\n", millis()/1000, WiFi.status(), skUp, ESP.getFreeHeap(), touchOK); }
  delay(5);
}
