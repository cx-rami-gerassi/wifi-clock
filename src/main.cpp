#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <time.h>

#include "secrets.h"  // defines WIFI_SSID / WIFI_PASSWORD (gitignored; see secrets.h.example)

// ---------- CONFIG ----------
// POSIX timezone string. Default: Israel (handles DST automatically).
// Find others at: https://github.com/nayarsystems/posix_tz_db
#define TZ_INFO "IST-2IDT,M3.4.4/26,M10.5.0"
// --------------------------------------------------

#define SDA_PIN 5
#define SCL_PIN 6

// On-board "BOOT" button (the left one). It's wired to GPIO9 with a pull-up, so
// it reads HIGH normally and LOW while pressed. (The right button is RST -- a
// hardware reset that can't be read from firmware.)
#define BTN_PIN 9

U8G2_SSD1306_72X40_ER_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, SCL_PIN, SDA_PIN);

// Display screens, cycled by short-pressing the BOOT button.
enum Mode { MODE_CLOCK, MODE_DATE, MODE_SECONDS, MODE_UPTIME, MODE_COUNT };
static Mode mode = MODE_CLOCK;

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

// Debounced edge-detect on the BOOT button. Call frequently; advances to the
// next screen on each press (release-to-press transition, i.e. the falling
// edge). 25ms debounce filters contact bounce without feeling laggy.
static void pollButton() {
  static bool stable = HIGH;        // last debounced level
  static bool lastReading = HIGH;   // last raw sample
  static unsigned long lastChange = 0;

  bool reading = digitalRead(BTN_PIN);
  unsigned long now = millis();
  if (reading != lastReading) {
    lastReading = reading;
    lastChange = now;
  }
  if (now - lastChange > 25 && reading != stable) {
    stable = reading;
    if (stable == LOW) {  // pressed
      mode = (Mode)((mode + 1) % MODE_COUNT);
    }
  }
}

// ---- Screen renderers. Each draws into the buffer; the caller flushes. ----

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

void setup() {
  Serial.begin(115200);
  delay(1500);  // let USB CDC enumerate before logging
  pinMode(BTN_PIN, INPUT_PULLUP);  // BOOT button cycles display modes
  u8g2.begin();
  u8g2.setBusClock(100000);  // 100kHz I2C: more tolerant of supply noise

  showMessage("WiFi", "connecting");
  Serial.printf("Connecting to %s", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  // Lower TX power softens the current spike that can brown-out the OLED.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
  }
  Serial.printf("\nConnected, IP: %s  RSSI: %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());

  // Kick off NTP. configTzTime applies the POSIX TZ rules (incl. DST). We do
  // NOT block here: the wait + retry lives in loop() so a dropped first SNTP
  // packet self-heals instead of freezing the device on "Syncing time...".
  // NOTE: the OLED re-init is deliberately deferred to loop() (after sync), not
  // done here -- calling u8g2.begin() immediately after WiFi bring-up wedges on
  // the still-unsettled I2C bus and hangs forever.
  showMessage("Syncing", "time...");
  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
}

void loop() {
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

  pollButton();  // cycle display modes on a short press

  if (!getLocalTime(&t, 10)) {
    showMessage("Time", "lost");
    delay(1000);
    return;
  }

  // Redraw at ~4Hz (smooth colon blink / ticking seconds) but keep the loop
  // itself fast so the button stays responsive. The mode is advanced inside
  // pollButton(); we just render whichever screen is current.
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
      default:           drawClock(t);   break;
    }
    u8g2.sendBuffer();
  }

  delay(15);  // button sampling cadence (also the debounce resolution)
}
