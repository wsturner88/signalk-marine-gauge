// Captive-portal WiFi + SignalK onboarding.
//
// The gauge becomes an open AP ("Gauge-XXXX"); a wildcard DNS answers every
// lookup with our IP so phones auto-open the setup page (the hotel-WiFi trick).
// The page lists scanned networks (AP_STA mode lets us scan while serving the
// phone), takes the WiFi password, and either auto-discovers SignalK via mDNS
// (host left empty) or takes a manual host:port. Saved to NVS ("gauge"
// namespace, keys: ssid/pass/skhost/skport), then main reboots us.
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "portal.h"

extern Preferences prefs;   // owned by main.cpp, namespace "gauge"

static WebServer http(80);
static DNSServer dns;
static bool savedFlag = false;
static char apName[20] = "Gauge";
static String netsHtml;     // cached scan results, rendered as tappable rows

static const char HTML_HEAD[] PROGMEM = R"(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SignalK Gauge Setup</title><style>
body{font-family:-apple-system,Segoe UI,sans-serif;background:#05080F;color:#ECEFF1;margin:0;padding:20px;max-width:480px}
h1{font-size:22px;color:#4FC3F7;margin:0 0 4px}p{color:#78909C;margin:4px 0 14px}
.n{padding:12px 8px;border-bottom:1px solid #16202c;cursor:pointer}.n b{float:right;color:#546E7A;font-weight:400}
a{color:#4FC3F7;text-decoration:none;font-size:14px}
label{color:#90A4AE;font-size:14px}
input{width:100%;box-sizing:border-box;padding:12px;margin:6px 0 14px;border-radius:8px;border:1px solid #26323e;background:#0E1620;color:#ECEFF1;font-size:16px}
.ck{width:auto!important;margin:0 8px 0 0}
button{width:100%;padding:14px;border-radius:10px;border:0;background:#4FC3F7;color:#04121f;font-size:18px;font-weight:700;margin-top:8px}
</style></head><body><h1>&#9875; SignalK Gauge</h1>)";

static String pageHtml() {
  String s = FPSTR(HTML_HEAD);
  s += F("<p>Pick your boat WiFi and enter its password.</p>");
  s += netsHtml;
  s += F("<p style='text-align:right'><a href='/r'>rescan &#8635;</a></p>"
         "<form method='POST' action='/save'>"
         "<label>WiFi network</label><input id='s' name='s' required>"
         "<label>Password</label><input name='p' type='password'>"
         "<label><input class='ck' type='checkbox' name='a' checked onchange=\"document.getElementById('m').style.display=this.checked?'none':'block'\">"
         "Find the SignalK server automatically</label>"
         "<div id='m' style='display:none'><label>SignalK server address</label>"
         "<input name='h' placeholder='192.168.8.96'>"
         "<label>Port</label><input name='o' type='number' value='80'></div>"
         "<button>Save &amp; Connect</button></form>"
         "<script>document.querySelectorAll('.n').forEach(function(e){e.onclick=function(){document.getElementById('s').value=e.dataset.s}})</script>"
         "</body></html>");
  return s;
}

static void buildNets() {
  int n = WiFi.scanNetworks();   // blocking (~2-3 s); AP keeps running in AP_STA
  if (n > 64) n = 64;
  netsHtml = "";
  bool used[64] = {false};
  int emitted = 0;
  while (emitted < 12) {         // strongest first, duplicate SSIDs collapsed
    int best = -1;
    for (int i = 0; i < n; i++)
      if (!used[i] && WiFi.SSID(i).length() && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
    if (best < 0) break;
    used[best] = true;
    String ssid = WiFi.SSID(best);
    if (netsHtml.indexOf("data-s=\"" + ssid + "\"") >= 0) continue;
    netsHtml += "<div class='n' data-s=\"" + ssid + "\">" + ssid + "<b>" +
                String(WiFi.RSSI(best)) + " dBm" +
                (WiFi.encryptionType(best) == WIFI_AUTH_OPEN ? "" : " &#128274;") + "</b></div>";
    emitted++;
  }
  WiFi.scanDelete();
}

static void handleSave() {
  String ssid = http.arg("s"), pass = http.arg("p");
  bool autoSk = http.arg("a") == "on";
  String host = autoSk ? "" : http.arg("h");
  uint16_t port = (uint16_t)http.arg("o").toInt(); if (!port) port = 80;
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("skhost", host);
  prefs.putUShort("skport", port);
  String s = FPSTR(HTML_HEAD);
  s += "<p style='font-size:18px;color:#66BB6A'>Saved!</p><p>The gauge is rebooting and joining <b>" +
       ssid + "</b>. " +
       (host.length() ? "SignalK: " + host : String("It will find your SignalK server automatically")) +
       ".</p><p>You can close this page and rejoin your normal WiFi.</p></body></html>";
  http.send(200, "text/html", s);
  savedFlag = true;
  Serial.printf("[portal] saved ssid='%s' sk='%s':%u\n", ssid.c_str(), host.c_str(), port);
}

void portalStart() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  snprintf(apName, sizeof(apName), "Gauge-%02X%02X", mac[4], mac[5]);
  WiFi.mode(WIFI_AP_STA);       // AP for the phone + STA side free for scans/retries
  buildNets();                  // scan BEFORE the AP has clients for a clean list
  WiFi.softAP(apName);          // open network, 192.168.4.1
  dns.start(53, "*", WiFi.softAPIP());
  http.on("/", []() { http.send(200, "text/html", pageHtml()); });
  http.on("/r", []() { buildNets(); http.sendHeader("Location", "/"); http.send(302, "text/plain", ""); });
  http.on("/save", HTTP_POST, handleSave);
  // every unknown URL (incl. iOS/Android connectivity probes) -> our page
  http.onNotFound([]() { http.sendHeader("Location", "http://192.168.4.1/"); http.send(302, "text/plain", ""); });
  http.begin();
  Serial.printf("[portal] AP '%s' up at %s\n", apName, WiFi.softAPIP().toString().c_str());
}

void portalStop() {
  http.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

void portalLoop() { dns.processNextRequest(); http.handleClient(); }
bool portalSaved() { return savedFlag; }
int portalClients() { return WiFi.softAPgetStationNum(); }
const char *portalApName() { return apName; }
