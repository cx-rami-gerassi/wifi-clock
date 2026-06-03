# wifi-clock

A WiFi-synced NTP clock for the **ESP32-C3** with a 72×40 SSD1306 OLED. It connects
to WiFi, syncs time over NTP (with automatic DST via a POSIX timezone string), and
displays `HH:MM` with a blinking colon, plus the date and seconds.

WiFi credentials are entered once over a **browser-based setup page** (no recompiling,
no hardcoded passwords), and the on-board **BOOT button** cycles through several
display screens.

![clock display: HH:MM, date bottom-left, seconds bottom-right]

## Hardware

- ESP32-C3 (e.g. `esp32-c3-devkitm-1`)
- SSD1306 72×40 I²C OLED
  - `SDA` → GPIO 5
  - `SCL` → GPIO 6
- On-board **BOOT button** (GPIO 9) — cycles display screens / opens WiFi setup

## Build, upload, monitor

1. Install [PlatformIO](https://platformio.org/).
2. (Optional) Set your timezone in [`src/main.cpp`](src/main.cpp) via `TZ_INFO`
   (POSIX format — find yours in the [posix_tz_db](https://github.com/nayarsystems/posix_tz_db)).
   Default is Israel (`IST-2IDT,...`).

```sh
pio run                       # build
pio run -t upload             # flash (add --upload-port /dev/cu.usbmodemXXX if needed)
pio device monitor            # serial logs @ 115200
```

No WiFi credentials are compiled in — you set them on the device itself (below).

## First-time WiFi setup

On first boot — or any time it can't connect — the clock starts its own network and
serves a setup page:

1. The OLED shows **SETUP MODE** with the network name and address.
2. From a phone/laptop, join the WiFi network **`WiFi-Clock`**.
3. A setup page opens automatically (or browse to **`192.168.4.1`**). Pick your
   network from the scanned list — or type a name for a hidden network — enter the
   password, and **Save & connect**.
4. The clock stores the credentials, restarts, and connects. It reconnects
   automatically on every future boot.

**2.4 GHz networks only** — the ESP32-C3 has no 5 GHz radio.

To change networks later: short-press BOOT to the **WiFi** screen, then long-press
(~1.5 s) to re-open setup. Your saved network stays put unless you save a new one.

## Display screens

Short-press the BOOT button to cycle through:

| Screen | Shows |
| --- | --- |
| **Clock** | Big `HH:MM` (blinking colon) + date (left) / seconds (right) |
| **Big Date** | Weekday · big `DD/MM` · year |
| **Seconds** | Large ticking seconds counter |
| **Uptime** | Time since boot (`HH:MM:SS`, or `Nd HH:MM` past a day) |
| **WiFi** | Connection status, SSID, IP, and signal (long-press here = setup) |

## Design notes

> For deeper context, hard-won gotchas, and the full roadmap, see [NOTES.md](NOTES.md).


- **Sync never freezes.** NTP runs non-blocking in `loop()`, showing a live counter and
  re-requesting every 8 s, so a dropped first packet self-heals instead of hanging on the
  "Syncing time..." screen.
- **The OLED is initialized exactly once,** in `setup()`. Do **not** call `u8g2.begin()` a
  second time after WiFi is up — it wedges the ESP32-C3 and freezes the display.
- **TX power is intentionally low** (`WIFI_POWER_8_5dBm`) to avoid the current spike that
  can brown out the OLED, at the cost of a slightly weaker link.

## Roadmap

- ~~WiFi config portal~~ — done (browser-based setup, see above)
- Auto-dimming / night brightness
- Weather display
