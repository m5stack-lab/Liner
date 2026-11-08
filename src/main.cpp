// M5Stack PaperColor — Volumio "Now Playing" display
//
// Polls Volumio's local REST API and, when the playing track changes, renders
// a portrait now-playing card (album art + title/artist/album) to the 400x600
// Spectra 6 e-paper panel. The panel driver (Panel_ED2208, via M5GFX) quantizes
// our plain RGB888 drawing down to its 6-color palette automatically — we just
// draw normally and call display() to commit a refresh.
//
// A full e-paper refresh takes ~15-30s, so redraws are event-driven (only on
// track change), not timer-driven.

#include <M5Unified.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>

#include "config.h"
#include "web_ui.h"

// M5GFX's bundled QR encoder (the one behind M5.Display.qrcode()), reused to
// serve the same codes to the dashboard as SVG. Its header defines generic
// macro names, so they're dropped right after, as M5GFX itself does.
#include <vector>
#include <lgfx/utility/lgfx_qrcode.h>
#undef MODE_NUMERIC
#undef MODE_ALPHANUMERIC
#undef MODE_BYTE
#undef ECC_LOW
#undef ECC_MEDIUM
#undef ECC_QUARTILE
#undef ECC_HIGH
#undef LOCK_VERSION

static const char* LINER_VERSION = "1.9.0";
static const char* MDNS_HOSTNAME = "liner"; // also the OTA target: liner.local
static const char* WEB_USER = "liner";      // username when the web password is on

static const int PANEL_W = 400;
static const int PANEL_H = 600;
static const int ART_SIZE = 400; // album art occupies the top, full-width square

// Idle screensaver: after this long showing "Nothing Playing", start cycling
// generative Spectra-6 patterns instead, changing on this interval. Kept
// infrequent since each change costs a full 15-30s e-paper refresh.
static const uint32_t SCREENSAVER_DELAY_MS = 5UL * 60 * 1000;
static const uint32_t SCREENSAVER_INTERVAL_MS = 10UL * 60 * 1000;

String lastRenderedKey = "\x01__BOOT__\x01"; // sentinel so the first real state always renders

// Album art state for the track currently on screen. Volumio 4 can report a
// placeholder (or empty) albumart for the first poll of a new track and fill
// in the real cover a moment later; if the first render couldn't draw art, a
// later poll with a different art URL triggers one more redraw.
String lastRenderedArtUrl;
bool lastRenderArtOk = false;

// Hang protection: if loop() stops making progress for this long, the task
// watchdog reboots the device instead of leaving a frozen screen and a dead
// web portal. Must exceed the worst legitimate stretch between feeds (one
// art download, ~30s, or one e-paper refresh, ~30s; we feed in between).
static const uint32_t WDT_TIMEOUT_S = 90;

void feedWatchdog() { esp_task_wdt_reset(); }

void startWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true); // reconfigures if already initialized
#endif
  esp_task_wdt_add(NULL); // the loop task
}

// Cached from the most recent successful poll, regardless of whether it
// triggered an e-paper redraw — lets the web portal show current playback
// without making its own extra round-trip to Volumio on every page load.
bool cachedPlaying = false;
String cachedTitle, cachedArtist, cachedAlbum, cachedArtUrl, cachedMetaLine;

// ---------------------------------------------------------------------------
// Wi-Fi + Volumio setup, and an always-on settings/status web page
//
// Credentials and the Volumio host live in NVS (via Preferences), not in
// compile-time config — so one built binary works for anyone, configured
// through a small web form. This is what makes a prebuilt release binary
// (e.g. for M5Burner) actually usable by someone other than the person who
// built it. The same server and page work in two contexts: reachable at
// http://192.168.4.1 while broadcasting the Liner-Setup AP (first boot, a
// failed connection, or the top button), and at http://liner.local (or the
// device's LAN IP) once connected normally — so settings can be changed
// without needing to disconnect and rejoin the device's own AP.
// ---------------------------------------------------------------------------

Preferences prefs;
WebServer webServer(80);
bool inSetupMode = false;
bool pendingReconnect = false; // set by handleSaveNetwork() when the network was changed

String savedSsid;
String savedPassword;
String savedVolumioHost;
bool otaEnabled = true;
bool screensaverEnabled = true;
bool webAuthEnabled = false;
String webPassword;

DNSServer dnsServer;            // captive portal while in setup mode
bool otaRunning = false;        // ArduinoOTA actually listening (see applyOtaState)
bool setupScreenShown = false;  // the setup QR screen is what's on the panel now
bool setupRequestedByUser = false;
uint32_t setupSince = 0;
uint32_t lastPollTime = 0;

// Setup mode entered on its own (Wi-Fi lost or failed) retries the saved
// network this often, but only while nobody is using the setup AP.
static const uint32_t SETUP_RETRY_MS = 2UL * 60 * 1000;

// Volumio health: after this many failed polls in a row (~15s) the panel
// switches to a "can't reach Volumio" screen with a QR to the dashboard.
static const int VOLUMIO_FAIL_THRESHOLD = 3;
// While Volumio is unreachable, poll it this rarely. A failed poll blocks for
// its DNS/connect timeout and the dashboard can't answer meanwhile, so polling
// every 5s would lock out the very page used to fix the host.
static const uint32_t VOLUMIO_RETRY_MS = 30UL * 1000;
int volumioFailures = 0;
bool volumioReachable = false;

void loadSettings() {
  prefs.begin("liner", true); // read-only
  savedSsid = prefs.getString("ssid", "");
  savedPassword = prefs.getString("pass", "");
  savedVolumioHost = prefs.getString("vhost", VOLUMIO_HOST_DEFAULT);
  otaEnabled = prefs.getBool("ota", true);
  screensaverEnabled = prefs.getBool("screensaver", true);
  webAuthEnabled = prefs.getBool("webauth", false);
  webPassword = prefs.getString("webpass", "");
  if (webPassword.length() == 0) webAuthEnabled = false;
  prefs.end();
}

void saveNetwork(const String& ssid, const String& password, const String& vhost) {
  String host = vhost.length() ? vhost : String(VOLUMIO_HOST_DEFAULT);
  prefs.begin("liner", false); // read-write
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  prefs.putString("vhost", host);
  prefs.end();
  savedSsid = ssid;
  savedPassword = password;
  savedVolumioHost = host;
}

void saveOptions() {
  prefs.begin("liner", false);
  prefs.putBool("ota", otaEnabled);
  prefs.putBool("screensaver", screensaverEnabled);
  prefs.end();
}

void saveWebAuth(bool enabled, const String& password) {
  prefs.begin("liner", false);
  prefs.putBool("webauth", enabled);
  prefs.putString("webpass", password);
  prefs.end();
  webAuthEnabled = enabled;
  webPassword = password;
}

