# Developer notes

Context, gotchas, and roadmap for the wifi-clock — the stuff that isn't obvious from
reading the code. Keep this updated as the project evolves.

## Hard-won gotchas

### Never call `u8g2.begin()` a second time after WiFi is up
Calling `u8g2.begin()` again once the radio is running **wedges the ESP32-C3** — it
hangs (or resets, dropping the USB-CDC link), freezing whatever was last on the screen.
The classic symptom is being **permanently stuck on the "Syncing time..." screen** even
though the serial log prints `Time synced.`.

The display is initialized exactly **once**, in `setup()`, before WiFi. That's enough —
the per-loop `clearBuffer()` / `sendBuffer()` keeps it updated. If display corruption ever
genuinely needs recovery, resend only the init sequence, not a full `begin()`.

### NTP sync must be non-blocking
An early version blocked in `setup()` on `while (!getLocalTime(&t, 5000)) {}`. If a single
NTP/UDP packet was dropped, it spun **forever** on the syncing screen with no recovery.
Now the sync lives in `loop()`: it shows a live counter and **re-requests NTP every 8 s**
(re-calling `configTzTime`, which also re-resolves DNS), so a lost first packet self-heals.

### TX power is intentionally low
`WiFi.setTxPower(WIFI_POWER_8_5dBm)` softens the current spike that can brown out the OLED.
The trade-off is a weaker link (~-67 dBm on the test network), which makes dropped packets
more likely — hence the self-healing sync above. Don't "fix" the low TX power without
re-checking the brownout behavior.

### The OLED's brightness (contrast) curve is lopsided
`u8g2.setContrast()` (SSD1306 cmd 0x81) is the only brightness knob, but on this panel it's
badly non-linear: low values bunch together (`12` and `90` look identical) AND it saturates
near the top (`128` looks the same as `255`). The only visibly-distinct triple we found is
`{1, 40, 255}` (low/med/high). If you change these, eyeball them on real hardware — evenly
spaced numbers do NOT give evenly spaced brightness.

### One `WebServer` serves both the setup portal and the run dashboard
The same `WebServer server(80)` is reused for the captive setup portal **and** the run-mode
dashboard. ESP32's `WebServer` keeps the **first** handler registered for a path, so
registering `/` twice would let one page shadow the other. Routes are therefore registered
**once** in `registerRoutes()` (idempotent), and `/` + `onNotFound` branch on `appState`
(`handleRootDispatch`). The dashboard comes up in `setup()` after a successful connect
(`startDashboard()` → mDNS `wifi-clock.local`), and `server.handleClient()` runs every loop
in `ST_RUN`. Endpoints: `GET /` (page), `GET /api` (live JSON status), `GET /set?...` (one
control knob per query arg, mirroring the button semantics). The page is a single
self-contained HTML/CSS/JS string (`DASH_HTML`) — no external assets, so it works on a LAN
with no internet. The JS keeps its own `N[]` list of screen names; **it must stay in enum
order** (`Mode`), since `/set?mode=` and `/api`'s `mode` field are bare indices.

### Flashlight & emergency strobe are full-screen *overrides*, drawn in `loop()`
`torchOn` (steady white) and `strobeOn` (flashing) are global flags, **not** tied to their
`MODE_FLASH` / `MODE_EMERGENCY` screens. When either is set, `loop()` fills (or strobes) the
whole panel regardless of which `mode` is selected — the mode renderer is skipped. They are
mutually exclusive. The strobe drops the redraw interval to ~15 ms (vs the usual 250 ms) so
its on/off edges stay crisp; the rate `flashHz` (1–10 Hz) is web-adjustable and persisted in
NVS (`cfg/freq`). The `MODE_FLASH` / `MODE_EMERGENCY` screens themselves only show the
"how to turn it on" hint. Any button press — or switching screens from the web — clears both.
NB: this is a small OLED, not a power LED, so the "emergency light" is only as bright as the
panel at full contrast — a visual indicator, not a room strobe.

### The weather fetch blocks the dashboard briefly
`fetchWeather()` is a synchronous HTTPS GET. While it runs (~1 s, at most once per refresh
interval) the loop can't service `server.handleClient()`, so the dashboard is momentarily
unresponsive. Acceptable for a hobby clock; if it ever matters, move the fetch off the loop.

## Hardware

- ESP32-C3 (`esp32-c3-devkitm-1`)
- SSD1306 72×40 I²C OLED: `SDA` → GPIO 5, `SCL` → GPIO 6
- **Buttons:** `BOOT` = GPIO 9 — readable in firmware (active-low, `INPUT_PULLUP`); a short
  press cycles display modes. `RST` = hardware reset, *not* readable. Which one is physically
  left vs right depends on board orientation, so **go by the silkscreen label**, not the side.
- **2.4 GHz only:** the C3 radio cannot see 5 GHz networks. A 5 GHz-only SSID just hangs
  forever on "WiFi connecting" (the connect loop blocks with no timeout). Use the 2.4 GHz band.
- On macOS the board enumerates as native USB-CDC (e.g. `/dev/cu.usbmodem101`). Opening the
  serial port does **not** reset the chip; pulse DTR/RTS to reset. Note: passively reading the
  port often misses the boot/connect log because the ROM and app hand off the USB endpoint —
  the reliable way to capture boot output is to attach the monitor right after a flash.

