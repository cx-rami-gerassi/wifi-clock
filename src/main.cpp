#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <time.h>

// ---------- CONFIG ----------
// POSIX timezone string. Default: Israel (handles DST automatically).
// Find others at: https://github.com/nayarsystems/posix_tz_db
#define TZ_INFO "IST-2IDT,M3.4.4/26,M10.5.0"

// Name of the temporary network the clock creates for first-time WiFi setup.
#define AP_SSID "WiFi-Clock"
// --------------------------------------------------

#define SDA_PIN 5
#define SCL_PIN 6

// On-board "BOOT" button (GPIO9, active-low INPUT_PULLUP). Short press cycles
// display modes; long press (>=1.5s) wipes saved WiFi and reboots into setup.
// NOTE: holding it during a *reset* enters firmware-download mode, so setup is
// triggered by a runtime long-press, never by a boot-hold.
#define BTN_PIN 9
#define LONG_PRESS_MS 1500

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN);

// Display screens, cycled by short-pressing the BOOT button.
enum Mode { MODE_CLOCK, MODE_DATE, MODE_SECONDS, MODE_UPTIME, MODE_WIFI, MODE_COUNT };
static Mode mode = MODE_CLOCK;

// App phase: either serving the setup portal, or running the clock.
enum AppState { ST_PORTAL, ST_RUN };
static AppState appState = ST_RUN;

Preferences prefs;          // persists WiFi creds in NVS (namespace "wifi")
WebServer server(80);       // setup web page
DNSServer dns;              // captive-portal redirect during setup
String wifiSsid, wifiPass;  // current credentials (loaded from NVS)

// ---------- Small drawing helpers ----------

// Draw a string horizontally centered on the 72px-wide display.
static void drawCentered(int y, const char *s) {
  int w = u8g2.getStrWidth(s);
  int x = (72 - w) / 2;
  if (x < 0) x = 0;
  u8g2.drawStr(x, y, s);
}

// Draw a string flush against the right edge of the 72px display.
static void drawRight(int y, const char *s) {
  int x = 72 - u8g2.getStrWidth(s);
  if (x < 0) x = 0;
  u8g2.drawStr(x, y, s);
}

static void showMessage(const char *line1, const char *line2 = nullptr) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  drawCentered(18, line1);
  if (line2) drawCentered(34, line2);
  u8g2.sendBuffer();
}

// Setup-mode instructions screen.
static void showPortalScreen() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(7, "SETUP MODE");
  u8g2.drawStr(0, 16, "Join WiFi:");
  drawCentered(24, AP_SSID);
  u8g2.drawStr(0, 32, "then open");
  drawCentered(40, "192.168.4.1");
  u8g2.sendBuffer();
}

// ---------- Credential storage (NVS) ----------

static void loadCreds() {
  prefs.begin("wifi", true);  // read-only
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  prefs.end();
}

static void saveCreds(const String &ssid, const String &pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

// ---------- Setup web portal ----------

// Minimal HTML escaping for SSIDs shown on the page.
static String esc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

static void handleRoot() {
  int n = WiFi.scanNetworks();  // scan while in AP_STA mode
  String opts = "<option value=''>-- choose a network --</option>";
  for (int i = 0; i < n; i++) {
    String s = esc(WiFi.SSID(i));
    opts += "<option value='" + s + "'>" + s + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }

  String page =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>WiFi Clock setup</title>"
    "<style>body{font-family:sans-serif;margin:1.2em;max-width:420px}"
    "h2{margin-top:0}label{display:block;margin:.8em 0 .2em;font-weight:bold}"
    "select,input{width:100%;padding:.5em;font-size:1em;box-sizing:border-box}"
    "button{margin-top:1.2em;padding:.7em;width:100%;font-size:1em}</style></head><body>"
    "<h2>WiFi Clock setup</h2>"
    "<form action='/save' method='POST'>"
    "<label>Pick a network</label><select name='ssid_pick'>" + opts + "</select>"
    "<label>&hellip; or type a name (hidden networks)</label>"
    "<input name='ssid_manual' placeholder='Network name'>"
    "<label>Password</label>"
    "<input name='pass' type='password' placeholder='WiFi password'>"
    "<button type='submit'>Save &amp; connect</button>"
    "</form><p style='color:#666;font-size:.85em'>A typed name overrides the dropdown. "
    "The clock is 2.4&nbsp;GHz only.</p></body></html>";

  server.send(200, "text/html", page);
}

static void handleSave() {
  String pick = server.arg("ssid_pick");
  String man = server.arg("ssid_manual");
  man.trim();
  String ssid = man.length() ? man : pick;  // manual entry wins
  String pass = server.arg("pass");

  if (ssid.length() == 0) {
    server.send(200, "text/html",
                "<p>No network chosen. <a href='/'>Go back</a></p>");
    return;
  }

  saveCreds(ssid, pass);
  server.send(200, "text/html",
              "<!doctype html><meta charset='utf-8'>"
              "<h2>Saved!</h2><p>Connecting to <b>" + esc(ssid) +
              "</b>. The clock will restart now.</p>");
  Serial.printf("Saved creds for '%s'; restarting.\n", ssid.c_str());
  delay(1500);
  ESP.restart();
}

// Captive-portal: send any other request back to the setup page.
static void handleNotFound() {
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "");
}