// The e-paper refresh takes 15-30s, but the panel runs it on its own once
// display() returns. Instead of blocking in waitDisplay(), Liner keeps
// answering the dashboard (and OTA) in the meantime, so the web page shows a
// new track within a poll while the paper catches up. Web handlers never draw
// on the panel, so this can't re-enter the display code.
bool panelBusy = false; // a now-playing redraw is underway (art download or refresh)

// Where the time goes in the last now-playing redraw, reported in /api/status
// as "lastRender". lagMs is how far into the track Volumio already was when
// Liner noticed the change (its "seek"), i.e. the detection delay.
struct RenderTiming {
  uint32_t lagMs = 0, artMs = 0, artBytes = 0, decodeMs = 0, textMs = 0, waitPrevMs = 0, transferMs = 0, refreshMs = 0;
  bool artCached = false; // cover came from the cache (no download)
  uint32_t at = 0; // uptime (s) when the redraw finished
} lastTiming;

void serviceNetwork() {
  feedWatchdog();
  webServer.handleClient();
  if (inSetupMode) dnsServer.processNextRequest();
  if (otaRunning) ArduinoOTA.handle();
}

void waitPanel() {
  panelBusy = true;
  while (M5.Display.displayBusy()) {
    serviceNetwork();
    delay(5);
  }
  panelBusy = false;
}

void showStatus(const String& msg) {
  setupScreenShown = false;
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  M5.Display.drawString(msg, PANEL_W / 2, PANEL_H / 2);
  M5.Display.endWrite();
  M5.Display.display();
  waitPanel();
}

// Multi-line variant for setup mode, where the AP name + IP need their own lines.
void showStatusLines(const String& line1, const String& line2, const String& line3) {
  setupScreenShown = false;
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  int y = line3.length() ? (PANEL_H / 2 - 30) : (PANEL_H / 2 - 15);
  M5.Display.drawString(line1, PANEL_W / 2, y);
  y += 30;
  M5.Display.drawString(line2, PANEL_W / 2, y);
  if (line3.length()) {
    y += 30;
    M5.Display.drawString(line3, PANEL_W / 2, y);
  }
  M5.Display.endWrite();
  M5.Display.display();
  waitPanel();
}

// QR screens. Codes are plain black on white, which the Spectra 6 panel
// reproduces exactly, so phones scan them straight off the glass.
static const char* SETUP_AP_IP = "192.168.4.1"; // softAP default

// What each QR code encodes, shared by the panel and the dashboard.
// "wifi" joins the setup AP. Anything else is Liner's own address: its
// setup-AP IP while no network is assigned, its LAN IP once connected (an IP
// rather than liner.local, since not every phone resolves .local names).
String qrText(const String& which) {
  if (which == "wifi") return String("WIFI:T:nopass;S:") + SETUP_AP_NAME + ";;";
  IPAddress ip = inSetupMode ? WiFi.softAPIP() : WiFi.localIP();
  return "http://" + ip.toString() + "/";
}

// A QR code's module grid. Modules are read straight from the bit buffer
// rather than through lgfx_qrcode_getModule(): that function is compiled as
// C, where M5GFX's header makes `bool` an unsigned char, so it returns the raw
// bit (0x80, 0x40, ...) and C++ callers, which expect 0 or 1, drop most dark
// modules. M5.Display.qrcode() goes through that call too, so the panel uses
// drawQr() below instead.
struct QrGrid {
  std::vector<uint8_t> modules;
  QRCode qr = {};
  int size() const { return qr.size; }
  bool dark(int x, int y) const {
    uint32_t i = (uint32_t)y * qr.size + x;
    return (modules[i >> 3] >> (7 - (i & 7))) & 1;
  }
};

bool makeQr(const String& text, QrGrid& g) {
  for (uint8_t version = 1; version <= 10; version++) {
    g.modules.assign(lgfx_qrcode_getBufferSize(version), 0);
    if (lgfx_qrcode_initText(&g.qr, g.modules.data(), version, 0, text.c_str()) == 0) return true;
  }
  return false;
}

// Black on white inside a w x w square at (x, y), centered on whole pixels.
void drawQr(const String& text, int x, int y, int w) {
  QrGrid g;
  if (!makeQr(text, g)) return;
  int cell = w / g.size();
  int off = (w - cell * g.size()) / 2;
  M5.Display.fillRect(x, y, w, w, TFT_WHITE);
  for (int r = 0; r < g.size(); r++)
    for (int c = 0; c < g.size(); c++)
      if (g.dark(c, r)) M5.Display.fillRect(x + off + c * cell, y + off + r * cell, cell, cell, TFT_BLACK);
}

// SVG of a QR code: one path, merged runs of dark modules, 2-module quiet zone.
String qrSvg(const String& text) {
  QrGrid g;
  if (!makeQr(text, g)) return "";
  const int q = 2, n = g.size() + q * 2;
  String path;
  path.reserve(g.size() * g.size() * 2);
  for (int y = 0; y < g.size(); y++) {
    for (int x = 0; x < g.size(); x++) {
      if (!g.dark(x, y)) continue;
      int run = 1;
      while (x + run < g.size() && g.dark(x + run, y)) run++;
      path += "M" + String(x + q) + " " + String(y + q) + "h" + String(run) + "v1h-" + String(run) + "z";
      x += run - 1;
    }
  }
  return "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 " + String(n) + " " + String(n) +
         "' shape-rendering='crispEdges'><rect width='100%' height='100%' fill='#fff'/>"
         "<path fill='#000' d='" + path + "'/></svg>";
}

void drawCentered(const String& s, int y, const lgfx::IFont* font) {
  M5.Display.setFont(font);
  M5.Display.drawString(s, PANEL_W / 2, y);
}

void beginScreen() {
  setupScreenShown = false;
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_center);
}

void endScreen() {
  M5.Display.endWrite();
  feedWatchdog();
  M5.Display.display();
  waitPanel();
  feedWatchdog();
}

// Not on any Wi-Fi: first boot, a failed connect, or the top button. Step 1
// joins the phone to the open setup AP; step 2 opens the dashboard (most
// phones also pop it up on their own via the captive portal).
void renderSetupScreen() {
  beginScreen();
  drawCentered("Set up Liner", 34, &fonts::FreeSansBold18pt7b);
  drawCentered("1. Scan to join Wi-Fi", 82, &fonts::FreeSans12pt7b);
  drawQr(qrText("wifi"), (PANEL_W - 200) / 2, 100, 200);
  drawCentered("2. Scan to open setup", 330, &fonts::FreeSans12pt7b);
  drawQr(qrText("address"), (PANEL_W - 160) / 2, 350, 160);
  drawCentered(String("Wi-Fi: ") + SETUP_AP_NAME, 538, &fonts::FreeSans9pt7b);
  drawCentered(String("then browse to ") + SETUP_AP_IP, 564, &fonts::FreeSans9pt7b);
  endScreen();
  setupScreenShown = true;
}

