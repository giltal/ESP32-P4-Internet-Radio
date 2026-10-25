# ESP32-P4 Internet Radio

A touch-screen internet radio for the **Guition JC1060P470C** (ESP32-P4 + ESP32-C6, 7" 1024×600 IPS, GT911 touch, ES8311 codec + NS4150 speaker amp), built with **ESP-IDF 5.5** and **LVGL 9**.

Stations come from [radio-browser.info](https://www.radio-browser.info), filtered to the ones the P4 can actually play, so the radio keeps working when stations move their stream URLs. The default country is Israel (`IL`), but you can change it in menuconfig.

## Features

- Station directory from radio-browser.info for one country, sorted by popularity. It keeps only plain **MP3 / AAC / HE-AAC** streams (no HLS), removes duplicates, caches the list on the SD card and refreshes it daily.
- **Favorites** tab, last station resumed at boot, prev / next buttons.
- Streaming over HTTP and HTTPS (certificate bundle). It follows **redirects** (needed for StreamTheWorld / Kan), resolves `.pls` / `.m3u` playlists, and accepts Shoutcast `ICY 200` responses.
- **ICY "now playing"** titles. Legacy Windows-1255 (Hebrew) and Windows-1251 (Russian) titles are converted to UTF-8.
- 384 KB PSRAM ring buffer with pre-buffering, underrun recovery and automatic reconnect with back-off.
- Hebrew right-to-left UI in the Alef font, with a large clock, the Hebrew weekday and the full date. Arabic and Russian station names render through Noto Sans fallback fonts (LVGL BiDi + Arabic shaping).
- Song title and artist split from the stream's ICY "Artist - Title" metadata.
- Weather from Open-Meteo (free, no key): current conditions in the header and a 5-day forecast tab; the city is searched by name in settings.
- Clock kept by the on-board RX8025T RTC (stored as UTC; Israel time zone with automatic summer time), synced from the network or set by hand.
- Settings screen (Hebrew): Wi-Fi scan with a Hebrew / English / digits keyboard, date & time setup with automatic network sync, and brightness.
- Level meter, buffer meter, and codec / bitrate / sample-rate readout.

## Hardware notes (JC1060P470C)

| Function | Pins |
|---|---|
| LCD | MIPI-DSI 2-lane, JD9165, reset GPIO27 (V1) / GPIO0 (V2) |
| Backlight | GPIO23 (PWM) |
| I2C (touch + codec) | SDA 7, SCL 8 — GT911 @ 0x5D/0x14, ES8311 @ 0x18 |
| I2S | MCLK 13, BCLK 12, LRCK 10, DOUT 9, DIN 11 |
| Speaker amp enable | GPIO20 |
| ESP32-C6 (Wi-Fi) | SDIO: CLK 18, CMD 19, D0–D3 14–17, reset 54 |

Things to check on your unit:

1. **Chip revision.** The build defaults to ESP32-P4 **rev v1.x**, which is what most JC1060P470C boards have. If the boot log shows `chip revision: v3.x`, build with the rev3 overlay (see below). An image built for the wrong revision won't boot.
2. **Panel variant.** Guition changed the 7" panel in 2026. If the rear label has **"V2"** after the material number, select *LCD panel variant → V2 panel* in menuconfig. A black or noisy screen usually means the wrong variant is selected.
3. **Backlight polarity.** If raising the brightness makes the screen darker, enable *Backlight PWM is active-low*.
4. **Wi-Fi.** This project uses `esp_hosted` **2.12.x**, which works with the C6 firmware Guition ships (the same pairing ESPHome uses). If you've reflashed the C6 with a different esp-hosted slave version, match the host version in `main/idf_component.yml`.

## Build and flash

```bash
# ESP-IDF v5.5.x environment
. $IDF_PATH/export.sh

idf.py set-target esp32p4
idf.py menuconfig        # optional: "Internet Radio (JC1060P470C)" menu
idf.py build flash monitor
```

Dependencies download automatically through the IDF Component Manager on the first build.

**ESP32-P4 rev v3.x boards:**

```bash
rm -f sdkconfig
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.p4rev3" set-target esp32p4 build flash monitor
```

## First run

1. On the radio, open ⚙ **Settings**, pick your network, enter the password and tap **Connect**. You can also set *Default Wi-Fi SSID / password* in menuconfig before building.
2. Once it's online, the station list downloads by itself. The **Refresh** button fetches it again.
3. Tap a station to play it. Tap ★ to add it to Favorites.

## menuconfig options

`Internet Radio (JC1060P470C)`:

- **Wi-Fi**: default SSID and password (used only until you pick a network on screen).
- **Display panel**: V1/V2 panel, reset GPIO, backlight GPIO and polarity, touch swap/mirror.
- **Audio**: amplifier enable GPIO, stream buffer size, pre-buffer size.
- **Station directory**: country code (`IL`, `US`, `DE`…) and the maximum number of stations.

## Code layout

| File | Purpose |
|---|---|
| `main/board.c` | JD9165 panel (both init tables), GT911 touch, LVGL port, backlight, I2S + ES8311 |
| `main/wifi_mgr.c` | Wi-Fi via esp_hosted / esp_wifi_remote, credentials in RAM, scan, SNTP (Israel time zone) |
| `main/station_db.c` | radio-browser.info download, filtering, SD-card cache, favorites |
| `main/sdcard.c` | microSD (kept mounted next to the C6 link): `WiFi.txt` and all saved settings |
| `main/player.c` | HTTP reader (redirects, playlists, ICY metadata) → ring buffer → MP3/AAC decoder → codec |
| `main/byte_ring.c` | Resettable PSRAM byte ring buffer |
| `main/ui.c` | LVGL UI (Hebrew, RTL): clock/date header, station lists, now-playing card, settings overlay |
| `main/ui_common.c` | Fonts with fallback chain, RTL helpers, Hebrew/English keyboard, toast |
| `main/weather.c` | Open-Meteo forecast + geocoding city search, location on the SD card |
| `main/clock.c` | RX8025T RTC + SNTP, Israel time zone, manual clock setting |
| `main/fonts/` | Alef Hebrew fonts (22/32/48 px, 120 px clock digits) and Noto Sans fallbacks (Latin, Hebrew, Cyrillic, Arabic) + LVGL symbols |

### Regenerating the fonts

The Alef fonts: `sh tools/gen_fonts_heb.sh` (Node.js required). The Noto fallback fonts:

```bash
npm i lv_font_conv
npx lv_font_conv --bpp 4 --size 20 --no-compress --format lvgl --force-fast-kern-format \
  --font NotoSans.ttf       -r 0x20-0x7E,0xA0-0xFF,0x400-0x45F,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x20AA,0x20AC \
  --font NotoSansHebrew.ttf -r 0x591-0x5F4 \
  --font NotoSansArabic.ttf -r 0x600-0x6FF,0xFE70-0xFEFF \
  --font FontAwesome5-Solid+Brands+Regular.woff -r <LVGL symbol list>,0xF005 \
  -o main/fonts/font_ui_20.c
```

(Repeat for sizes 28 and 40. The FontAwesome file and symbol list are in `lvgl/scripts/built_in_font/`.)

## Credits

- Panel init sequences and timings: Guition reference drivers, via ESPHome's `mipi_dsi` models.
- Weather data: Open-Meteo (open-meteo.com, CC BY 4.0). Weather icons: Font Awesome 5 Free (icons CC BY 4.0, font SIL OFL 1.1).
- Fonts: Alef (SIL Open Font License, see `fonts_src/OFL.txt`), Noto Sans / Noto Sans Hebrew / Noto Sans Arabic (SIL Open Font License), FontAwesome 5 Free (SIL OFL / CC BY 4.0).
- Hebrew UI layout, keyboard and RTC driver adapted from the ESP32_P4_FingerprintLoginSystem project.
- Station data: radio-browser.info community database.

## License

MIT. See [LICENSE](LICENSE).