static void startPortal() {
  appState = ST_PORTAL;
  WiFi.mode(WIFI_AP_STA);  // AP for the phone + STA so we can scan networks
  WiFi.softAP(AP_SSID);
  // Lower TX power softens the current spike that can brown-out the OLED.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  IPAddress ip = WiFi.softAPIP();  // 192.168.4.1
  dns.start(53, "*", ip);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  showPortalScreen();
  Serial.printf("Setup portal up: join '%s', open http://%s/\n",
                AP_SSID, ip.toString().c_str());
}

// Try to join the saved network within timeoutMs. Shows progress on the OLED.
static bool tryConnect(uint32_t timeoutMs) {
  showMessage("WiFi", "connecting");
  Serial.printf("Connecting to %s", wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > timeoutMs) {
      Serial.println("\nConnect timed out.");
      return false;
    }
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nConnected, IP: %s  RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

// ---------- Button ----------

// Debounced edge-detect on the BOOT button. Short press -> next screen.
// Long press (>=LONG_PRESS_MS) opens the setup portal, but ONLY from the WiFi
// screen, so it's deliberate -- and it leaves the saved network untouched, so
// backing out (power-cycle) still reconnects to it. A long press on any other
// screen is ignored.
static void pollButton() {
  static bool stable = HIGH, lastReading = HIGH;
  static unsigned long lastChange = 0, pressStart = 0;

  bool reading = digitalRead(BTN_PIN);
  unsigned long now = millis();
  if (reading != lastReading) {
    lastReading = reading;
    lastChange = now;
  }
  if (now - lastChange > 25 && reading != stable) {
    stable = reading;
    if (stable == LOW) {           // pressed
      pressStart = now;
    } else {                       // released
      if (now - pressStart >= LONG_PRESS_MS) {
        if (mode == MODE_WIFI) startPortal();  // setup only from WiFi screen;
                                               // saved creds left untouched
      } else {
        mode = (Mode)((mode + 1) % MODE_COUNT);
      }
    }
  }
}

// ---------- Screen renderers (draw into buffer; caller flushes) ----------

// MODE_CLOCK: big HH:MM with a blinking colon; date (left) + seconds (right).
static void drawClock(const struct tm &t) {
  char hhStr[3], mmStr[3];
  snprintf(hhStr, sizeof(hhStr), "%02d", t.tm_hour);
  snprintf(mmStr, sizeof(mmStr), "%02d", t.tm_min);
  char dateStr[6], secStr[3];
  strftime(dateStr, sizeof(dateStr), "%d/%m", &t);
  strftime(secStr, sizeof(secStr), "%S", &t);

  // Draw HH, colon, and MM at FIXED positions so the digits never shift; the
  // colon just blinks on/off once per second.
  u8g2.setFont(u8g2_font_logisoso20_tn);
  int wHH = u8g2.getStrWidth(hhStr);
  int wColon = u8g2.getStrWidth(":");
  int total = wHH + wColon + u8g2.getStrWidth(mmStr);
  int x = (72 - total) / 2;
  if (x < 0) x = 0;
  const int yTime = 23;
  u8g2.drawStr(x, yTime, hhStr);
  if (t.tm_sec % 2) u8g2.drawStr(x + wHH, yTime, ":");
  u8g2.drawStr(x + wHH + wColon, yTime, mmStr);

  u8g2.setFont(u8g2_font_5x8_tr);
  u8g2.drawStr(0, 39, dateStr);
  drawRight(39, secStr);
}

// MODE_DATE: weekday on top, big DD/MM in the middle, year at the bottom.
static void drawBigDate(const struct tm &t) {
  char dow[8], dm[6], yr[5];
  strftime(dow, sizeof(dow), "%a", &t);  // e.g. "Wed"
  strftime(dm, sizeof(dm), "%d/%m", &t);
  strftime(yr, sizeof(yr), "%Y", &t);

  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, dow);
  u8g2.setFont(u8g2_font_helvB14_tr);  // full font incl. '/'
  drawCentered(28, dm);
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(39, yr);
}

// MODE_SECONDS: a big, ticking seconds counter with a small label.
static void drawSeconds(const struct tm &t) {
  char secStr[3];
  strftime(secStr, sizeof(secStr), "%S", &t);
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, "SEC");
  u8g2.setFont(u8g2_font_logisoso28_tn);
  drawCentered(38, secStr);
}

