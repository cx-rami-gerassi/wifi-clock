# wifi-clock

A WiFi-synced NTP clock for the **ESP32-C3** with a 72×40 SSD1306 OLED. It connects
to WiFi, syncs time over NTP (with automatic DST via a POSIX timezone string), and
displays `HH:MM` with a blinking colon, plus the date and seconds.

![clock display: HH:MM, date bottom-left, seconds bottom-right]

## Hardware

- ESP32-C3 (e.g. `esp32-c3-devkitm-1`)
- SSD1306 72×40 I²C OLED
  - `SDA` → GPIO 5
  - `SCL` → GPIO 6

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy the credentials template and fill in your network:
   ```sh
   cp include/secrets.h.example include/secrets.h
   # then edit include/secrets.h with your WiFi SSID + password
   ```
   `include/secrets.h` is gitignored, so your password never gets committed.
3. (Optional) Set your timezone in [`src/main.cpp`](src/main.cpp) via `TZ_INFO`
   (POSIX format — find yours in the [posix_tz_db](https://github.com/nayarsystems/posix_tz_db)).
   Default is Israel (`IST-2IDT,...`).

## Build, upload, monitor

```sh
pio run                       # build
pio run -t upload             # flash (add --upload-port /dev/cu.usbmodemXXX if needed)
pio device monitor            # serial logs @ 115200
```

## Design notes

- **Sync never freezes.** NTP runs non-blocking in `loop()`, showing a live counter and
  re-requesting every 8 s, so a dropped first packet self-heals instead of hanging on the
  "Syncing time..." screen.
- **The OLED is initialized exactly once,** in `setup()`. Do **not** call `u8g2.begin()` a
  second time after WiFi is up — it wedges the ESP32-C3 and freezes the display.
- **TX power is intentionally low** (`WIFI_POWER_8_5dBm`) to avoid the current spike that
  can brown out the OLED, at the cost of a slightly weaker link.

## Roadmap

- WiFi config portal (enter credentials over a web page; no recompile)
- Auto-dimming / night brightness
- Weather display
