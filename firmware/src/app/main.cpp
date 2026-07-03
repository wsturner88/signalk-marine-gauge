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
#include <Preferences.h>
#include <ESPmDNS.h>
#include "portal.h"
#include "secrets.h"

Preferences prefs;   // NVS "gauge": display mode + WiFi/SignalK onboarding config

// ---------------- Config (NVS, seeded once from secrets.h if empty) ----------------
static char cfgSsid[33] = "", cfgPass[65] = "", cfgSkHost[64] = "";
static uint16_t cfgSkPort = 80;   // cfgSkHost empty = auto-discover SignalK via mDNS
static void loadConfig() {
  strlcpy(cfgSsid,   prefs.getString("ssid", "").c_str(),   sizeof(cfgSsid));
  strlcpy(cfgPass,   prefs.getString("pass", "").c_str(),   sizeof(cfgPass));
  strlcpy(cfgSkHost, prefs.getString("skhost", "").c_str(), sizeof(cfgSkHost));
  cfgSkPort = prefs.getUShort("skport", 80);
#ifdef WIFI_SSID
  if (!cfgSsid[0]) {   // dev seed: first boot adopts secrets.h so BIJOUX units just work
    strlcpy(cfgSsid, WIFI_SSID, sizeof(cfgSsid));
    strlcpy(cfgPass, WIFI_PASS, sizeof(cfgPass));
    strlcpy(cfgSkHost, SK_HOST, sizeof(cfgSkHost));
    cfgSkPort = SK_PORT;
    prefs.putString("ssid", cfgSsid); prefs.putString("pass", cfgPass);
    prefs.putString("skhost", cfgSkHost); prefs.putUShort("skport", cfgSkPort);
    Serial.println("[cfg] seeded from secrets.h");
  }
#endif
}
enum : uint8_t { RM_NORMAL, RM_PORTAL };
static uint8_t runMode = RM_NORMAL;
static bool skStarted = false;                     // ws.begin done (host resolved)
static char skHostRes[64] = ""; static uint16_t skPortRes = 0;
static void enterPortal();                         // defined with the portal UI below

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
  double lat = NAN, lon = NAN;                       // navigation.position (double: float loses ~0.5m)
  double anchorLat = NAN, anchorLon = NAN;           // navigation.anchor.position
  bool  seenY2k = false;                             // any anchor-plugin path received
  float battSoc = NAN, battV = NAN, battA = NAN, battW = NAN;
  float battTtg = NAN;   // s to empty (Victron caps at ~10 days when charging/full)
  float fuel = NAN, cabinTemp = NAN;
  float waterPort = NAN, waterStbd = NAN, waterAft = NAN;
  float solarW = NAN, solarYield = NAN;
  char  shore[20] = "--";
  float baro = NAN, cabinHum = NAN;   // Pa, ratio 0-1 (Cerbo CABIN Ruuvi)
  float freezerTemp = NAN;            // K (Cerbo sensor named "Freezer")
  float outsideTemp = NAN;            // K — no source yet; standard path subscribed for a future weather feed/sensor
  int   doy = 0; float utc = 0;       // GPS date/time (for the AUTO day/night sun calc)
  uint32_t utcAtMs = 0; bool timeValid = false;
} boat;

// ---------------- Display modes / backlight ----------------
// The CH32V003 expander exposes the backlight PWM duty at reg 0x05 (0-255).
enum : uint8_t { DM_DAY, DM_NIGHT, DM_SLEEP, DM_AUTO };
static const char *dmName[] = {"DAY", "NIGHT", "SLEEP", "AUTO"};
static const char *dmDesc[] = {
  "full brightness, always on",
  "dim glow, always on",
  "dark - touch to wake 20 s",
  "day / night by sun at GPS position",
};
static lv_obj_t *modeBtnLbl, *modeDescLbl;
static void modeCb(lv_event_t *);
// Saving the mode to flash stalls the PSRAM path and slips the scan-out (the
// roll!), so the NVS write is DEFERRED until 4s after the last tap, and a
// resync fires right after it (see loop()).
static bool modeDirty = false; static uint32_t modeDirtyAt = 0;
#define BL_DAY   255
#define BL_NIGHT 30     // always-readable dim glow (nav station / V-berth) — tune to taste
#define BL_WAKE  200    // SLEEP: brightness after a wake tap
static uint8_t  dispMode = DM_DAY;
static uint32_t wakeUntil = 0;         // SLEEP: stay lit until this ms
static bool     alarmSounding = false; // any active alarm forces full brightness
// v = desired brightness 0..255. The CH32V003 duty at reg 0x05 is INVERTED
// (empirical, bltest 2026-07-03): low reg = bright, 255 = backlight off.
// NEVER write reg value 0 — it latched the backlight dead until power loss.
static void setBacklight(uint8_t v) {
  static int cur = -1;
  if ((int)v == cur) return;
  cur = v;
  uint8_t reg = (v == 0) ? 255 : (uint8_t)((255 - v) < 8 ? 8 : 255 - v);
  exReg(0x05, reg);
}
// Sun above civil twilight (-6°)? NOAA-approx solar elevation from GPS pos+time.
// No time/position yet -> assume day (bright is the safe failure mode).
static bool sunIsUp() {
  if (!boat.timeValid || isnan(boat.lat)) return true;
  float utc = boat.utc + (millis() - boat.utcAtMs) / 3600000.0f;
  float g = 6.2832f / 365.0f * (boat.doy - 1 + (utc - 12) / 24.0f);
  float decl = 0.006918f - 0.399912f * cosf(g) + 0.070257f * sinf(g) - 0.006758f * cosf(2 * g)
             + 0.000907f * sinf(2 * g) - 0.002697f * cosf(3 * g) + 0.00148f * sinf(3 * g);
  float ha  = (utc + (float)boat.lon / 15.0f - 12.0f) * 15.0f * 0.0174533f;
  float lat = (float)boat.lat * 0.0174533f;
  float elev = asinf(sinf(lat) * sinf(decl) + cosf(lat) * cosf(decl) * cosf(ha));
  return elev > -0.105f;
}
static void applyBacklight() {
  uint8_t v;
  if (alarmSounding || runMode == RM_PORTAL) v = BL_DAY;   // alarms + setup override every mode
  else switch (dispMode) {
    default:
    case DM_DAY:   v = BL_DAY; break;
    case DM_NIGHT: v = BL_NIGHT; break;
    case DM_SLEEP: v = (millis() < wakeUntil) ? BL_WAKE : 0; break;
    case DM_AUTO:  v = sunIsUp() ? BL_DAY : BL_NIGHT; break;
  }
  setBacklight(v);
}
static bool dataDirty = false;