// MODE_UPTIME: time since boot. Shows HH:MM:SS, or "Nd HH:MM" once past a day.
static void drawUptime() {
  unsigned long s = millis() / 1000;
  unsigned long days = s / 86400;
  s %= 86400;
  int hh = s / 3600;
  s %= 3600;
  int mm = s / 60;
  int ss = s % 60;

  char buf[16];
  if (days > 0) snprintf(buf, sizeof(buf), "%lud %02d:%02d", days, hh, mm);
  else          snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mm, ss);

  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, "UPTIME");
  u8g2.setFont(u8g2_font_7x13_tr);  // monospace, has ':' and 'd'
  drawCentered(28, buf);
}

// MODE_WIFI: connection status, network name, IP, and signal strength.
static void drawWifi() {
  u8g2.setFont(u8g2_font_5x8_tr);
  bool up = (WiFi.status() == WL_CONNECTED);
  drawCentered(8, up ? "WiFi OK" : "WiFi --");
  if (up) {
    char ssid[15];
    strncpy(ssid, WiFi.SSID().c_str(), sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';  // truncate long SSIDs to fit
    u8g2.drawStr(0, 18, ssid);
    u8g2.drawStr(0, 28, WiFi.localIP().toString().c_str());
    char r[12];
    snprintf(r, sizeof(r), "%d dBm", WiFi.RSSI());
    u8g2.drawStr(0, 38, r);
  } else {
    drawCentered(26, "no link");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC enumerate before logging
  pinMode(BTN_PIN, INPUT_PULLUP);  // BOOT button
  // Display is initialized exactly ONCE, here, before WiFi. Calling
  // u8g2.begin() again after the radio is up wedges the C3 (see NOTES.md).
  u8g2.begin();
  u8g2.setBusClock(100000);  // 100kHz I2C: more tolerant of supply noise

  loadCreds();

  if (wifiSsid.length() == 0) {
    // No network saved yet -> first-time setup.
    startPortal();
    return;
  }

  if (tryConnect(20000)) {
    // Kick off NTP. configTzTime applies the POSIX TZ rules (incl. DST). We do
    // NOT block here: the wait + retry lives in loop() so a dropped first SNTP
    // packet self-heals instead of freezing on "Syncing time...".
    showMessage("Syncing", "time...");
    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
    appState = ST_RUN;
  } else {
    // Saved creds didn't work (wrong password, network gone, 5GHz, ...).
    // Fall back to the setup portal so the user can fix it.
    startPortal();
  }
}

void loop() {
  // ---- Setup portal phase: just service the web page until creds are saved
  // (handleSave() reboots the device once they are). ----
  if (appState == ST_PORTAL) {
    dns.processNextRequest();
    server.handleClient();
    delay(5);
    return;
  }

  struct tm t;

  // ---- Initial NTP sync: non-blocking, self-healing ----
  // Until the clock has a valid time, show a live counter and re-request NTP
  // periodically. Re-calling configTzTime re-resolves DNS and re-sends the
  // request, so a lost first packet recovers on its own (no infinite freeze).
  static bool timeReady = false;
  if (!timeReady) {
    if (getLocalTime(&t, 100)) {
      timeReady = true;
      Serial.println("Time synced.");
      // NOTE: do NOT call u8g2.begin() again here. A second begin() after WiFi
      // is up wedges the device (hangs/resets), freezing the "Syncing" frame on
      // screen. The display was already initialized in setup() and works fine.
    } else {
      static unsigned long syncStart = 0, lastRetry = 0;
      unsigned long now = millis();
      if (syncStart == 0) { syncStart = now; lastRetry = now; }

      if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();

      if (now - lastRetry > 8000) {
        lastRetry = now;
        configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
        Serial.printf("Retrying NTP (%lus elapsed, RSSI %d dBm)\n",
                      (now - syncStart) / 1000, WiFi.RSSI());
      }

      char buf[16];
      snprintf(buf, sizeof(buf), "%lus", (now - syncStart) / 1000);
      showMessage("Syncing", buf);
      delay(250);
      return;
    }
  }

  pollButton();  // short press cycles modes; long press opens setup
  if (appState == ST_PORTAL) return;  // long-press just switched us to setup

  if (!getLocalTime(&t, 10)) {
    showMessage("Time", "lost");
    delay(1000);
    return;
  }

  // Redraw at ~4Hz (smooth colon blink / ticking seconds) but keep the loop
  // itself fast so the button stays responsive.
  static unsigned long lastDraw = 0;
  unsigned long now = millis();
  if (now - lastDraw >= 250) {
    lastDraw = now;
    u8g2.clearBuffer();
    switch (mode) {
      case MODE_CLOCK:   drawClock(t);   break;
      case MODE_DATE:    drawBigDate(t); break;
      case MODE_SECONDS: drawSeconds(t); break;
      case MODE_UPTIME:  drawUptime();   break;
      case MODE_WIFI:    drawWifi();     break;
      default:           drawClock(t);   break;
    }
    u8g2.sendBuffer();
  }

  delay(15);  // button sampling cadence (also the debounce resolution)
}