// On Wi-Fi but Volumio isn't answering. The QR opens the dashboard on the
// LAN, where the Volumio host can be fixed.
void renderNetworkIssue() {
  String url = qrText("address");
  beginScreen();
  drawCentered("Can't reach Volumio", 60, &fonts::FreeSansBold18pt7b);
  drawCentered(savedVolumioHost, 104, &fonts::FreeSans12pt7b);
  drawCentered("Liner keeps retrying.", 136, &fonts::FreeSans9pt7b);
  drawQr(url, (PANEL_W - 220) / 2, 180, 220);
  drawCentered("Scan to open settings", 440, &fonts::FreeSans12pt7b);
  drawCentered(String(MDNS_HOSTNAME) + ".local", 480, &fonts::FreeSans9pt7b);
  drawCentered(WiFi.localIP().toString(), 506, &fonts::FreeSans9pt7b);
  endScreen();
}

// ---------------------------------------------------------------------------
// OTA
//
// Runs only while the toggle is on AND the device is on the LAN. ArduinoOTA's
// own mDNS handling is off because ArduinoOTA.end() calls MDNS.end(), which
// would take liner.local down with it; the _arduino._tcp advert is managed
// here instead, so switching OTA off also stops advertising it.
// ---------------------------------------------------------------------------

void setupOta() {
  ArduinoOTA.setHostname(MDNS_HOSTNAME);
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart([]() { Serial.println("OTA: update starting"); });
  ArduinoOTA.onEnd([]() { Serial.println("OTA: update complete, rebooting"); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    feedWatchdog();
    Serial.printf("OTA: %u%%\n", (done * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error [%u]: %s\n", error,
      error == OTA_AUTH_ERROR ? "auth failed" :
      error == OTA_BEGIN_ERROR ? "begin failed" :
      error == OTA_CONNECT_ERROR ? "connect failed" :
      error == OTA_RECEIVE_ERROR ? "receive failed" :
      error == OTA_END_ERROR ? "end failed" : "unknown");
  });
}

// Cheap enough to call every loop: it only acts when the wanted state changes.
void applyOtaState() {
  bool want = otaEnabled && !inSetupMode && WiFi.status() == WL_CONNECTED;
  if (want == otaRunning) return;
  if (want) {
    ArduinoOTA.begin();
    MDNS.enableArduino(3232, false);
    Serial.printf("OTA ready at %s.local\n", MDNS_HOSTNAME);
  } else {
    ArduinoOTA.end();
    MDNS.disableArduino();
    Serial.println("OTA stopped");
  }
  otaRunning = want;
}

// ---------------------------------------------------------------------------
// Web dashboard + JSON API
//
// GET  /             dashboard page (include/web_ui.h)
// GET  /api/status   device, network, now playing and settings as JSON
// GET  /api/scan     nearby Wi-Fi networks
// GET  /api/qr.svg   Liner's address as a QR code (setup-AP IP or LAN IP)
// POST /api/options  ota=0|1, screensaver=0|1 (applied immediately)
// POST /api/auth     enabled=0|1, password=... (web password)
// POST /save         ssid, password, vhost (Wi-Fi + Volumio host)
//
// With password protection on, every route asks for HTTP digest auth
// (username "liner"), so the password never crosses the network in clear text.
// POSTs must also carry an X-Liner header. The dashboard always sends it; a
// custom header makes browsers run a CORS preflight the device never answers,
// so another web page can't use your saved login to change settings.
// ---------------------------------------------------------------------------

bool authorized() {
  if (!webAuthEnabled || webPassword.length() == 0) return true;
  if (webServer.authenticate(WEB_USER, webPassword.c_str())) return true;
  webServer.requestAuthentication(DIGEST_AUTH, "Liner", "Password required");
  return false;
}

bool authorizedPost() {
  if (!authorized()) return false;
  if (!webServer.hasHeader("X-Liner")) {
    webServer.send(403, "text/plain", "Use the Liner dashboard to change settings.");
    return false;
  }
  return true;
}

void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(code, "application/json", out);
}

void handleRoot() {
  if (!authorized()) return;
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

void handleStatus() {
  if (!authorized()) return;
  bool online = !inSetupMode && WiFi.status() == WL_CONNECTED;
  JsonDocument d;
  d["version"] = LINER_VERSION;
  d["mode"] = inSetupMode ? "setup" : (online ? "online" : "offline");
  d["ssid"] = inSetupMode ? String(SETUP_AP_NAME) : (online ? WiFi.SSID() : String(""));
  d["ip"] = inSetupMode ? WiFi.softAPIP().toString() : (online ? WiFi.localIP().toString() : String(""));
  if (online) d["rssi"] = WiFi.RSSI();
  d["hostname"] = String(MDNS_HOSTNAME) + ".local";
  d["savedSsid"] = savedSsid;
  d["vhost"] = savedVolumioHost;
  d["volumio"] = online && volumioReachable;
  d["uptime"] = millis() / 1000;
  d["ota"] = otaEnabled;
  d["otaActive"] = otaRunning;
  d["screensaver"] = screensaverEnabled;
  d["auth"] = webAuthEnabled;
  d["panelBusy"] = panelBusy;
  JsonObject lr = d["lastRender"].to<JsonObject>();
  lr["lagMs"] = lastTiming.lagMs;
  lr["artMs"] = lastTiming.artMs;
  lr["artBytes"] = lastTiming.artBytes;
  lr["artCached"] = lastTiming.artCached;
  lr["decodeMs"] = lastTiming.decodeMs;
  lr["textMs"] = lastTiming.textMs;
  lr["waitPrevMs"] = lastTiming.waitPrevMs;
  lr["transferMs"] = lastTiming.transferMs;
  lr["refreshMs"] = lastTiming.refreshMs;
  lr["at"] = lastTiming.at;
  JsonObject np = d["np"].to<JsonObject>();
  np["playing"] = cachedPlaying;
  if (cachedPlaying) {
    np["title"] = cachedTitle;
    np["artist"] = cachedArtist;
    np["album"] = cachedAlbum;
    np["art"] = cachedArtUrl;
    np["meta"] = cachedMetaLine;
  }
  sendJson(200, d);
}

// GET /api/qr.svg: Liner's address as a QR code (see qrText). Takes no
// input, so it can't be made to encode anything else.
void handleQr() {
  if (!authorized()) return;
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "image/svg+xml", qrSvg(qrText("address")));
}

void handleScan() {
  if (!authorized()) return;
  if (panelBusy) { // a scan would stall the album-art download mid-redraw
    webServer.send(503, "text/plain", "Liner is updating its screen. Try again in a few seconds.");
    return;
  }
  int n = WiFi.scanNetworks();
  JsonDocument d;
  JsonArray arr = d.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    bool dup = false; // keep only the strongest entry per SSID (mesh APs)
    for (JsonObject o : arr) {
      if (ssid == o["ssid"].as<const char*>()) {
        if (WiFi.RSSI(i) > o["rssi"].as<int>()) o["rssi"] = WiFi.RSSI(i);
        dup = true;
        break;
      }
    }
    if (dup) continue;
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(200, d);
}

