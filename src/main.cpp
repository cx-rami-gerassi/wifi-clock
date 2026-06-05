#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ---------- CONFIG ----------
// POSIX timezone string. Default: Israel (handles DST automatically).
// Find others at: https://github.com/nayarsystems/posix_tz_db
#define TZ_INFO "IST-2IDT,M3.4.4/26,M10.5.0"

// Name of the temporary network the clock creates for first-time WiFi setup.
#define AP_SSID "WiFi-Clock"

// Weather location (decimal degrees). Default: Tel Aviv. Open-Meteo is free and
// needs no API key. Change these to your own coordinates.
#define WEATHER_LAT "32.0853"
#define WEATHER_LON "34.7818"
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
enum Mode { MODE_CLOCK, MODE_DATE, MODE_SECONDS, MODE_UPTIME, MODE_STATS,
            MODE_WEATHER, MODE_WIFI, MODE_BRIGHT, MODE_FLASH, MODE_EMERGENCY,
            MODE_RUNNER, MODE_COUNT };
static Mode mode = MODE_CLOCK;

// OLED brightness (contrast register). Three manual levels, persisted in NVS.
// This panel's contrast curve is lopsided -- it bunches up at the low end AND
// saturates near the top (anything past ~128 looks the same as 255), so the
// only well-separated triple is very-low / ~40 / max.
static const uint8_t BRIGHT_VALUES[3] = {1, 40, 255};  // low / med / high
static uint8_t brightLevel = 2;   // index into BRIGHT_VALUES; default = max
static bool torchOn = false;      // flashlight: whole screen steady white
static bool strobeOn = false;     // emergency light: whole screen flashing white
static uint8_t flashHz = 2;       // strobe rate, flashes/sec (1..10), web-adjustable
static unsigned long jumpStart = 0;  // RUNNER: millis() a jump began (0 = grounded)

// torchOn and strobeOn are full-screen overrides: when either is set the panel
// is taken over (steady or flashing) regardless of which screen `mode` selects.
// They are mutually exclusive.

// Weather (Open-Meteo). Refreshed on a timer in loop(), shown on MODE_WEATHER.
static float weatherTemp = 0;          // current temperature, deg C
static int   weatherCode = -1;         // WMO weather code (-1 = unknown)
static bool  weatherValid = false;     // true once a fetch has succeeded
static unsigned long lastWeather = 0;  // millis() of last fetch (0 = never)

// App phase: either serving the setup portal, or running the clock.
enum AppState { ST_PORTAL, ST_RUN };
static AppState appState = ST_RUN;

Preferences prefs;          // persists WiFi creds in NVS (namespace "wifi")
WebServer server(80);       // setup web page
DNSServer dns;              // captive-portal redirect during setup
String wifiSsid, wifiPass;  // current credentials (loaded from NVS)

// Registers all HTTP routes exactly once. Both the setup portal and the run-mode
// dashboard share the same WebServer + routes; handlers branch on appState.
static void registerRoutes();

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

// ---------- Brightness (NVS namespace "cfg") ----------

static void applyBrightness() {
  u8g2.setContrast(BRIGHT_VALUES[brightLevel]);
}

static void loadBrightness() {
  // Key is "blvl" (renamed from "bright") so any value saved during earlier
  // testing is ignored once -- the device starts at max by default.
  prefs.begin("cfg", true);
  brightLevel = prefs.getUChar("blvl", 2);  // default: max
  prefs.end();
  if (brightLevel > 2) brightLevel = 2;
}

static void saveBrightness() {
  prefs.begin("cfg", false);
  prefs.putUChar("blvl", brightLevel);
  prefs.end();
}

// ---------- Emergency-strobe frequency (NVS namespace "cfg") ----------

static void loadFlashFreq() {
  prefs.begin("cfg", true);
  flashHz = prefs.getUChar("freq", 2);  // default 2 Hz
  prefs.end();
  if (flashHz < 1) flashHz = 1;
  if (flashHz > 10) flashHz = 10;
}

static void saveFlashFreq() {
  prefs.begin("cfg", false);
  prefs.putUChar("freq", flashHz);
  prefs.end();
}

