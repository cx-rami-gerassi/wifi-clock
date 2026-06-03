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
- On macOS the board enumerates as native USB-CDC (e.g. `/dev/cu.usbmodem101`). Opening the
  serial port does **not** reset the chip; pulse DTR/RTS to reset.

## Milestones

Named stages you can jump back to (git tags). To return: `git checkout <tag>`, then
`git switch -` to come back. List them with `git tag -n`.

| Tag                  | Date       | Functionality                                                                 |
| -------------------- | ---------- | ----------------------------------------------------------------------------- |
| `v0.1-working-clock` | 2026-06-03 | Baseline working clock: WiFi connect, non-blocking self-healing NTP sync, HH:MM + blinking colon, date & seconds on OLED. Pre-roadmap. |

## Roadmap

Roughly in suggested order:

1. **WiFi config portal** — stop hardcoding credentials. On first boot (or failed connect),
   start a SoftAP + captive web page to enter SSID/password; persist in NVS/Preferences.
   Biggest usability win and removes the need for `secrets.h`.
2. **Auto-dimming / night brightness** — adjust OLED contrast by time of day (or a
   photoresistor) via `u8g2.setContrast()`.
3. **Weather** — fetch current temp/conditions (e.g. Open-Meteo, no API key) over HTTPS and
   show on a second screen or alternating with the clock.