## Milestones

Named stages you can jump back to (git tags). To return: `git checkout <tag>`, then
`git switch -` to come back. List them with `git tag -n`.

| Tag                  | Date       | Functionality                                                                 |
| -------------------- | ---------- | ----------------------------------------------------------------------------- |
| `v0.1-working-clock` | 2026-06-03 | Baseline working clock: WiFi connect, non-blocking self-healing NTP sync, HH:MM + blinking colon, date & seconds on OLED. Pre-roadmap. |
| `v0.2-display-modes` | 2026-06-03 | Bigger clock font (logisoso20). BOOT button (GPIO9) cycles 4 screens: Clock → Big Date → Seconds → Uptime. Non-blocking loop for responsive button. |
| `v0.3-wifi-setup`    | 2026-06-03 | Browser-based WiFi setup (SoftAP `WiFi-Clock` + captive page at 192.168.4.1, scanned list + manual SSID), creds in NVS, auto-reconnect, portal only on failure. Added WiFi status screen; long-press on it (only) re-opens setup non-destructively. Replaces `secrets.h`. |
| `v0.4-bright-flash`  | 2026-06-03 | Manual brightness screen (Low/Med/High via `setContrast`, persisted in NVS, default max) + flashlight screen (full-white at max power). Long-press actions now fire on the hold-threshold, not on release, for immediate feedback. |
| `v0.5-weather`       | 2026-06-03 | Weather screen: current temp + condition from Open-Meteo (HTTPS, no API key) for `WEATHER_LAT`/`WEATHER_LON` (default Tel Aviv). Non-blocking refresh (60s until first success, then every 15 min); WMO code → short label. |
| `v0.6-web-dashboard` | 2026-06-05 | Stats screen (free/min heap + CPU MHz). Animated running-man screen (`MODE_RUNNER`); long-press = parabolic jump (pose blended by height so the landing doesn't snap). Run-mode **web dashboard** at `http://wifi-clock.local/` (mDNS): live `/api` JSON + `/set` controls, sharing one `WebServer` with the portal via `appState` dispatch. Flashlight reworked into a global on/off override (works over any screen). **Emergency strobe** screen + web frequency slider (1–10 Hz, persisted in NVS). |

## WiFi provisioning (how setup works)

`secrets.h` is gone — credentials live in NVS (Preferences namespace `wifi`) and are
entered over a web page, not compiled in.

- **Boot:** load creds → if present, try to connect for 20 s → on success run the clock,
  on failure (or no creds) fall into the setup portal. So the last-known-good network is
  always tried first; the portal only appears when it can't connect.
- **Portal:** `WIFI_AP_STA` mode so `WiFi.scanNetworks()` works while the AP is up; a
  `WebServer` serves the form and `DNSServer` does the captive redirect. Saving creds calls
  `ESP.restart()` — the cleanest AP→STA transition (no teardown juggling).
- **Re-entering setup:** long-press BOOT **only from the WiFi screen** (deliberate, avoids
  accidental triggers). It calls `startPortal()` live without erasing NVS, so a power-cycle
  reconnects to the saved network if you don't submit anything new.
- Don't try to force the portal by holding BOOT during reset — GPIO9 held at reset enters
  firmware-download mode, not the app.

## Roadmap

Roughly in suggested order:

1. ~~**WiFi config portal**~~ — **done** (`v0.3-wifi-setup`). Browser setup over a SoftAP,
   creds persisted in NVS; `secrets.h` removed. See "WiFi provisioning" above.
2. **Auto-dimming / night brightness** — manual brightness + flashlight done in
   `v0.4-bright-flash`. Still open: switch brightness *automatically* by time of day (use the
   already-synced clock hour) or a photoresistor on an ADC pin.
3. ~~**Weather**~~ — **done** (`v0.5-weather`). Open-Meteo over HTTPS (`WiFiClientSecure` +
   `setInsecure()`), no API key. Gotcha: the response has a `current_units` object *before*
   the real `current` data, so the JSON parse anchors to `indexOf("\"current\":")` first —
   otherwise it reads `"temperature_2m":"°C"` from units and `atof` returns 0. Used the
   `helvB14_tf` (not `_tr`) font for the `\xB0` degree glyph. Set coords via the two `#define`s.
4. ~~**Web dashboard / remote control**~~ — **done** (`v0.6-web-dashboard`). Live status +
   controls over the home network at `http://wifi-clock.local/`. See the gotchas above.
5. ~~**Stats / runner / emergency strobe**~~ — **done** (`v0.6-web-dashboard`). Stats screen,
   animated running-man (long-press jumps), and a full-screen emergency strobe.

Still open / ideas for next:

- **Auto-dimming / night brightness** (item 2 above) — still the main unfinished roadmap item.
  Dim by clock hour, or add a photoresistor on an ADC pin.
- **Async weather fetch** so the dashboard never stalls (see "weather fetch blocks" gotcha).
- **Device-button frequency control** for the strobe (currently web-only; could cycle
  1→2→5→10 Hz on the Emergency screen, but the long-press is already taken by start/stop).
- **BLE** indoor temp/humidity sensor → an indoor-conditions screen (the C3 has BLE, unused).
- A **reboot / re-run-WiFi-setup** button on the dashboard.