void handleOptions() {
  if (!authorizedPost()) return;
  if (webServer.hasArg("ota")) otaEnabled = webServer.arg("ota") == "1";
  if (webServer.hasArg("screensaver")) screensaverEnabled = webServer.arg("screensaver") == "1";
  saveOptions();
  applyOtaState();
  handleStatus();
}

void handleAuth() {
  if (!authorizedPost()) return;
  bool enable = webServer.arg("enabled") == "1";
  String pw = webServer.arg("password");
  if (enable && (pw.length() < 4 || pw.length() > 64)) {
    webServer.send(400, "text/plain", "Password must be 4 to 64 characters.");
    return;
  }
  saveWebAuth(enable, enable ? pw : String(""));
  Serial.printf("Web password %s\n", enable ? "set" : "removed");
  JsonDocument d;
  d["ok"] = true;
  d["auth"] = webAuthEnabled;
  sendJson(200, d);
}

void handleSaveNetwork() {
  if (!authorizedPost()) return;
  String ssid = webServer.arg("ssid");
  String password = webServer.arg("password");
  String vhost = webServer.arg("vhost");
  ssid.trim();
  vhost.trim();

  if (ssid.length() == 0) {
    webServer.send(400, "text/plain", "Network name is required.");
    return;
  }

  // The dashboard never shows the saved password back, so a blank field on
  // an unchanged network means "keep it as-is", not "set it to empty".
  bool networkChanged = (ssid != savedSsid) || (password.length() > 0 && password != savedPassword);
  if (ssid == savedSsid && password.length() == 0) password = savedPassword;
  // Leaving setup mode always needs a connect, even with the same network.
  bool reconnect = networkChanged || inSetupMode;

  if (vhost.length() && vhost != savedVolumioHost) {
    volumioFailures = 0;
    lastPollTime = 0; // try the new host on the next loop
  }
  saveNetwork(ssid, password, vhost);

  JsonDocument d;
  d["ok"] = true;
  d["reconnecting"] = reconnect;
  d["ssid"] = ssid;
  sendJson(200, d);

  // loop() draws "Connecting to..." and reconnects; handlers never draw.
  if (reconnect) pendingReconnect = true;
}

// In setup mode every unknown URL (phones' captive-portal probes included)
// is sent to the dashboard, so joining Liner-Setup opens it automatically.
void handleNotFound() {
  if (inSetupMode) {
    webServer.sendHeader("Location", String("http://") + SETUP_AP_IP + "/");
    webServer.send(302, "text/plain", "");
    return;
  }
  webServer.send(404, "text/plain", "Not found");
}

// Registers routes and starts the web server exactly once. The server itself
// stays up for the device's entire runtime, in both AP (setup) and STA
// (normal) modes.
void startWebServer() {
  static const char* headers[] = {"X-Liner"};
  webServer.collectHeaders(headers, 1);
  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/api/status", HTTP_GET, handleStatus);
  webServer.on("/api/scan", HTTP_GET, handleScan);
  webServer.on("/api/qr.svg", HTTP_GET, handleQr);
  webServer.on("/api/options", HTTP_POST, handleOptions);
  webServer.on("/api/auth", HTTP_POST, handleAuth);
  webServer.on("/save", HTTP_POST, handleSaveNetwork);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
}

// byUser: the top button was pressed. Setup mode entered on its own (lost or
// failed Wi-Fi) keeps retrying the saved network in the background; one the
// user asked for stays put until they save.
void startSetupMode(bool byUser) {
  inSetupMode = true;
  setupRequestedByUser = byUser;
  setupSince = millis();
  applyOtaState();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_AP_NAME);
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());

  Serial.printf("Setup mode: connect to Wi-Fi \"%s\", then visit http://%s\n", SETUP_AP_NAME,
                WiFi.softAPIP().toString().c_str());
  if (!setupScreenShown) renderSetupScreen(); // skip the 15-30s redraw on a retry
}

// Attempts to connect using saved credentials. Falls back to setup mode if
// there are none, or if the connection attempt times out.
void connectWiFi() {
  if (savedSsid.length() == 0) {
    Serial.println("No saved Wi-Fi credentials, entering setup mode.");
    startSetupMode(false);
    return;
  }

  bool wasInSetup = inSetupMode;
  if (wasInSetup) dnsServer.stop();
  inSetupMode = false; // leaving AP mode (if we were in it) to attempt a real connection
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());

  Serial.printf("Connecting to Wi-Fi: %s\n", savedSsid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin(MDNS_HOSTNAME); // also enables outgoing .local resolution
    MDNS.addService("http", "tcp", 80);
    volumioFailures = 0;
    applyOtaState();
    if (wasInSetup || setupScreenShown) lastRenderedKey = "\x01__BOOT__\x01"; // replace the QR screen
  } else {
    Serial.println("\nWi-Fi connect failed, entering setup mode.");
    startSetupMode(false);
  }
}

// Short press of the top button: setup mode on demand, so the network or
// Volumio host can be changed at any time, even while connected.
void enterSetupMode() {
  Serial.println("Top button: entering Wi-Fi setup mode.");
  setupScreenShown = false; // always redraw on an explicit press
  startSetupMode(true);
}

// Long press (5s) of the top button: removes the web password, the way back
// in when it's forgotten. Needs hands on the device, so it can't be done remotely.
void resetWebPassword() {
  Serial.println("Top button held: removing web password.");
  saveWebAuth(false, "");
  showStatusLines("Web password", "removed", "");
  delay(1500);
  if (inSetupMode) {
    renderSetupScreen();
  } else {
    lastRenderedKey = "\x01__BOOT__\x01"; // redraw now playing on the next poll
    lastPollTime = 0;
  }
}

// ---------------------------------------------------------------------------
// Volumio
// ---------------------------------------------------------------------------

// Percent-encodes a string for safe use as a URL query parameter value.
String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    bool unreserved = isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Generic resolution: external absolute URLs go through the wsrv.nl proxy,
// which re-encodes as baseline JPEG (streaming CDNs commonly serve progressive
// JPEG, which M5GFX's embedded decoder can't handle) and pre-scales to our art
// box size, shrinking the download too. Relative paths are Volumio-local art,
// fetched directly.
String genericResolveArtUrl(const String& albumart) {
  if (albumart.length() == 0) return "";
  if (albumart.startsWith("http://") || albumart.startsWith("https://")) {
    return "https://wsrv.nl/?url=" + urlEncode(albumart) +
           "&w=" + String(ART_SIZE) + "&h=" + String(ART_SIZE) +
           "&fit=cover&output=jpg";
  }
  String base = "http://" + savedVolumioHost + ":" + String(VOLUMIO_PORT);
  if (!albumart.startsWith("/")) base += "/";
  return base + albumart;
}