// ---------------- UI handles ----------------
#define NTILES 7
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
static lv_obj_t *socArc, *socLbl, *battVLbl, *chgBox, *chgLbl, *solarLbl, *yieldLbl, *srcLbl, *ttgLbl;
// system / connection page
static lv_obj_t *sysWifiState, *sysWifiInfo, *sysSkState, *sysSkInfo, *gust5Lbl, *gust1hLbl;
static lv_obj_t *wifiSetupLbl; static uint32_t wifiSetupArmedAt = 0;
// anchor page
static lv_obj_t *anchorStateLbl, *anchorRangeLbl, *anchorBar;
static lv_obj_t *anchorPlot, *aWindLbl, *aGustLbl, *aDepthLbl, *aSwingLbl;
static lv_color_t *anchorPlotBuf;
static float trailXs[90], trailYs[90]; static int trailN = 0;  // 10s samples = 15 min swing trail (m, E/N of anchor)
static float maxSwing = 0;                                     // largest distance from anchor seen this set (m)
// weather page
static lv_obj_t *baroArc, *baroValLbl, *baroTrendLbl, *baroChart;
static lv_chart_series_t *baroSer;
static lv_obj_t *wxOutLbl, *wxCabinLbl, *wxFreezeLbl;
static float baroHist[144]; static int baroN = 0;   // 10-min samples = 24h (RAM only, resets on boot)

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

  // hours-to-empty, left of the SoC ring (Victron capacity.timeRemaining)
  dimLbl(tile, "TIME LEFT", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 18, 84);
  ttgLbl = centerNum(tile, &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, 18, 104);

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
// boat position in meters east/north of the anchor drop (equirectangular — fine at swing scale)
static bool anchorRel(float *dxE, float *dyN) {
  if (isnan(boat.lat) || isnan(boat.anchorLat)) return false;
  float k = cosf((float)boat.anchorLat * 0.0174533f);
  *dxE = (float)((boat.lon - boat.anchorLon) * 111320.0) * k;
  *dyN = (float)((boat.lat - boat.anchorLat) * 111320.0);
  return true;
}
// Top-down swing plot (north-up): anchor cross at center, alarm ring, 15-min
// position trail, boat dot + heading tick, wind arrow on the rim (direction
// the wind blows FROM). Redrawn ~1/s in loop() while the page is visible.
static void drawAnchorPlot() {
  if (!anchorPlotBuf) return;
  lv_canvas_fill_bg(anchorPlot, lv_color_hex(0x0A1018), LV_OPA_COVER);
  bool drag = (!isnan(boat.anchorRadius) && !isnan(boat.anchorMaxRadius) && boat.anchorRadius > boat.anchorMaxRadius) ||
              !strcmp(boat.anchorState, "alarm") || !strcmp(boat.anchorState, "emergency");
  float dx = 0, dy = 0; bool rel = anchorRel(&dx, &dy);
  float dist = rel ? sqrtf(dx * dx + dy * dy) : 0;
  float R = 8;                                   // meters shown from center to rim (auto-zoom)
  if (!isnan(boat.anchorMaxRadius)) R = fmaxf(R, boat.anchorMaxRadius);
  R = fmaxf(fmaxf(R, dist), maxSwing);
  float sc = 130.0f / R;
  if (!isnan(boat.anchorMaxRadius)) {            // alarm boundary ring
    lv_draw_arc_dsc_t ad; lv_draw_arc_dsc_init(&ad);
    ad.color = lv_color_hex(drag ? C_RED : C_GREEN); ad.width = 2;
    lv_canvas_draw_arc(anchorPlot, 150, 150, (lv_coord_t)(boat.anchorMaxRadius * sc), 0, 360, &ad);
  }
  lv_draw_label_dsc_t nd; lv_draw_label_dsc_init(&nd);
  nd.color = lv_color_hex(C_DIM); nd.font = &lv_font_montserrat_14;
  lv_canvas_draw_text(anchorPlot, 143, 2, 20, &nd, "N");
  lv_draw_line_dsc_t cd; lv_draw_line_dsc_init(&cd);   // anchor drop = cross
  cd.color = lv_color_hex(C_TEXT); cd.width = 2;
  lv_point_t ch[] = {{142, 150}, {158, 150}}, cv[] = {{150, 142}, {150, 158}};
  lv_canvas_draw_line(anchorPlot, ch, 2, &cd);
  lv_canvas_draw_line(anchorPlot, cv, 2, &cd);
  lv_draw_rect_dsc_t td; lv_draw_rect_dsc_init(&td);   // swing trail dots
  td.bg_color = lv_color_hex(0x33627A); td.bg_opa = LV_OPA_COVER;
  td.radius = LV_RADIUS_CIRCLE; td.border_width = 0;
  for (int i = 0; i < trailN; i++) {
    int x = 150 + (int)(trailXs[i] * sc), y = 150 - (int)(trailYs[i] * sc);
    if (x >= 2 && x < 298 && y >= 2 && y < 298) lv_canvas_draw_rect(anchorPlot, x - 1, y - 1, 3, 3, &td);
  }
  if (rel) {                                           // the boat, with a heading tick
    int bx = 150 + (int)(dx * sc), by = 150 - (int)(dy * sc);
    lv_draw_rect_dsc_t bd; lv_draw_rect_dsc_init(&bd);
    bd.bg_color = lv_color_hex(C_ACCENT); bd.bg_opa = LV_OPA_COVER;
    bd.radius = LV_RADIUS_CIRCLE; bd.border_width = 0;
    lv_canvas_draw_rect(anchorPlot, bx - 4, by - 4, 9, 9, &bd);
    if (!isnan(boat.heading)) {
      lv_point_t hl[] = {{(lv_coord_t)bx, (lv_coord_t)by},
                         {(lv_coord_t)(bx + sinf(boat.heading) * 14), (lv_coord_t)(by - cosf(boat.heading) * 14)}};
      lv_draw_line_dsc_t hd; lv_draw_line_dsc_init(&hd);
      hd.color = lv_color_hex(C_ACCENT); hd.width = 3; hd.round_end = 1;
      lv_canvas_draw_line(anchorPlot, hl, 2, &hd);
    }
  }
  if (!isnan(boat.windAngleAppAvg) && !isnan(boat.heading)) {   // wind arrow at the rim
    float wd = boat.heading + boat.windAngleAppAvg;             // compass direction wind comes FROM
    float wx = sinf(wd), wy = -cosf(wd), px = cosf(wd), py = sinf(wd);
    lv_draw_line_dsc_t wl; lv_draw_line_dsc_init(&wl);
    wl.color = lv_color_hex(C_YELLOW); wl.width = 4; wl.round_end = 1; wl.round_start = 1;
    lv_point_t sh[] = {{(lv_coord_t)(150 + wx * 146), (lv_coord_t)(150 + wy * 146)},
                       {(lv_coord_t)(150 + wx * 118), (lv_coord_t)(150 + wy * 118)}};
    lv_canvas_draw_line(anchorPlot, sh, 2, &wl);
    lv_point_t t1[] = {{(lv_coord_t)(150 + wx * 118), (lv_coord_t)(150 + wy * 118)},
                       {(lv_coord_t)(150 + wx * 131 + px * 6), (lv_coord_t)(150 + wy * 131 + py * 6)}};
    lv_point_t t2[] = {{(lv_coord_t)(150 + wx * 118), (lv_coord_t)(150 + wy * 118)},
                       {(lv_coord_t)(150 + wx * 131 - px * 6), (lv_coord_t)(150 + wy * 131 - py * 6)}};
    lv_canvas_draw_line(anchorPlot, t1, 2, &wl);
    lv_canvas_draw_line(anchorPlot, t2, 2, &wl);
  }
}