// ---------- Weather (Open-Meteo, HTTPS, no API key) ----------

// Pull the number that follows "key": in a JSON string. Tiny and allocation-free
// -- enough for the flat Open-Meteo "current" object; not a general parser.
static bool jsonNum(const String &json, const char *key, float &out) {
  String pat = String("\"") + key + "\":";
  int i = json.indexOf(pat);
  if (i < 0) return false;
  out = atof(json.c_str() + i + pat.length());
  return true;
}

// WMO weather code -> short label that fits the 72px screen.
static const char *wmoText(int code) {
  if (code == 0) return "Clear";
  if (code <= 3) return "Cloudy";
  if (code <= 48) return "Fog";
  if (code <= 57) return "Drizzle";
  if (code <= 67) return "Rain";
  if (code <= 77) return "Snow";
  if (code <= 82) return "Showers";
  if (code <= 86) return "Snow";
  return "Storm";  // 95+
}

// Fetch current temperature + weather code over HTTPS. setInsecure() skips cert
// validation -- fine for a public read-only endpoint on a hobby clock.
static void fetchWeather() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT
               "&longitude=" WEATHER_LON
               "&current=temperature_2m,weather_code";
  if (!http.begin(client, url)) {
    Serial.println("Weather: http.begin failed");
    return;
  }
  int rc = http.GET();
  if (rc == 200) {
    String body = http.getString();
    // The response carries a "current_units" object (e.g. "temperature_2m":"°C")
    // BEFORE the real "current" data, so anchor the parse to the data object --
    // otherwise indexOf matches the units string and atof("°C") gives 0.
    int ci = body.indexOf("\"current\":");
    String cur = ci >= 0 ? body.substring(ci) : body;
    float temp, code;
    if (jsonNum(cur, "temperature_2m", temp) && jsonNum(cur, "weather_code", code)) {
      weatherTemp = temp;
      weatherCode = (int)code;
      weatherValid = true;
      Serial.printf("Weather: %.1fC code %d\n", weatherTemp, weatherCode);
    } else {
      Serial.println("Weather: parse failed");
    }
  } else {
    Serial.printf("Weather: HTTP %d\n", rc);
  }
  http.end();
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

// Unknown path: in setup, bounce to the captive portal; in run mode, to the
// dashboard root.
static void handleNotFound() {
  server.sendHeader("Location",
                    appState == ST_PORTAL ? "http://192.168.4.1/" : "/");
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

  registerRoutes();
  server.begin();

  showPortalScreen();
  Serial.printf("Setup portal up: join '%s', open http://%s/\n",
                AP_SSID, ip.toString().c_str());
}

// ---------- Run-mode web dashboard ----------
// Unlike the setup portal, this runs while the clock is running, on the home
// network. It's a single self-contained page (no external assets) that polls a
// JSON endpoint and POSTs control changes back, so it works offline on the LAN.