// Resolves Volumio's (possibly relative) albumart path into a fetchable URL,
// dispatched by Volumio's "service" field (the active playback plugin) so
// each source has a clear spot for its own quirks. Only Tidal-via-external-URL
// has actually been exercised so far; the rest fall through to the generic
// path untested — verify against real playback before relying on them.
String resolveAlbumArtUrl(const String& albumart, const String& service) {
  if (service == "mpd") {
    // Local library / USB / NAS playback. Volumio typically serves art via
    // its own relative /albumart path, extracted from file tags — generic
    // path should already cover this. TODO: verify once tested; local file
    // tag art with no embedded cover may need a placeholder fallback.
    return genericResolveArtUrl(albumart);
  }
  if (service == "spop" || service == "volspotconnect2") {
    // Spotify Connect. Art normally comes from Spotify's i.scdn.co CDN as an
    // absolute URL — generic external-URL path (wsrv.nl proxy) should apply.
    // TODO: verify once tested; confirm Spotify's JPEGs are baseline (if so,
    // the proxy hop could be skipped for this service to save a round-trip).
    return genericResolveArtUrl(albumart);
  }
  // Default: covers Tidal (verified working) and any other/future service
  // (Qobuz, webradio, AirPlay, etc.) via the same generic logic.
  return genericResolveArtUrl(albumart);
}

// Maps Volumio's internal service identifier to something worth putting on
// screen. Falls back to the raw value (capitalized) for anything unlisted,
// rather than hiding it — better to show an unfamiliar label than nothing.
String friendlyServiceName(const String& service) {
  if (service == "mpd") return "Local";
  if (service == "spop" || service == "volspotconnect2") return "Spotify";
  if (service == "tidal") return "Tidal";
  if (service == "webradio") return "Web Radio";
  if (service.length() == 0) return "";
  String out = service;
  out.setCharAt(0, toupper(out[0]));
  return out;
}

// Builds the small "audio nerd" line — platform, format/quality, bitrate —
// from whatever Volumio's getState actually populated. Only Tidal has been
// exercised so far; other services may leave some of these fields empty.
String buildMetaLine(const String& service, const String& trackType,
                      const String& samplerate, const String& bitdepth,
                      const String& bitrate) {
  String parts[3];
  int n = 0;

  // Some services (Tidal via the mpd plugin, at least) report their own name
  // as trackType instead of an actual codec — showing the platform segment
  // too would just repeat it (e.g. "Tidal · TIDAL 44.1kHz/16bit"). Skip the
  // platform segment in that case; trackType already conveys the source.
  // Volumio 4 plays streaming services through MPD and reports them as
  // service "mpd" with the real source in trackType (e.g. "tidal"), which
  // would otherwise be labeled "Local".
  String effectiveService = service;
  String tt = trackType; tt.toLowerCase();
  if (service == "mpd" && (tt == "tidal" || tt == "qobuz" || tt == "spotify")) effectiveService = tt;
  String platform = friendlyServiceName(effectiveService);
  bool trackTypeIsPlatform = platform.length() && trackType.equalsIgnoreCase(platform);
  if (platform.length() && !trackTypeIsPlatform) parts[n++] = platform;

  String fmt = trackType;
  fmt.toUpperCase();
  String quality = samplerate;
  if (bitdepth.length()) quality += (quality.length() ? "/" : "") + bitdepth;
  if (fmt.length() || quality.length()) {
    parts[n++] = fmt + (fmt.length() && quality.length() ? " " : "") + quality;
  }

  if (bitrate.length()) {
    String br = bitrate;
    String brLower = br; brLower.toLowerCase();
    if (brLower.indexOf("kbps") == -1) br += " kbps";
    parts[n++] = br;
  }

  String meta;
  for (int i = 0; i < n; i++) {
    if (meta.length()) meta += "  ·  ";
    meta += parts[i];
  }
  return meta;
}

bool fetchState(JsonDocument& doc) {
  HTTPClient http;
  String url = "http://" + savedVolumioHost + ":" + String(VOLUMIO_PORT) + "/api/v1/getState";
  http.begin(url);
  http.setTimeout(4000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("getState failed, HTTP %d\n", code);
    http.end();
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    return false;
  }
  return true;
}

// Fires a Volumio transport command (e.g. "next", "prev") and doesn't wait
// for a re-render — the next poll cycle will pick up the resulting state
// change and redraw normally.
void sendVolumioCommand(const char* cmd) {
  HTTPClient http;
  String url = "http://" + savedVolumioHost + ":" + String(VOLUMIO_PORT) +
               "/api/v1/commands/?cmd=" + String(cmd);
  http.begin(url);
  http.setTimeout(4000);
  int code = http.GET();
  Serial.printf("Command \"%s\" -> HTTP %d\n", cmd, code);
  http.end();
}

// Fetches raw album art bytes (JPEG or PNG) into a PSRAM buffer.
// Caller owns the returned buffer and must free() it. Returns nullptr on failure.
//
// Handles responses without a Content-Length (chunked transfer encoding, as
// Volumio 4's Node/Express stack can send for /albumart) by reading until the
// connection closes, and follows redirects (Volumio answers a cache miss on
// /albumart with a redirect to its fallback image).
static const size_t ART_MAX_BYTES = 2 * 1024 * 1024;