static void buildAnchor(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  // the status IS the page title — big, color-coded
  anchorStateLbl = centerNum(tile, &lv_font_montserrat_36, LV_ALIGN_TOP_MID, 0, 8);
  lv_label_set_text(anchorStateLbl, "NO Y2K PLUGIN");
  lv_obj_set_style_text_color(anchorStateLbl, lv_color_hex(C_DIM), 0);

  anchorPlotBuf = (lv_color_t *)heap_caps_malloc(300 * 300 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  anchorPlot = lv_canvas_create(tile);
  if (anchorPlotBuf) {
    lv_canvas_set_buffer(anchorPlot, anchorPlotBuf, 300, 300, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(anchorPlot, lv_color_hex(0x0A1018), LV_OPA_COVER);
  }
  lv_obj_set_pos(anchorPlot, 14, 56);

  const int SX = 326;   // right-hand stats column
  dimLbl(tile, "RANGE / MAX ft", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, SX, 58);
  anchorRangeLbl = centerNum(tile, &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, SX, 76);
  dimLbl(tile, "WIND kn", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, SX, 122);
  aWindLbl = centerNum(tile, &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, SX, 140);
  aGustLbl = dimLbl(tile, "", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, SX + 70, 150);
  dimLbl(tile, "DEPTH ft", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, SX, 186);
  aDepthLbl = centerNum(tile, &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, SX, 204);
  dimLbl(tile, "MAX SWING ft", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, SX, 250);
  aSwingLbl = centerNum(tile, &lv_font_montserrat_28, LV_ALIGN_TOP_LEFT, SX, 268);

  anchorBar = lv_bar_create(tile);
  lv_obj_set_size(anchorBar, 300, 12);
  lv_obj_set_pos(anchorBar, 14, 366);
  lv_bar_set_range(anchorBar, 0, 100);
  lv_obj_set_style_bg_color(anchorBar, lv_color_hex(0x1E2A38), LV_PART_MAIN);
  lv_obj_set_style_radius(anchorBar, 6, LV_PART_MAIN);
  lv_obj_set_style_radius(anchorBar, 6, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(anchorBar, lv_color_hex(C_GREEN), LV_PART_INDICATOR);
  // plugin credit + discreet "test alarm" button (clear of the nav buttons)
  dimLbl(tile, "y2k-anchor-alarm", &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 14, 390);
  lv_obj_t *tb = lv_btn_create(tile);
  lv_obj_set_size(tb, 110, 34);
  lv_obj_align(tb, LV_ALIGN_BOTTOM_RIGHT, -14, -52);
  lv_obj_set_style_bg_color(tb, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(tb, LV_OPA_60, 0);
  lv_obj_set_style_radius(tb, 10, 0);
  lv_obj_t *tl = lv_label_create(tb);
  lv_label_set_text(tl, "test alarm");
  lv_obj_set_style_text_color(tl, lv_color_hex(C_DIM), 0);
  lv_obj_center(tl);
  lv_obj_add_event_cb(tb, anchorTestCb, LV_EVENT_CLICKED, NULL);
}

// ---- page 5: weather ----
// Barometer (Cerbo CABIN Ruuvi) with an on-device 3h trend + 24h history chart.
// OUTSIDE stays "--" until a source exists (weather plugin or an outdoor sensor).
static void buildWeather(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  titleLabel(tile, "WEATHER");

  lv_obj_t *cb = card(tile, 14, 44, 260, 252, "Barometer");
  baroArc = lv_arc_create(cb);
  lv_obj_set_size(baroArc, 168, 168);
  lv_obj_align(baroArc, LV_ALIGN_TOP_MID, 0, 16);
  lv_arc_set_rotation(baroArc, 135);
  lv_arc_set_bg_angles(baroArc, 0, 270);
  lv_arc_set_range(baroArc, 980, 1040);          // mb — covers deep low to strong high
  lv_obj_remove_style(baroArc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(baroArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(baroArc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(baroArc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(baroArc, lv_color_hex(0x1E2A38), LV_PART_MAIN);
  lv_obj_set_style_arc_color(baroArc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  baroValLbl = centerNum(cb, &lv_font_montserrat_36, LV_ALIGN_TOP_MID, 0, 76);
  dimLbl(cb, "mb", &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 114);
  baroTrendLbl = centerNum(cb, &lv_font_montserrat_20, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_label_set_text(baroTrendLbl, "trend --");
  lv_obj_set_style_text_color(baroTrendLbl, lv_color_hex(C_DIM), 0);

  lv_obj_t *co = card(tile, 288, 44, 178, 78, "Outside");
  wxOutLbl = bigVal(co, &lv_font_montserrat_28);
  lv_obj_align(wxOutLbl, LV_ALIGN_BOTTOM_LEFT, 0, 2);
  lv_obj_t *cc = card(tile, 288, 131, 178, 78, "Cabin");
  wxCabinLbl = bigVal(cc, &lv_font_montserrat_28);
  lv_obj_align(wxCabinLbl, LV_ALIGN_BOTTOM_LEFT, 0, 2);
  lv_obj_t *cf = card(tile, 288, 218, 178, 78, "Freezer");
  wxFreezeLbl = bigVal(cf, &lv_font_montserrat_28);
  lv_obj_align(wxFreezeLbl, LV_ALIGN_BOTTOM_LEFT, 0, 2);

  lv_obj_t *ch = card(tile, 14, 304, 452, 122, "Pressure  (24 h)");
  baroChart = lv_chart_create(ch);
  lv_obj_set_size(baroChart, 424, 74);
  lv_obj_align(baroChart, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_chart_set_type(baroChart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(baroChart, 144);      // one point per 10 min
  lv_chart_set_update_mode(baroChart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_chart_set_div_line_count(baroChart, 3, 5);
  lv_obj_set_style_bg_opa(baroChart, LV_OPA_0, 0);
  lv_obj_set_style_border_width(baroChart, 0, 0);
  lv_obj_set_style_line_color(baroChart, lv_color_hex(0x1E2A38), LV_PART_MAIN);
  lv_obj_set_style_size(baroChart, 0, LV_PART_INDICATOR);   // line only, no point dots
  baroSer = lv_chart_add_series(baroChart, lv_color_hex(C_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(baroChart, baroSer, LV_CHART_POINT_NONE);
}

// ---- page 5: system / connection (WiFi + SignalK status, gusts in corners) ----
static void buildSystem(lv_obj_t *tile) {
  lv_obj_set_style_bg_color(tile, lv_color_hex(C_BG), 0);
  titleLabel(tile, "SYSTEM");

  // WiFi card (slim: state + one info line)
  lv_obj_t *cw = card(tile, 24, 52, 432, 104, "WiFi");
  sysWifiState = lv_label_create(cw);
  lv_label_set_text(sysWifiState, "Disconnected");
  lv_obj_set_style_text_font(sysWifiState, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sysWifiState, lv_color_hex(C_RED), 0);
  lv_obj_align(sysWifiState, LV_ALIGN_TOP_LEFT, 0, 26);
  sysWifiInfo = lv_label_create(cw);
  lv_label_set_text(sysWifiInfo, "--");
  lv_obj_set_style_text_font(sysWifiInfo, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sysWifiInfo, lv_color_hex(C_DIM), 0);
  lv_obj_align(sysWifiInfo, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  // WiFi SETUP -> onboarding portal (two taps: arms to SURE?, second tap goes)
  lv_obj_t *sb = lv_btn_create(cw);
  lv_obj_set_size(sb, 96, 40);
  lv_obj_align(sb, LV_ALIGN_TOP_RIGHT, 0, 20);
  lv_obj_set_style_bg_color(sb, lv_color_hex(0x1E2A38), 0);
  lv_obj_set_style_radius(sb, 10, 0);
  wifiSetupLbl = lv_label_create(sb);
  lv_label_set_text(wifiSetupLbl, "SETUP");
  lv_obj_set_style_text_color(wifiSetupLbl, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(wifiSetupLbl, &lv_font_montserrat_16, 0);
  lv_obj_center(wifiSetupLbl);
  lv_obj_add_event_cb(sb, [](lv_event_t *) {
    if (wifiSetupArmedAt && millis() - wifiSetupArmedAt < 5000) {
      wifiSetupArmedAt = 0; lv_label_set_text(wifiSetupLbl, "SETUP");
      enterPortal();
    } else {
      wifiSetupArmedAt = millis(); lv_label_set_text(wifiSetupLbl, "SURE?");
    }
  }, LV_EVENT_CLICKED, NULL);

  // SignalK card (slim)
  lv_obj_t *cs = card(tile, 24, 168, 432, 104, "SignalK data");
  sysSkState = lv_label_create(cs);
  lv_label_set_text(sysSkState, "Disconnected");
  lv_obj_set_style_text_font(sysSkState, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(sysSkState, lv_color_hex(C_RED), 0);
  lv_obj_align(sysSkState, LV_ALIGN_TOP_LEFT, 0, 26);
  sysSkInfo = lv_label_create(cs);
  lv_label_set_text(sysSkInfo, "--");
  lv_obj_set_style_text_font(sysSkInfo, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(sysSkInfo, lv_color_hex(C_DIM), 0);
  lv_obj_align(sysSkInfo, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // Display card: current mode + description, CHANGE button cycles
  lv_obj_t *cd = card(tile, 24, 284, 432, 124, "Display");
  modeBtnLbl = lv_label_create(cd);
  lv_label_set_text(modeBtnLbl, dmName[dispMode]);
  lv_obj_set_style_text_font(modeBtnLbl, &lv_font_montserrat_36, 0);
  lv_obj_set_style_text_color(modeBtnLbl, lv_color_hex(C_ACCENT), 0);
  lv_obj_align(modeBtnLbl, LV_ALIGN_TOP_LEFT, 0, 28);
  modeDescLbl = lv_label_create(cd);
  lv_label_set_text(modeDescLbl, dmDesc[dispMode]);
  lv_obj_set_style_text_font(modeDescLbl, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(modeDescLbl, lv_color_hex(C_DIM), 0);
  lv_obj_align(modeDescLbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_t *mb = lv_btn_create(cd);
  lv_obj_set_size(mb, 118, 52);
  lv_obj_align(mb, LV_ALIGN_RIGHT_MID, 0, 4);
  lv_obj_set_style_bg_color(mb, lv_color_hex(0x1E2A38), 0);
  lv_obj_set_style_radius(mb, 12, 0);
  lv_obj_t *ml = lv_label_create(mb);
  lv_label_set_text(ml, "CHANGE");
  lv_obj_set_style_text_color(ml, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(ml, &lv_font_montserrat_16, 0);
  lv_obj_center(ml);
  lv_obj_add_event_cb(mb, modeCb, LV_EVENT_CLICKED, NULL);
}

// refresh WiFi/SignalK detail text (called periodically + on connect)
static void updateSysInfo() {
  if (!sysWifiInfo) return;
  char b[80];
  if (WiFi.status() == WL_CONNECTED)
    snprintf(b, sizeof(b), "%s   %s   %ld dBm", cfgSsid, WiFi.localIP().toString().c_str(), (long)WiFi.RSSI());
  else
    snprintf(b, sizeof(b), "%s   (joining...)", cfgSsid[0] ? cfgSsid : "(not configured)");
  lv_label_set_text(sysWifiInfo, b);
  if (skStarted)           snprintf(b, sizeof(b), "%s : %u", skHostRes, skPortRes);
  else if (cfgSkHost[0])   snprintf(b, sizeof(b), "%s : %u   (connecting)", cfgSkHost, cfgSkPort);
  else                     snprintf(b, sizeof(b), "searching via mDNS...");
  lv_label_set_text(sysSkInfo, b);
  // disarm a stale SURE? on the setup button
  if (wifiSetupLbl && wifiSetupArmedAt && millis() - wifiSetupArmedAt > 5000) {
    wifiSetupArmedAt = 0; lv_label_set_text(wifiSetupLbl, "SETUP");
  }
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
// display-mode CHANGE button (System page): tap cycles DAY/NIGHT/SLEEP/AUTO.
// No flash write here — deferred to loop() to keep the scan-out clean.
static void modeCb(lv_event_t *) {
  dispMode = (dispMode + 1) & 3;
  modeDirty = true; modeDirtyAt = millis();
  lv_label_set_text(modeBtnLbl, dmName[dispMode]);
  lv_label_set_text(modeDescLbl, dmDesc[dispMode]);
  wakeUntil = millis() + 20000;   // entering SLEEP: stay lit 20s so the change is visible
  applyBacklight();
}
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
  buildAnchor(tiles[4]); buildWeather(tiles[5]); buildSystem(tiles[6]);
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
  if (!isnan(boat.battTtg)) {
    float t = boat.battTtg;
    if (t >= 850000)     setTxt(ttgLbl, "--");            // Victron's "not really discharging" cap
    else if (t < 3600)   { snprintf(b, sizeof(b), "%dm", (int)(t / 60)); setTxt(ttgLbl, b); }
    else if (t < 86400)  { snprintf(b, sizeof(b), "%.1fh", t / 3600); setTxt(ttgLbl, b); }
    else                 { snprintf(b, sizeof(b), "%dd %dh", (int)(t / 86400), (int)(t / 3600) % 24); setTxt(ttgLbl, b); }
    static uint32_t lastTc = 0;
    uint32_t tc = t < 4 * 3600 ? C_RED : (t < 12 * 3600 ? C_YELLOW : C_TEXT);
    if (tc != lastTc) { lastTc = tc; lv_obj_set_style_text_color(ttgLbl, lv_color_hex(tc), 0); }
  }
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
  // --- weather (page 5) ---
  if (!isnan(boat.baro)) {
    snprintf(b, sizeof(b), "%.1f", boat.baro / 100.0f); setTxt(baroValLbl, b);
    int v = (int)(boat.baro / 100.0f + 0.5f); if (v < 980) v = 980; if (v > 1040) v = 1040;
    static int lastBv = -1;
    if (v != lastBv) { lastBv = v; lv_arc_set_value(baroArc, v); }
  }
  if (!isnan(boat.outsideTemp)) { snprintf(b, sizeof(b), "%d\xC2\xB0""F", (int)(boat.outsideTemp * 1.8f - 459.67f + 0.5f)); setTxt(wxOutLbl, b); }
  if (!isnan(boat.cabinTemp)) {
    int tf = (int)(boat.cabinTemp * 1.8f - 459.67f + 0.5f);
    if (!isnan(boat.cabinHum)) snprintf(b, sizeof(b), "%d\xC2\xB0""F  %d%%", tf, (int)(boat.cabinHum * 100 + 0.5f));
    else                       snprintf(b, sizeof(b), "%d\xC2\xB0""F", tf);
    setTxt(wxCabinLbl, b);
  }
  if (!isnan(boat.freezerTemp)) { snprintf(b, sizeof(b), "%.1f\xC2\xB0""F", boat.freezerTemp * 1.8f - 459.67f); setTxt(wxFreezeLbl, b); }
  // --- anchor watch (page 4) ---
  {
    // Anchored = navigation.anchor.state "on". Dragging = currentRadius beyond
    // maxRadius (robust — doesn't depend on the notification streaming), OR the
    // notification escalating to alarm/emergency.
    bool radDrag = (!isnan(boat.anchorRadius) && !isnan(boat.anchorMaxRadius) && boat.anchorRadius > boat.anchorMaxRadius);
    bool drag = radDrag || !strcmp(boat.anchorState, "alarm") || !strcmp(boat.anchorState, "emergency");
    bool anchored = !strcmp(boat.anchorOn, "on");
    const char *txt; uint32_t col;
    if (drag)              { txt = "DRAGGING!";     col = C_RED; }
    else if (anchored)     { txt = "ANCHORED";      col = C_GREEN; }
    else if (boat.seenY2k) { txt = "NOT ANCHORED";  col = C_DIM; }
    else                   { txt = "NO Y2K PLUGIN"; col = C_DIM; }
    setTxt(anchorStateLbl, txt);
    static uint32_t lastACol = 0;
    if (col != lastACol) { lastACol = col; lv_obj_set_style_text_color(anchorStateLbl, lv_color_hex(col), 0); }
    if (!isnan(boat.anchorMaxRadius)) {
      int cur = isnan(boat.anchorRadius) ? 0 : (int)(boat.anchorRadius * 3.28084f + 0.5f);
      int mx  = (int)(boat.anchorMaxRadius * 3.28084f + 0.5f);
      snprintf(b, sizeof(b), "%d / %d", cur, mx);
      setTxt(anchorRangeLbl, b);
      int pct = (mx > 0) ? (cur * 100 / mx) : 0; if (pct > 100) pct = 100;
      lv_bar_set_value(anchorBar, pct, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(anchorBar, lv_color_hex(col), LV_PART_INDICATOR);
    } else {
      setTxt(anchorRangeLbl, "--");
      lv_bar_set_value(anchorBar, 0, LV_ANIM_OFF);
    }
    if (!isnan(boat.windSpeedApp)) { snprintf(b, sizeof(b), "%.1f", boat.windSpeedApp * 1.94384f); setTxt(aWindLbl, b); }
    if (!isnan(boat.gust5m))       { snprintf(b, sizeof(b), "g %.0f", boat.gust5m * 1.94384f);     setTxt(aGustLbl, b); }
    if (!isnan(boat.depth))        { snprintf(b, sizeof(b), "%.1f", boat.depth * 3.28084f);        setTxt(aDepthLbl, b); }
    if (maxSwing > 0)              { snprintf(b, sizeof(b), "%d", (int)(maxSwing * 3.28084f + 0.5f)); setTxt(aSwingLbl, b); }
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
    "{\"path\":\"electrical.batteries.288.capacity.timeRemaining\",\"period\":5000},"
    "{\"path\":\"tanks.fuel.20.currentLevel\",\"period\":5000},"
    "{\"path\":\"tanks.freshWater.21.currentLevel\",\"period\":5000},"
    "{\"path\":\"tanks.freshWater.22.currentLevel\",\"period\":5000},"
    "{\"path\":\"tanks.freshWater.23.currentLevel\",\"period\":5000},"
    "{\"path\":\"environment.venus.28.temperature\",\"period\":5000},"
    "{\"path\":\"environment.venus.28.pressure\",\"period\":5000},"
    "{\"path\":\"environment.venus.28.humidity\",\"period\":10000},"
    "{\"path\":\"environment.inside.refrigerator.temperature\",\"period\":10000},"
    "{\"path\":\"environment.outside.temperature\",\"period\":10000},"
    "{\"path\":\"electrical.solar.278.panelPower\",\"period\":2000},"
    "{\"path\":\"electrical.solar.278.yieldToday\",\"period\":5000},"
    "{\"path\":\"electrical.venus.acSource\",\"period\":5000},"
    "{\"path\":\"navigation.position\",\"period\":2000},"
    "{\"path\":\"navigation.datetime\",\"period\":30000},"
    "{\"path\":\"navigation.anchor.position\",\"period\":5000},"
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
  else if (!strcmp(p, "electrical.batteries.288.capacity.timeRemaining")) boat.battTtg = v.as<float>();
  else if (!strcmp(p, "tanks.fuel.20.currentLevel")) boat.fuel = v.as<float>();
  else if (!strcmp(p, "tanks.freshWater.21.currentLevel")) boat.waterAft = v.as<float>();
  else if (!strcmp(p, "tanks.freshWater.22.currentLevel")) boat.waterStbd = v.as<float>();
  else if (!strcmp(p, "tanks.freshWater.23.currentLevel")) boat.waterPort = v.as<float>();
  else if (!strcmp(p, "environment.venus.28.temperature")) boat.cabinTemp = v.as<float>();
  else if (!strcmp(p, "environment.venus.28.pressure")) boat.baro = v.as<float>();
  else if (!strcmp(p, "environment.venus.28.humidity")) boat.cabinHum = v.as<float>();
  else if (!strcmp(p, "environment.inside.refrigerator.temperature")) boat.freezerTemp = v.as<float>();
  else if (!strcmp(p, "environment.outside.temperature")) boat.outsideTemp = v.as<float>();
  else if (!strcmp(p, "electrical.solar.278.panelPower")) boat.solarW = v.as<float>();
  else if (!strcmp(p, "electrical.solar.278.yieldToday")) boat.solarYield = v.as<float>();
  else if (!strcmp(p, "electrical.venus.acSource")) { const char *s = v.as<const char *>(); if (s) strlcpy(boat.shore, s, sizeof(boat.shore)); }
  else if (!strcmp(p, "navigation.position")) { boat.lat = v["latitude"].as<double>(); boat.lon = v["longitude"].as<double>(); }
  else if (!strcmp(p, "navigation.datetime")) {   // GPS UTC, for the AUTO sun calc
    const char *s = v.as<const char *>();
    int Y, M, D, h, m; float sec;
    if (s && sscanf(s, "%d-%d-%dT%d:%d:%f", &Y, &M, &D, &h, &m, &sec) == 6) {
      static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
      boat.doy = cum[M - 1] + D + ((M > 2 && Y % 4 == 0) ? 1 : 0);
      boat.utc = h + m / 60.0f + sec / 3600.0f;
      boat.utcAtMs = millis(); boat.timeValid = true;
    }
  }
  else if (!strcmp(p, "navigation.anchor.position")) {
    boat.seenY2k = true;
    boat.anchorLat = v["latitude"].as<double>(); boat.anchorLon = v["longitude"].as<double>();
  }
  else if (!strcmp(p, "navigation.anchor.state")) { boat.seenY2k = true; const char *s = v.as<const char *>(); if (s) strlcpy(boat.anchorOn, s, sizeof(boat.anchorOn)); }
  else if (!strcmp(p, "navigation.anchor.currentRadius")) { boat.seenY2k = true; boat.anchorRadius = v.as<float>(); }
  else if (!strcmp(p, "navigation.anchor.maxRadius")) { boat.seenY2k = true; boat.anchorMaxRadius = v.as<float>(); }
  else if (!strcmp(p, "notifications.navigation.anchor")) {
    boat.seenY2k = true;
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
// SignalK server resolution: manual host from config if set, else mDNS browse
// for _signalk-http._tcp once WiFi is up. skBegin is called exactly once.
static void skBegin(const char *h, uint16_t port) {
  strlcpy(skHostRes, h, sizeof(skHostRes)); skPortRes = port;
  ws.begin(skHostRes, skPortRes, SK_PATH);
  ws.onEvent(wsEvent); ws.setReconnectInterval(3000);
  skStarted = true;
  Serial.printf("[sk] -> ws://%s:%u%s\n", skHostRes, skPortRes, SK_PATH);
  updateSysInfo();
}

// ---------------- Onboarding portal (on-screen page + state machine) ----------------
static lv_obj_t *portalPage = nullptr, *portalStatus = nullptr;
static void showPortalUI() {
  if (portalPage) { lv_obj_clear_flag(portalPage, LV_OBJ_FLAG_HIDDEN); return; }
  portalPage = lv_obj_create(lv_layer_top());   // covers everything, eats input
  lv_obj_set_size(portalPage, 480, 480); lv_obj_set_pos(portalPage, 0, 0);
  lv_obj_set_style_bg_color(portalPage, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(portalPage, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(portalPage, 0, 0);
  lv_obj_set_style_radius(portalPage, 0, 0);
  lv_obj_clear_flag(portalPage, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *t = lv_label_create(portalPage);
  lv_label_set_text(t, "WiFi SETUP");
  lv_obj_set_style_text_color(t, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_36, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_t *qr = lv_qrcode_create(portalPage, 180, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
  char qrs[48]; snprintf(qrs, sizeof(qrs), "WIFI:T:nopass;S:%s;;", portalApName());
  lv_qrcode_update(qr, qrs, strlen(qrs));
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 66);
  dimLbl(portalPage, "1)  Point your phone camera here, or join WiFi:", &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 262);
  lv_obj_t *ap = lv_label_create(portalPage);
  lv_label_set_text(ap, portalApName());
  lv_obj_set_style_text_color(ap, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_text_font(ap, &lv_font_montserrat_28, 0);
  lv_obj_align(ap, LV_ALIGN_TOP_MID, 0, 286);
  dimLbl(portalPage, "2)  The setup page opens by itself", &lv_font_montserrat_16, LV_ALIGN_TOP_MID, 0, 330);
  dimLbl(portalPage, "(or browse to 192.168.4.1)", &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 354);
  portalStatus = lv_label_create(portalPage);
  lv_label_set_text(portalStatus, "waiting for a phone to join...");
  lv_obj_set_style_text_color(portalStatus, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_text_font(portalStatus, &lv_font_montserrat_20, 0);
  lv_obj_align(portalStatus, LV_ALIGN_BOTTOM_MID, 0, -22);
}
static bool portalStaRetry = false;   // true only when entered because WiFi FAILED
static void enterPortal() {
  if (runMode == RM_PORTAL) return;
  runMode = RM_PORTAL;
  portalStart();
  if (portalStaRetry && cfgSsid[0]) WiFi.begin(cfgSsid, cfgPass);  // failure entry: keep retrying behind the AP
  else WiFi.disconnect();   // deliberate entry: drop STA so we don't insta-exit on "still connected"
  showPortalUI();
  applyBacklight();
  Serial.println("[portal] entered setup mode");
}
static void exitPortal() {
  portalStop();
  if (portalPage) lv_obj_add_flag(portalPage, LV_OBJ_FLAG_HIDDEN);
  runMode = RM_NORMAL;
  Serial.println("[portal] exited (WiFi came up)");
}

void setup() {
  Serial.begin(115200); delay(800);
  Serial.println("\n\n=== BIJOUX gauge boot ===");
  prefs.begin("gauge", false);
  dispMode = prefs.getUChar("dmode", DM_DAY) & 3;
  wakeUntil = millis() + 60000;   // booting in SLEEP: stay lit a minute
  loadConfig();
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
  // dead-man's switch: finger held on the glass while powering up -> setup portal
  bool bootSetup = false;
  if (touchOK) {
    uint32_t t0 = millis();
    while (millis() - t0 < 500) {
      if (touch.getPoint(tpx, tpy, 1) > 0) { bootSetup = true; break; }
      delay(40);
    }
  }

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
  if (!cfgSsid[0] || bootSetup) {
    enterPortal();               // nothing configured (or finger held at boot)
  } else {
    WiFi.mode(WIFI_STA); WiFi.begin(cfgSsid, cfgPass);
  }
  // SignalK connect happens in loop() once WiFi is up (manual host or mDNS discovery)
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
  // --- onboarding portal state machine ---
  static bool wifiEverUp = false;
  if (runMode == RM_PORTAL) {
    portalLoop();
    static uint32_t rebootAt = 0, lastPst = 0;
    if (portalSaved() && !rebootAt) rebootAt = millis() + 2500;  // let the phone render the "saved" page
    if (rebootAt && millis() > rebootAt) ESP.restart();
    if (millis() - lastPst > 500 && portalStatus) {
      lastPst = millis();
      if (portalSaved()) setTxt(portalStatus, "Saved!  Rebooting...");
      else if (portalClients() > 0) setTxt(portalStatus, "phone connected - follow the page there");
      else setTxt(portalStatus, "waiting for a phone to join...");
    }
    // saved WiFi came back while we sat in FAILURE-entered setup -> resume normal
    if (portalStaRetry && !portalSaved() && WiFi.status() == WL_CONNECTED) exitPortal();
  } else if (!wifiEverUp && cfgSsid[0] && millis() > 45000) {
    // configured but never connected this boot (moved boats? typo?) -> offer setup
    portalStaRetry = true;
    enterPortal();
  }
  static bool up = false;
  if (WiFi.status() == WL_CONNECTED) {
    if (!up) { up = true; wifiEverUp = true; Serial.printf("WiFi IP %s\n", WiFi.localIP().toString().c_str()); setWifi(true); updateSysInfo(); }
    // --- SignalK: manual host, or discover via mDNS ---
    if (!skStarted && runMode == RM_NORMAL) {
      static uint32_t lastSkTry = 0;
      if (lastSkTry == 0 || millis() - lastSkTry > 15000) {
        lastSkTry = millis();
        if (cfgSkHost[0]) skBegin(cfgSkHost, cfgSkPort);
        else {
          static bool mdnsUp = false;
          if (!mdnsUp) mdnsUp = MDNS.begin("gauge");   // we also become gauge.local
          int n = MDNS.queryService("signalk-http", "tcp");   // blocks ~3s
          if (n > 0) skBegin(MDNS.address(0).toString().c_str(), MDNS.port(0));
          else { Serial.println("[sk] mDNS: no SignalK server found yet"); updateSysInfo(); }
        }
      }
    }
    if (skStarted) ws.loop();
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
  // Barometer log: one sample per 10 min -> 24h ring, chart point, and the 3h
  // trend (the number that matters — a fast fall means weather is coming).
  {
    static uint32_t lastBaro = 0;
    if (!isnan(boat.baro) && (lastBaro == 0 || millis() - lastBaro > 600000UL)) {
      lastBaro = millis();
      float mb = boat.baro / 100.0f;
      if (baroN < 144) baroHist[baroN++] = mb;
      else { memmove(baroHist, baroHist + 1, 143 * sizeof(float)); baroHist[143] = mb; }
      lv_chart_set_next_value(baroChart, baroSer, (lv_coord_t)(mb * 10 + 0.5f));
      float mn = baroHist[0], mx = baroHist[0];
      for (int i = 1; i < baroN; i++) { mn = fminf(mn, baroHist[i]); mx = fmaxf(mx, baroHist[i]); }
      lv_chart_set_range(baroChart, LV_CHART_AXIS_PRIMARY_Y,
                         (lv_coord_t)((mn - 1) * 10), (lv_coord_t)((mx + 1) * 10));
      if (baroN >= 2) {
        int back = (baroN - 1 < 18) ? baroN - 1 : 18;              // 18 samples = 3h
        float d3h = (baroHist[baroN - 1] - baroHist[baroN - 1 - back]) * (18.0f / back);
        char t[40];
        const char *sym = d3h > 0.5f ? LV_SYMBOL_UP : (d3h < -0.5f ? LV_SYMBOL_DOWN : LV_SYMBOL_MINUS);
        snprintf(t, sizeof(t), "%s %+.1f mb / 3h", sym, d3h);
        setTxt(baroTrendLbl, t);
        // marine rules of thumb: >1 mb/3h = change coming, >3 falling = gale watch
        uint32_t c = d3h <= -3.0f ? C_RED : (d3h <= -1.0f ? C_YELLOW : (d3h >= 1.0f ? C_GREEN : C_DIM));
        lv_obj_set_style_text_color(baroTrendLbl, lv_color_hex(c), 0);
      }
    }
  }
  // Anchor swing trail: sample position-vs-anchor every 10s while anchored;
  // track the largest swing. Trail + stats reset on each fresh anchor set.
  {
    static uint32_t lastTrail = 0; static bool wasAnchored = false;
    bool anchored = !strcmp(boat.anchorOn, "on");
    if (anchored && !wasAnchored) { trailN = 0; maxSwing = 0; }
    wasAnchored = anchored;
    float dx, dy;
    if (anchored && anchorRel(&dx, &dy) && millis() - lastTrail > 10000) {
      lastTrail = millis();
      if (trailN < 90) { trailXs[trailN] = dx; trailYs[trailN++] = dy; }
      else { memmove(trailXs, trailXs + 1, 89 * sizeof(float)); memmove(trailYs, trailYs + 1, 89 * sizeof(float));
             trailXs[89] = dx; trailYs[89] = dy; }
      float dist = sqrtf(dx * dx + dy * dy);
      if (dist > maxSwing) maxSwing = dist;
    }
  }
  // Redraw the swing plot ~1/s, only while the ANCHOR page is visible.
  {
    static uint32_t lastPlot = 0;
    if (curTile == 4 && millis() - lastPlot > 1000) { lastPlot = millis(); drawAnchorPlot(); }
  }
  lv_timer_handler();
  // Anchor drag alarm: pulse the buzzer (~350ms on/off) whenever the y2k plugin
  // reports alarm/emergency. Non-blocking; sounds regardless of the visible page.
  {
    static uint32_t beepAt = 0; static bool alarmOn = false;
    bool dragging = (!strcmp(boat.anchorState, "alarm") || !strcmp(boat.anchorState, "emergency")) ||
                    (!isnan(boat.anchorRadius) && !isnan(boat.anchorMaxRadius) && boat.anchorRadius > boat.anchorMaxRadius);
    alarmSounding = dragging;   // forces full brightness in every display mode
    if (dragging) {
      if (millis() - beepAt > 350) { beepAt = millis(); alarmOn = !alarmOn; buzzer(alarmOn); }
    } else if (alarmOn) { alarmOn = false; buzzer(false); }
  }
  // Display modes: a touch while sleeping lights the screen for 20s; the
  // backlight setter is change-guarded so calling every pass is free.
  if (dispMode == DM_SLEEP && touchDown) wakeUntil = millis() + 20000;
  applyBacklight();
  // Deferred mode save: NVS/flash writes stall PSRAM and slip the scan-out, so
  // commit well after the last tap and realign the panel immediately afterward.
  if (modeDirty && millis() - modeDirtyAt > 4000) {
    modeDirty = false;
    prefs.putUChar("dmode", dispMode);
    fbResync("nvs");
  }
  static uint32_t beat = 0;
  if (millis() - beat > 4000) { beat = millis(); updateSysInfo(); Serial.printf("[beat] up=%lus wifi=%d sk=%d heap=%u touch=%d\n", millis()/1000, WiFi.status(), skUp, ESP.getFreeHeap(), touchOK); }
  delay(5);
}
