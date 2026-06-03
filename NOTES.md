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
2. **Auto-dimming / night brightness** — adjust OLED contrast by time of day (or a
   photoresistor) via `u8g2.setContrast()`.
3. **Weather** — fetch current temp/conditions (e.g. Open-Meteo, no API key) over HTTPS and
   show on a second screen or alternating with the clock.