uint8_t* fetchAlbumArt(const String& url, size_t& outLen) {
  if (url.length() == 0) return nullptr;

  // Declaration order matters: locals are destroyed in reverse, and on
  // Arduino-ESP32 3.x ~HTTPClient() calls stop() on its client. The clients
  // must outlive `http`, or that call jumps through a dead vtable.
  WiFiClient plainClient;
  WiFiClientSecure tlsClient;
  HTTPClient http;
  bool ok;
  if (url.startsWith("https://")) {
    tlsClient.setInsecure(); // public album art; no secrets on this connection
    ok = http.begin(tlsClient, url);
  } else {
    ok = http.begin(plainClient, url);
  }
  if (!ok) {
    Serial.println("  art: http.begin() failed");
    return nullptr;
  }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
  http.setTimeout(8000);
  http.setConnectTimeout(8000);
  const char* hdrs[] = {"Content-Type", "Transfer-Encoding"};
  http.collectHeaders(hdrs, 2);

  int code = http.GET();
  Serial.printf("  art HTTP GET -> code=%d\n", code);
  if (code != HTTP_CODE_OK) {
    Serial.printf("Album art fetch failed, HTTP %d (%s)\n", code,
                  code < 0 ? http.errorToString(code).c_str() : "");
    http.end();
    return nullptr;
  }

  int len = http.getSize(); // -1 when the server sends no Content-Length
  Serial.printf("  art content-length=%d type=%s transfer-encoding=%s\n", len,
                http.header("Content-Type").c_str(), http.header("Transfer-Encoding").c_str());
  if (len == 0 || len > (int)ART_MAX_BYTES) {
    http.end();
    return nullptr;
  }

  size_t cap = len > 0 ? (size_t)len : 64 * 1024;
  uint8_t* buf = (uint8_t*)ps_malloc(cap);
  if (!buf) {
    Serial.println("ps_malloc failed for album art buffer");
    http.end();
    return nullptr;
  }

  // writeToStream() handles both Content-Length and chunked bodies (it
  // de-chunks for us). This sink grows the PSRAM buffer as data arrives and
  // keeps OTA and the watchdog serviced during a slow transfer.
  class BufSink : public Stream {
   public:
    uint8_t*& buf; size_t& cap; size_t used = 0; bool oom = false;
    BufSink(uint8_t*& b, size_t& c) : buf(b), cap(c) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* data, size_t n) override {
      if (oom) return 0;
      if (used + n > cap) {
        size_t ncap = cap;
        while (used + n > ncap) ncap *= 2;
        if (ncap > ART_MAX_BYTES) { oom = true; return 0; }
        uint8_t* nb = (uint8_t*)ps_realloc(buf, ncap);
        if (!nb) { oom = true; return 0; }
        buf = nb; cap = ncap;
      }
      memcpy(buf + used, data, n);
      used += n;
      serviceNetwork(); // keep the dashboard answering during a slow download
      return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
  } sink(buf, cap);

  int written = http.writeToStream(&sink);
  http.end();

  if (written < 0 || sink.oom || sink.used == 0 || (len > 0 && sink.used != (size_t)len)) {
    Serial.printf("Album art incomplete: %u bytes (expected %d, writeToStream=%d, oom=%d)\n",
                  (unsigned)sink.used, len, written, sink.oom);
    free(buf);
    return nullptr;
  }
  Serial.printf("  art download complete: %u bytes, first bytes: %02X %02X %02X %02X\n",
                (unsigned)sink.used, buf[0], buf[1], buf[2], buf[3]);

  outLen = sink.used;
  return buf;
}

// ---------------------------------------------------------------------------
// Album art cache + prefetch
//
// Downloading the cover (TLS to the wsrv.nl proxy, which fetches and
// re-encodes it) is the slowest step before the e-paper refresh, so Liner
// keeps the last few covers in PSRAM. Every track of an album shares one
// cover. For playlists, where covers differ track to track, Liner fetches the
// next two queued tracks' covers once a redraw finishes, while the current
// track plays, so a track change (or a quick double skip) can go straight to
// the panel. Going back one track usually hits the cache too.
// ---------------------------------------------------------------------------

struct ArtEntry {
  String url;
  uint8_t* data = nullptr;
  size_t len = 0;
  uint32_t used = 0; // millis() of last use, for least-recently-used eviction
};
// Previous + current + next two, ~50 KB each from the proxy; PSRAM has room.
static const int ART_CACHE_SLOTS = 4;
static const int PREFETCH_AHEAD = 2;
ArtEntry artCache[ART_CACHE_SLOTS];

ArtEntry* artCacheFind(const String& url) {
  for (auto& e : artCache) {
    if (e.data && e.url == url) {
      e.used = millis();
      return &e;
    }
  }
  return nullptr;
}

// The cover for url, from the cache or downloaded into it. The cache owns the
// buffer, so callers must not free it. *fromCache reports a cache hit.
ArtEntry* getAlbumArt(const String& url, bool* fromCache = nullptr) {
  if (fromCache) *fromCache = false;
  if (url.length() == 0) return nullptr;
  if (ArtEntry* hit = artCacheFind(url)) {
    if (fromCache) *fromCache = true;
    return hit;
  }
  size_t len = 0;
  uint8_t* data = fetchAlbumArt(url, len);
  if (!data) return nullptr;
  ArtEntry* slot = &artCache[0];
  for (auto& e : artCache) {
    if (!e.data) { slot = &e; break; }
    if (e.used < slot->used) slot = &e;
  }
  free(slot->data);
  slot->url = url;
  slot->data = data;
  slot->len = len;
  slot->used = millis();
  return slot;
}

// Set after each now-playing redraw: the queue position on screen. loop()
// then calls prefetchNextArt() once the panel is idle. Skipped on shuffle,
// where Volumio's next track isn't the next queue entry.
bool prefetchPending = false;
int prefetchPosition = -1;

void prefetchNextArt() {
  prefetchPending = false;
  HTTPClient http;
  http.begin("http://" + savedVolumioHost + ":" + String(VOLUMIO_PORT) + "/api/v1/getQueue");
  http.setTimeout(4000);
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return;
  }
  // Keep only what's needed: a long queue can be a big response.
  JsonDocument filter;
  filter["queue"][0]["albumart"] = true;
  filter["queue"][0]["service"] = true;
  JsonDocument q;
  DeserializationError err = deserializeJson(q, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return;
  JsonArray queue = q["queue"].as<JsonArray>();
  if (prefetchPosition < 0) return;
  for (int next = prefetchPosition + 1;
       next <= prefetchPosition + PREFETCH_AHEAD && next < (int)queue.size(); next++) {
    String url = resolveAlbumArtUrl(queue[next]["albumart"] | "", queue[next]["service"] | "");
    if (url.length() == 0 || artCacheFind(url)) continue; // e.g. same album: already cached
    uint32_t t = millis();
    bool ok = getAlbumArt(url) != nullptr;
    Serial.printf("Prefetched cover for queue %d: %s in %lu ms\n", next, ok ? "ok" : "failed",
                  (unsigned long)(millis() - t));
    webServer.handleClient(); // between downloads, let the dashboard in
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Simple word-wrap: draws text within a width, returns the y position after the last line.
int drawWrapped(const String& text, int x, int y, int maxWidth, int lineHeight) {
  if (text.length() == 0) return y;
  String line;
  int cursorY = y;
  int start = 0;
  while (start < (int)text.length()) {
    int spaceIdx = text.indexOf(' ', start);
    String word = (spaceIdx == -1) ? text.substring(start) : text.substring(start, spaceIdx);
    String candidate = line.length() ? (line + " " + word) : word;
    if (M5.Display.textWidth(candidate) > maxWidth && line.length() > 0) {
      M5.Display.drawString(line, x, cursorY);
      cursorY += lineHeight;
      line = word;
    } else {
      line = candidate;
    }
    if (spaceIdx == -1) break;
    start = spaceIdx + 1;
  }
  if (line.length()) {
    M5.Display.drawString(line, x, cursorY);
    cursorY += lineHeight;
  }
  return cursorY;
}

void renderIdle() {
  setupScreenShown = false;
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);
  M5.Display.setTextDatum(textdatum_t::middle_center);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  M5.Display.drawString("Nothing Playing", PANEL_W / 2, PANEL_H / 2);
  M5.Display.endWrite();
  M5.Display.display();
  waitPanel();
}

// Idle screensaver: a random abstract pattern in the Spectra 6 palette,
// shown after a long stretch of nothing playing (see SCREENSAVER_DELAY_MS).
// Purely decorative — no text, since it's not conveying information.
void renderScreensaver() {
  setupScreenShown = false;
  static const uint16_t palette[] = {TFT_BLACK, TFT_RED, TFT_YELLOW, TFT_BLUE, TFT_GREEN};
  static const int paletteCount = sizeof(palette) / sizeof(palette[0]);

  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);

  switch (random(3)) {
    case 0: { // scattered shapes
      for (int i = 0; i < 16; i++) {
        int x = random(0, PANEL_W);
        int y = random(0, PANEL_H);
        int size = random(30, 120);
        uint16_t color = palette[random(paletteCount)];
        if (random(2) == 0) {
          M5.Display.fillCircle(x, y, size / 2, color);
        } else {
          M5.Display.fillRect(x - size / 2, y - size / 2, size, size, color);
        }
      }
      break;
    }
    case 1: { // concentric rings from center, largest first so each is visible
      int cx = PANEL_W / 2, cy = PANEL_H / 2;
      for (int r = 340; r > 0; r -= 32) {
        M5.Display.fillCircle(cx, cy, r, palette[random(paletteCount)]);
      }
      break;
    }
    case 2: { // diagonal bands
      int bandWidth = 55;
      for (int x = -PANEL_H; x < PANEL_W; x += bandWidth) {
        uint16_t color = palette[random(paletteCount)];
        M5.Display.fillTriangle(x, 0, x + bandWidth, 0, x + bandWidth + PANEL_H, PANEL_H, color);
        M5.Display.fillTriangle(x, 0, x + bandWidth + PANEL_H, PANEL_H, x + PANEL_H, PANEL_H, color);
      }
      break;
    }
  }

  M5.Display.endWrite();
  M5.Display.display();
  waitPanel();
}