// The dashboard page. Served verbatim; all live data comes from /api, all
// actions go to /set, so the HTML itself is static.
static const char DASH_HTML[] = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>WiFi Clock</title><style>
:root{color-scheme:dark}
*{box-sizing:border-box}
body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:#0b0f14;color:#e6edf3}
.wrap{max-width:460px;margin:0 auto;padding:18px}
h1{font-size:1.05rem;margin:.2em 0 0;letter-spacing:.04em;color:#9aa7b2}
.time{font-size:2.7rem;font-weight:700;font-variant-numeric:tabular-nums;line-height:1.1;margin:.05em 0}
.sub{color:#9aa7b2;font-size:.9rem;margin-bottom:.4em}
.card{background:#151c25;border:1px solid #222c38;border-radius:12px;padding:14px;margin:12px 0}
.card h2{font-size:.72rem;text-transform:uppercase;letter-spacing:.08em;color:#7f93a6;margin:0 0 .7em}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
.modes{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
button{font:inherit;padding:.7em .4em;border-radius:9px;border:1px solid #2b3a4a;background:#1c2733;color:#e6edf3;cursor:pointer}
button:active{transform:scale(.97)}
button.on{background:#2563eb;border-color:#2563eb;color:#fff}
.row{display:flex;justify-content:space-between;padding:.25em 0;font-size:.95rem}
.row span:last-child{color:#9aa7b2}
input[type=range]{width:100%;margin:.5em 0 0;accent-color:#2563eb}
.btn{width:100%}
</style></head><body><div class=wrap>
<h1>WIFI CLOCK</h1>
<div class=time id=time>--:--:--</div>
<div class=sub id=date>&nbsp;</div>
<div class=card><h2>Display</h2><div class=modes id=modes></div></div>
<div class=card><h2>Brightness</h2><div class=modes>
<button onclick="set('bright=0')">Low</button>
<button onclick="set('bright=1')">Med</button>
<button onclick="set('bright=2')">High</button>
</div></div>
<div class=card><h2>Flashlight</h2>
<button id=torch class=btn onclick="set('torch='+(T?0:1))">Light On / Off</button>
</div>
<div class=card><h2>Emergency Light</h2>
<button id=strobe class=btn onclick="set('strobe='+(S?0:1))">Flash On / Off</button>
<div class=row style="margin-top:.7em"><span>Frequency</span><span id=hz>- Hz</span></div>
<input type=range min=1 max=10 step=1 id=freq oninput="hz.textContent=this.value+' Hz'" onchange="set('freq='+this.value)">
</div>
<div class=card><h2>Status</h2>
<div class=row><span>Weather</span><span id=wx>-</span></div>
<div class=row><span>Network</span><span id=net>-</span></div>
<div class=row><span>IP</span><span id=ip>-</span></div>
<div class=row><span>Uptime</span><span id=up>-</span></div>
<div class=row><span>Free heap</span><span id=heap>-</span></div>
</div></div><script>
const N=["Clock","Date","Seconds","Uptime","Stats","Weather","WiFi","Bright","Flash","Emergency","Runner"];
let cur=-1,T=false,S=false;
const mc=document.getElementById('modes');
N.forEach((n,i)=>{let b=document.createElement('button');b.textContent=n;b.dataset.i=i;b.onclick=()=>set('mode='+i);mc.appendChild(b);});
function set(q){fetch('/set?'+q).then(load);}
function up(s){let d=Math.floor(s/86400);s%=86400;let h=Math.floor(s/3600);s%=3600;let m=Math.floor(s/60);return (d?d+'d ':'')+[h,m,s%60].map(x=>String(x).padStart(2,'0')).join(':');}
function load(){fetch('/api').then(r=>r.json()).then(d=>{
 time.textContent=d.time;date.innerHTML=d.date||'&nbsp;';
 wx.textContent=d.wxValid?Math.round(d.wxTemp)+'°C '+d.wxCond:'no data';
 net.textContent=d.ssid+' ('+d.rssi+' dBm)';ip.textContent=d.ip;
 up_.textContent=up(d.uptime);heap.textContent=(d.heap/1024).toFixed(1)+' KB';
 T=!!d.torch;torch.classList.toggle('on',T);
 S=!!d.strobe;strobe.classList.toggle('on',S);
 if(document.activeElement!==freq)freq.value=d.freq;
 hz.textContent=freq.value+' Hz';
 if(d.mode!=cur){cur=d.mode;[...mc.children].forEach(b=>b.classList.toggle('on',+b.dataset.i===cur));}
}).catch(()=>{});}
const up_=document.getElementById('up');
load();setInterval(load,1000);
</script></body></html>)HTML";

static void handleDash() {
  server.send(200, "text/html", DASH_HTML);
}

// "/" serves the setup page during setup, the dashboard while running.
static void handleRootDispatch() {
  if (appState == ST_PORTAL) handleRoot();
  else handleDash();
}

// Live status as JSON for the dashboard to poll.
static void handleApi() {
  struct tm t;
  char timeStr[9] = "--:--:--", dateStr[20] = "";
  if (getLocalTime(&t, 10)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &t);
    strftime(dateStr, sizeof(dateStr), "%a %d/%m/%Y", &t);
  }
  String j = "{";
  j += "\"time\":\"" + String(timeStr) + "\",";
  j += "\"date\":\"" + String(dateStr) + "\",";
  j += "\"uptime\":" + String(millis() / 1000) + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  j += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  j += "\"ssid\":\"" + esc(WiFi.SSID()) + "\",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  j += "\"mode\":" + String((int)mode) + ",";
  j += "\"bright\":" + String(brightLevel) + ",";
  j += "\"torch\":" + String(torchOn ? 1 : 0) + ",";
  j += "\"strobe\":" + String(strobeOn ? 1 : 0) + ",";
  j += "\"freq\":" + String(flashHz) + ",";
  j += "\"wxValid\":" + String(weatherValid ? 1 : 0) + ",";
  j += "\"wxTemp\":" + String(weatherTemp, 1) + ",";
  j += "\"wxCond\":\"" + String(weatherValid ? wmoText(weatherCode) : "") + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

// Apply a control change. Each knob is an optional query arg, so the dashboard
// can set one thing at a time. Mirrors the button-handler semantics exactly.
static void handleSet() {
  if (server.hasArg("mode")) {
    int m = server.arg("mode").toInt();
    if (m >= 0 && m < MODE_COUNT) {
      mode = (Mode)m;
      if (torchOn || strobeOn) {     // changing screens turns the light off
        torchOn = strobeOn = false;
        applyBrightness();
      }
    }
  }
  if (server.hasArg("bright")) {
    int b = server.arg("bright").toInt();
    if (b >= 0 && b <= 2) {
      brightLevel = (uint8_t)b;
      if (!torchOn && !strobeOn) applyBrightness();
      saveBrightness();
    }
  }
  if (server.hasArg("torch")) {
    if (server.arg("torch").toInt()) { torchOn = true; strobeOn = false; u8g2.setContrast(255); }
    else { torchOn = false; applyBrightness(); }
  }
  if (server.hasArg("strobe")) {
    if (server.arg("strobe").toInt()) { strobeOn = true; torchOn = false; u8g2.setContrast(255); }
    else { strobeOn = false; applyBrightness(); }
  }
  if (server.hasArg("freq")) {
    int f = server.arg("freq").toInt();
    if (f < 1) f = 1;
    if (f > 10) f = 10;
    flashHz = (uint8_t)f;
    saveFlashFreq();
  }
  if (server.hasArg("jump") && mode == MODE_RUNNER) jumpStart = millis();
  server.send(200, "text/plain", "ok");
}

// Registers every route once (idempotent). "/" and onNotFound branch on
// appState so the portal and the dashboard can share one WebServer.
static void registerRoutes() {
  static bool done = false;
  if (done) return;
  done = true;
  server.on("/", handleRootDispatch);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/api", handleApi);
  server.on("/set", handleSet);
  server.onNotFound(handleNotFound);
}

static void startDashboard() {
  registerRoutes();
  server.begin();

  // mDNS: reach the clock at http://wifi-clock.local/ without knowing the IP.
  if (MDNS.begin("wifi-clock")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS up: http://wifi-clock.local/");
  }
  Serial.printf("Dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
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

// Debounced BOOT button.
//   Short press            -> next screen (fires on release; needed to tell a
//                             short press apart from the start of a long one).
//   Long press (>=1.5s)    -> the action for the current screen, fired THE
//                             INSTANT the hold crosses the threshold (while
//                             still pressed) so it feels immediate:
//       WiFi   -> open the setup portal (saved creds left untouched)
//       Bright -> step Low/Med/High (persisted)
//       Flash  -> turn the steady flashlight on (whole screen, full power)
//       Emerg  -> turn the emergency strobe on (whole screen flashing)
//       Runner -> make the figure jump
//       others -> ignored
//   While either light (steady or strobe) is on, the next press turns it off.
static void pollButton() {
  static bool stable = HIGH, lastReading = HIGH;
  static unsigned long lastChange = 0, pressStart = 0;
  static bool handled = false;  // long-press / torch-off already acted on

  bool reading = digitalRead(BTN_PIN);
  unsigned long now = millis();
  if (reading != lastReading) {
    lastReading = reading;
    lastChange = now;
  }

  if (now - lastChange > 25 && reading != stable) {  // debounced edge
    stable = reading;
    if (stable == LOW) {           // just pressed
      pressStart = now;
      handled = false;
      if (torchOn || strobeOn) {   // any press exits the light, right away
        torchOn = false;
        strobeOn = false;
        applyBrightness();         // restore the chosen level
        handled = true;            // consume: ignore this press's release
      }
    } else {                       // just released
      if (!handled) mode = (Mode)((mode + 1) % MODE_COUNT);  // short press
    }
  }

  // Fire the long-press action the moment the hold passes the threshold,
  // without waiting for release.
  if (stable == LOW && !handled && now - pressStart >= LONG_PRESS_MS) {
    handled = true;
    switch (mode) {
      case MODE_WIFI:
        startPortal();             // setup only from WiFi screen
        break;
      case MODE_BRIGHT:
        brightLevel = (brightLevel + 1) % 3;
        applyBrightness();
        saveBrightness();
        break;
      case MODE_FLASH:
        torchOn = true;
        strobeOn = false;
        u8g2.setContrast(255);     // full power while the torch is on
        break;
      case MODE_EMERGENCY:
        strobeOn = true;
        torchOn = false;
        u8g2.setContrast(255);     // full power for the strobe too
        break;
      case MODE_RUNNER:
        jumpStart = millis();      // launch a jump (ignored if already airborne)
        break;
      default:
        break;                     // long press ignored elsewhere
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

// MODE_STATS: the board's closest thing to `top`. There's no shell on the
// ESP32-C3 (it runs FreeRTOS, not a general-purpose OS), so we print the live
// numbers ourselves: free heap now, the lowest the heap has ever dropped to
// (a leak shows up here), and the CPU clock.
static void drawStats() {
  char buf[16];
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, "MEMORY");
  // KB with one decimal so small drifts are visible on a ~270 KB heap.
  snprintf(buf, sizeof(buf), "free %4.1fk", ESP.getFreeHeap() / 1024.0);
  u8g2.drawStr(0, 20, buf);
  snprintf(buf, sizeof(buf), "low  %4.1fk", ESP.getMinFreeHeap() / 1024.0);
  u8g2.drawStr(0, 30, buf);
  snprintf(buf, sizeof(buf), "cpu  %dMHz", (int)getCpuFrequencyMhz());
  u8g2.drawStr(0, 40, buf);
}

// MODE_WEATHER: current temperature (big) + condition text. "no data" until the
// first successful fetch.
static void drawWeather() {
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, "WEATHER");
  if (!weatherValid) {
    drawCentered(26, "no data");
    return;
  }
  char buf[12];
  snprintf(buf, sizeof(buf), "%d\xB0""C", (int)lround(weatherTemp));
  u8g2.setFont(u8g2_font_helvB14_tf);  // _tf: includes the degree glyph 0xB0
  drawCentered(28, buf);
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(39, wmoText(weatherCode));
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

// MODE_BRIGHT: current brightness level; long-press cycles Low/Med/High.
static void drawBright() {
  const char *name = brightLevel == 0 ? "LOW" : brightLevel == 1 ? "MED" : "HIGH";
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, "BRIGHTNESS");
  u8g2.setFont(u8g2_font_helvB14_tr);
  drawCentered(28, name);
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(39, "hold=change");
}

// MODE_FLASH: the flashlight's control screen. The lit panel itself is a global
// override drawn in loop(); here we just show how to turn it on.
static void drawFlash() {
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, "FLASHLIGHT");
  drawCentered(22, "hold 1s");
  drawCentered(34, "to turn on");
}

// MODE_EMERGENCY: the strobe's control screen (the flashing is a global override
// in loop()). Shows the current rate; long-press starts it.
static void drawEmergency() {
  char buf[12];
  u8g2.setFont(u8g2_font_5x8_tr);
  drawCentered(8, "EMERGENCY");
  snprintf(buf, sizeof(buf), "%d Hz", flashHz);
  drawCentered(22, buf);
  drawCentered(34, "hold=flash");
}

// One key pose of the run cycle. All limb points are (dx, dy) offsets: legs are
// measured from the hip, arms from the shoulder. `bob` drops the whole figure a
// pixel on the mid-stride frames to give it a little vertical bounce.
struct RunFrame {
  int8_t kneeL[2], footL[2];
  int8_t kneeR[2], footR[2];
  int8_t elbowL[2], handL[2];
  int8_t elbowR[2], handR[2];
  int8_t bob;
};

// MODE_RUNNER: a stick figure jogging in place over a scrolling ground. The
// pose and the ground offset are both derived from millis(), so it animates on
// its own; loop() redraws this screen faster than the others to keep it smooth.
static void drawRunner() {
  const int groundY = 37;
  // Dashes that march left to fake forward motion.
  int shift = (millis() / 40) % 8;
  for (int x = -shift; x < 72; x += 8) u8g2.drawHLine(x, groundY, 4);

  // Four hand-tuned poses: stride-left, gather, stride-right, gather. Arms
  // swing opposite the legs.
  static const RunFrame frames[4] = {
    // kneeL    footL     kneeR     footR     elbowL    handL     elbowR    handR   bob
    {{ 4, 6}, { 8,11}, {-3, 6}, {-7,10}, {-4, 4}, {-7, 6}, { 4, 4}, { 7, 7}, 0},
    {{ 2, 5}, { 3,11}, {-1, 7}, { 0,12}, {-2, 4}, {-3, 7}, { 2, 4}, { 3, 7}, 1},
    {{-3, 6}, {-7,10}, { 4, 6}, { 8,11}, { 4, 4}, { 7, 7}, {-4, 4}, {-7, 6}, 0},
    {{-1, 7}, { 0,12}, { 2, 5}, { 3,11}, { 2, 4}, { 3, 7}, {-2, 4}, {-3, 7}, 1},
  };
  // Jump endpoints. As the figure rises it morphs from a feet-down stance into a
  // legs-tucked, arms-overhead apex pose; on the way down it morphs back. Both
  // are blended by height so the legs reach the ground exactly as lift hits 0 --
  // no snap when control returns to the run cycle.
  static const RunFrame standPose =
    {{-1, 6}, {-2,12}, { 1, 6}, { 2,12}, {-2, 4}, {-3, 7}, { 2, 4}, { 3, 7}, 0};
  static const RunFrame apexPose =
    {{ 4, 5}, { 6, 3}, {-4, 5}, {-6, 3}, {-3, 2}, {-6,-1}, { 3, 2}, { 6,-1}, 0};

  // A jump is a single parabolic hop: lift peaks halfway through JUMP_MS, then
  // the figure settles back onto the ground and resumes running.
  const unsigned long JUMP_MS = 700;
  const int JUMP_H = 16;          // peak lift in pixels
  int lift = 0;
  RunFrame jumpFrame;             // stand<->apex blend, only used while jumping
  if (jumpStart) {
    unsigned long dt = millis() - jumpStart;
    if (dt < JUMP_MS) {
      float p = dt / (float)JUMP_MS;        // 0..1 through the hop
      lift = (int)(JUMP_H * 4 * p * (1 - p) + 0.5f);  // parabola, 0 at the ends
      // Blend every limb point from stance to apex by how high we are.
      float t = lift / (float)JUMP_H;
      const int8_t *a = (const int8_t *)&standPose;
      const int8_t *b = (const int8_t *)&apexPose;
      int8_t *c = (int8_t *)&jumpFrame;
      for (size_t i = 0; i < sizeof(RunFrame); i++)
        c[i] = (int8_t)lroundf(a[i] + t * (b[i] - a[i]));
    } else {
      jumpStart = 0;                        // landed
    }
  }

  const RunFrame &f = jumpStart ? jumpFrame : frames[(millis() / 120) % 4];

  const int cx = 36;                      // hip x
  const int hipY = 24 + f.bob - lift;     // hip y (feet reach the ground from here)
  const int shX = cx + 1;         // shoulder, leaning slightly forward
  const int shY = hipY - 9;

  // Torso + filled head.
  u8g2.drawLine(cx, hipY, shX, shY);
  u8g2.drawDisc(shX + 1, shY - 3, 3, U8G2_DRAW_ALL);

  // Legs: hip -> knee -> foot.
  u8g2.drawLine(cx, hipY, cx + f.kneeL[0], hipY + f.kneeL[1]);
  u8g2.drawLine(cx + f.kneeL[0], hipY + f.kneeL[1], cx + f.footL[0], hipY + f.footL[1]);
  u8g2.drawLine(cx, hipY, cx + f.kneeR[0], hipY + f.kneeR[1]);
  u8g2.drawLine(cx + f.kneeR[0], hipY + f.kneeR[1], cx + f.footR[0], hipY + f.footR[1]);

  // Arms: shoulder -> elbow -> hand.
  u8g2.drawLine(shX, shY, shX + f.elbowL[0], shY + f.elbowL[1]);
  u8g2.drawLine(shX + f.elbowL[0], shY + f.elbowL[1], shX + f.handL[0], shY + f.handL[1]);
  u8g2.drawLine(shX, shY, shX + f.elbowR[0], shY + f.elbowR[1]);
  u8g2.drawLine(shX + f.elbowR[0], shY + f.elbowR[1], shX + f.handR[0], shY + f.handR[1]);
}

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC enumerate before logging
  pinMode(BTN_PIN, INPUT_PULLUP);  // BOOT button
  // Display is initialized exactly ONCE, here, before WiFi. Calling
  // u8g2.begin() again after the radio is up wedges the C3 (see NOTES.md).
  u8g2.begin();
  u8g2.setBusClock(100000);  // 100kHz I2C: more tolerant of supply noise
  loadBrightness();
  loadFlashFreq();
  applyBrightness();         // honor the saved brightness from the first frame

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
    startDashboard();  // bring up the live web dashboard on the home network
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

  // Service the dashboard every loop so it stays responsive even while the
  // clock is still syncing time below.
  server.handleClient();

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

  unsigned long now = millis();

  // ---- Weather refresh (non-blocking cadence) ----
  // Retry every 60s until the first success, then every 15 min. The blocking
  // HTTPS GET runs at most once per interval, so the loop stays responsive.
  if (WiFi.status() == WL_CONNECTED) {
    unsigned long due = weatherValid ? 900000UL : 60000UL;
    if (lastWeather == 0 || now - lastWeather >= due) {
      lastWeather = now;
      fetchWeather();
    }
  }

  pollButton();  // short press cycles modes; long press opens setup
  if (appState == ST_PORTAL) return;  // long-press just switched us to setup

  if (!getLocalTime(&t, 10)) {
    showMessage("Time", "lost");
    delay(1000);
    return;
  }

  // Redraw at ~4Hz (smooth colon blink / ticking seconds) -- except the runner
  // (animates) and the strobe (needs fast on/off edges). The loop itself stays
  // fast so the button remains responsive.
  static unsigned long lastDraw = 0;
  unsigned long drawEvery = strobeOn ? 15 : (mode == MODE_RUNNER ? 60 : 250);
  if (now - lastDraw >= drawEvery) {
    lastDraw = now;
    u8g2.clearBuffer();
    if (torchOn) {
      u8g2.drawBox(0, 0, 72, 40);              // steady flashlight
    } else if (strobeOn) {
      unsigned long half = 500UL / flashHz;    // one on/off phase, ms
      if (half < 1) half = 1;
      if ((now / half) % 2 == 0) u8g2.drawBox(0, 0, 72, 40);  // flashing
    } else {
      switch (mode) {
        case MODE_CLOCK:     drawClock(t);   break;
        case MODE_DATE:      drawBigDate(t); break;
        case MODE_SECONDS:   drawSeconds(t); break;
        case MODE_UPTIME:    drawUptime();   break;
        case MODE_STATS:     drawStats();    break;
        case MODE_WEATHER:   drawWeather();  break;
        case MODE_WIFI:      drawWifi();     break;
        case MODE_BRIGHT:    drawBright();   break;
        case MODE_FLASH:     drawFlash();    break;
        case MODE_EMERGENCY: drawEmergency(); break;
        case MODE_RUNNER:    drawRunner();   break;
        default:             drawClock(t);   break;
      }
    }
    u8g2.sendBuffer();
  }

  delay(15);  // button sampling cadence (also the debounce resolution)
}