// Returns true if album art was drawn.
bool renderNowPlaying(const String& title, const String& artist, const String& album,
                       const String& artUrl, const String& metaLine) {
  panelBusy = true; // cleared by waitPanel() once the refresh finishes
  setupScreenShown = false;
  M5.Display.startWrite();
  M5.Display.fillScreen(TFT_WHITE);

  // Album art, top full-width square.
  bool artDrawn = false;
  uint32_t t = millis();
  if (artUrl.length()) {
    bool cached = false;
    ArtEntry* entry = getAlbumArt(artUrl, &cached);
    feedWatchdog();
    lastTiming.artMs = millis() - t;
    lastTiming.artCached = cached;
    t = millis();
    if (entry) {
      const uint8_t* art = entry->data;
      size_t artLen = entry->len;
      lastTiming.artBytes = artLen;
      // Volumio-local art is served as-is (any size, JPEG or PNG); proxied
      // art is already a 400px baseline JPEG. scale 0 = fit to the art box.
      bool isPng = artLen > 4 && art[0] == 0x89 && art[1] == 'P' && art[2] == 'N' && art[3] == 'G';
      bool ok = isPng
          ? M5.Display.drawPng(art, artLen, 0, 0, ART_SIZE, ART_SIZE, 0, 0, 0.0f, 0.0f)
          : M5.Display.drawJpg(art, artLen, 0, 0, ART_SIZE, ART_SIZE, 0, 0, 0.0f, 0.0f);
      Serial.printf("  draw%s(len=%u, %s) -> %s\n", isPng ? "Png" : "Jpg", (unsigned)artLen,
                    cached ? "cached" : "downloaded", ok ? "OK" : "FAILED");
      if (!ok) { // don't keep a cover that can't be decoded
        free(entry->data);
        entry->data = nullptr;
        entry->url = "";
      }
      artDrawn = ok;
    }
  }
  lastTiming.decodeMs = millis() - t;
  t = millis();
  if (!artDrawn) {
    M5.Display.fillRect(0, 0, ART_SIZE, ART_SIZE, TFT_LIGHTGRAY);
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.setTextColor(TFT_BLACK, TFT_LIGHTGRAY);
    M5.Display.setFont(&fonts::FreeSans12pt7b);
    M5.Display.drawString("No Album Art", ART_SIZE / 2, ART_SIZE / 2);
  }

  // Text block below the art.
  int textX = 16;
  int textWidth = PANEL_W - 32;
  int y = ART_SIZE + 20;

  M5.Display.setTextDatum(textdatum_t::top_left);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  M5.Display.setFont(&fonts::FreeSansBold18pt7b);
  y = drawWrapped(title, textX, y, textWidth, 30);

  y += 6;
  M5.Display.setFont(&fonts::FreeSans12pt7b);
  y = drawWrapped(artist, textX, y, textWidth, 24);

  y += 4;
  M5.Display.setFont(&fonts::FreeSans9pt7b);
  M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
  y = drawWrapped(album, textX, y, textWidth, 20);

  // Platform / format / bitrate — for the audio nerds. Bold sets it apart
  // from the plain album line above. Only shown when Volumio actually
  // populated something; not every service fills in all of these fields.
  if (metaLine.length()) {
    y += 8;
    M5.Display.setFont(&fonts::FreeSansBold12pt7b);
    M5.Display.setTextColor(TFT_BLACK, TFT_WHITE);
    drawWrapped(metaLine, textX, y, textWidth, 24);
  }

  M5.Display.endWrite();
  lastTiming.textMs = millis() - t;
  feedWatchdog();
  t = millis();
  waitPanel(); // a still-running previous refresh; display() would otherwise block on it
  lastTiming.waitPrevMs = millis() - t;
  panelBusy = true;
  t = millis();
  M5.Display.display(); // dither + send the image; the refresh then runs on its own
  lastTiming.transferMs = millis() - t;
  t = millis();
  waitPanel();
  lastTiming.refreshMs = millis() - t;
  lastTiming.at = millis() / 1000;
  feedWatchdog();
  return artDrawn;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void setup() {
  // Serial first, before anything that could hang, so we always get SOME output.
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] Liner v%s\n", LINER_VERSION);
  Serial.printf("[boot] reset reason = %d\n", (int)esp_reset_reason()); // 6/7 = watchdog
  randomSeed(esp_random()); // hardware TRNG, for the idle screensaver patterns

  auto cfg = M5.config();
  Serial.println("[boot] calling M5.begin()...");
  M5.begin(cfg);
  Serial.println("[boot] M5.begin() returned");
  Serial.printf("[boot] board = %s\n", M5.getBoard() == m5::board_t::board_M5PaperColor ? "PaperColor (detected correctly)" : "NOT PaperColor!");

  M5.Display.setRotation(0); // native portrait for PaperColor
  // Every draw here calls display() itself, after waitPanel(). With auto
  // display on, endWrite() would also start a refresh, blocking (without
  // serving the dashboard) on any refresh still running.
  M5.Display.setAutoDisplay(false);
  Serial.println("[boot] display rotation set, drawing status...");

  loadSettings();
  setupOta();

  showStatus("Connecting to Wi-Fi...");
  Serial.println("[boot] status drawn, connecting wifi...");
  connectWiFi();

  // Started after connectWiFi() so a network interface (STA or the AP from
  // setup mode) is already up first — starting it any earlier stalled boot
  // entirely, with no crash/error logged, just a silent hang.
  startWebServer();

  if (WiFi.status() == WL_CONNECTED) {
    showStatus("Connected. Waiting for Volumio...");
  } else if (!inSetupMode) {
    showStatus("Wi-Fi failed. Retrying...");
  }
  // if inSetupMode, connectWiFi() already drew the setup-mode screen

  startWatchdog();
}

uint32_t idleSince = 0;           // 0 = not currently idle
uint32_t lastScreensaverRender = 0;

void loop() {
  // M5.update() drives button edge-detection and must run on every loop
  // iteration — a blocking delay() here (as this used to have) would swallow
  // any tap that starts and ends between polls, since wasPressed() only sees
  // transitions across successive update() calls.
  M5.update();
  feedWatchdog();

  // Button mapping (per M5Unified's PaperColor pin table): BtnA=G10 (up),
  // BtnB=G9 (down), BtnC=G1 (top).
  if (M5.BtnA.wasPressed()) {
    Serial.println("Up button: next track");
    sendVolumioCommand("next");
  }
  if (M5.BtnB.wasPressed()) {
    Serial.println("Down button: previous track");
    sendVolumioCommand("prev");
  }
  // Top button acts on release: a 5s hold removes the web password, a
  // shorter press opens setup mode.
  if (M5.BtnC.wasReleaseFor(5000)) {
    if (webAuthEnabled) resetWebPassword();
    else enterSetupMode();
  } else if (M5.BtnC.wasReleased()) {
    enterSetupMode();
  }

  // The web server runs continuously in both AP and STA mode — see the note
  // above the Wi-Fi/Volumio setup section — so this is unconditional.
  webServer.handleClient();
  if (inSetupMode) dnsServer.processNextRequest();

  applyOtaState();
  if (otaRunning) ArduinoOTA.handle();

  if (pendingReconnect) {
    pendingReconnect = false;
    showStatusLines("Connecting to", savedSsid, "...");
    connectWiFi(); // handleSaveSettings() already saved the new credentials
    lastRenderedKey = "\x01__BOOT__\x01"; // force a fresh render once connected
  }

  if (inSetupMode) {
    // Wi-Fi dropped or failed on its own: quietly retry the saved network now
    // and then, as long as nobody is connected to the setup AP using it.
    if (!setupRequestedByUser && savedSsid.length() &&
        millis() - setupSince > SETUP_RETRY_MS && WiFi.softAPgetStationNum() == 0) {
      Serial.println("Setup mode: retrying saved Wi-Fi");
      connectWiFi(); // back to setup mode (screen untouched) if it fails again
    }
    delay(2);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
    delay(10);
    return;
  }

  // Volumio polling is time-gated instead of blocking, so buttons stay responsive.
  // The panel is idle here (redraws finish before loop() moves on), so this
  // is when the next track's cover gets fetched.
  if (prefetchPending) prefetchNextArt();

  uint32_t pollInterval = volumioFailures >= VOLUMIO_FAIL_THRESHOLD ? VOLUMIO_RETRY_MS : POLL_INTERVAL_MS;
  if (millis() - lastPollTime < pollInterval) {
    delay(10);
    return;
  }
  lastPollTime = millis();

  JsonDocument doc;
  bool gotState = fetchState(doc);
  if (!gotState) {
    volumioReachable = false;
    if (++volumioFailures == VOLUMIO_FAIL_THRESHOLD) {
      Serial.println("Volumio unreachable, showing network screen");
      cachedPlaying = false;
      idleSince = 0;
      renderNetworkIssue();
      lastRenderedKey = "\x01__NETERR__\x01"; // any real state redraws over it
    }
  } else {
    volumioFailures = 0;
    volumioReachable = true;
    String status     = doc["status"]     | "";
    String title      = doc["title"]      | "";
    String artist     = doc["artist"]     | "";
    String album      = doc["album"]      | "";
    String art        = doc["albumart"]   | "";
    String service    = doc["service"]    | "";
    String trackType  = doc["trackType"]  | "";
    String samplerate = doc["samplerate"] | "";
    String bitdepth   = doc["bitdepth"]   | "";
    String bitrate    = doc["bitrate"]    | "";

    bool playing = (status == "play") && title.length() > 0;
    String key = playing ? (title + "|" + artist + "|" + album) : "__IDLE__";

    // Refreshed every poll regardless of whether the e-paper redraws, so the
    // web portal's Now Playing section stays current without its own
    // separate call to Volumio.
    cachedPlaying = playing;
    if (playing) {
      cachedTitle = title;
      cachedArtist = artist;
      cachedAlbum = album;
      cachedArtUrl = resolveAlbumArtUrl(art, service);
      cachedMetaLine = buildMetaLine(service, trackType, samplerate, bitdepth, bitrate);
    }

    bool artRetry = playing && key == lastRenderedKey && !lastRenderArtOk &&
                    cachedArtUrl.length() && cachedArtUrl != lastRenderedArtUrl;
    if (artRetry) Serial.println("Album art changed after a failed draw, redrawing");

    if (key != lastRenderedKey || artRetry) {
      Serial.printf("Track change -> %s (service=%s)\n", key.c_str(), service.c_str());
      Serial.printf("  raw albumart field: \"%s\"\n", art.c_str());
      if (playing) {
        Serial.printf("  resolved art URL: \"%s\"\n", cachedArtUrl.c_str());
        Serial.printf("  meta line: \"%s\"\n", cachedMetaLine.c_str());
        lastTiming = RenderTiming();
        lastTiming.lagMs = doc["seek"] | 0;
        lastRenderArtOk = renderNowPlaying(title, artist, album, cachedArtUrl, cachedMetaLine);
        lastRenderedArtUrl = cachedArtUrl;
        idleSince = 0;
        prefetchPosition = doc["position"] | -1;
        prefetchPending = !(doc["random"] | false);
      } else {
        renderIdle();
        idleSince = millis();
        lastScreensaverRender = 0;
      }
      lastRenderedKey = key;
    }

    // Screensaver: the state-change check above only fires once on entering
    // idle, since "__IDLE__" never itself changes — this timer-based check
    // is what keeps cycling patterns during a long continuous idle stretch.
    if (screensaverEnabled && !playing && idleSince != 0) {
      uint32_t idleFor = millis() - idleSince;
      uint32_t sinceLastSS = millis() - lastScreensaverRender;
      if (idleFor > SCREENSAVER_DELAY_MS &&
          (lastScreensaverRender == 0 || sinceLastSS > SCREENSAVER_INTERVAL_MS)) {
        Serial.println("Idle screensaver: rendering new pattern");
        renderScreensaver();
        lastScreensaverRender = millis();
      }
    }
  }

  delay(10);
}
