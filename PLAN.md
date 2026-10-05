# AIS Round Display — Implementation Plan

## Status (2026-10-05)

The original milestones (section 6) are built, and the display runs day to day. Sections 1–8 below are the original plan; where the build turned out differently, the list here says so.

**Built:**
- **Milestones 1–10.** Board bring-up, LVGL, Wi-Fi through the C6, settings and web page, the AIS stack, map builder, map screen, detail screen, time and tide, and the Network screen.
- **Milestone 11 (hardening):** not done as a formal 24 h soak test.
- **Feature requests in `bugs.md`:** implemented (see the notes there).
- **Wi-Fi chip (ESP32-C6) firmware updates from the web page.** The image is built from `coprocessor/` and goes through ESP-Hosted's OTA calls; a failed update leaves the old firmware running. This settles the "C6 firmware version" risk in section 7.
- **RTL-SDR receiver** (see `RTL_SDR_PLAN.md`). A dongle on the USB-A port decodes AIS by itself, alongside the network feed. The decoder matches AIS-catcher's most sensitive model on a scored 10-minute recording.

**Different from the plan:**
- **Maps:** stored on the **microSD card** as Web Mercator **tile packs** (256 px tiles, one pack per zoom level), not as PNGs in LittleFS. This allows panning up to 10 nm and zoom levels from 0.5 to 10 nm. The LittleFS partition is unused.
- **AIS parser:** lives in `firmware/components/ais`; the vessel table and network client are in `main/ais`.
- **Host tests:** plain C programs in `firmware/test/host` (run with `run_tests.sh`), not Unity. A host **UI preview** renders the real screens to PNG.
- **Licence:** the RTL-SDR tuner code is ported from librtlsdr, so the firmware is GPL-2.0-or-later (decided in `RTL_SDR_PLAN.md`). The ported files carry GPL headers, but there's no top-level `LICENSE` file yet.

**Next:**
1. **Display firmware updates from the web page** (next section): **built, not yet tested on the display.** It needs one last USB flash, for the new partition table and bootloader; then a test update, and a test of rollback with a deliberately broken image.
2. **RTL-SDR:** confirm the live decoder on hardware with a second scored recording, test unplug/replug, then re-sending what it receives (`RTL_SDR_PLAN.md`, steps 5 and 6).
3. **Add the `LICENSE` file** (GPL-2.0-or-later).

## Firmware updates over the web (built 2026-10-05)

Upload `firmware/build/ais_display.bin` on the config page, the same way the Wi-Fi chip's firmware is updated now.

- **Partition table:**
  - Two 4 MB app slots, `ota_0` and `ota_1` (the app is about 2.2 MB), plus `otadata`, replacing `factory` and the unused LittleFS `storage` partition.
  - NVS stays at the same offset, so the settings survive the change.
  - Changing the table means flashing over USB once (`idf.py flash`, which writes the bootloader, partition table and app). After that, all updates go through the web page.
- **Upload (`POST /api/firmware`):**
  - Streams into the inactive slot with `esp_ota_write`.
  - Before anything is written, the image header is checked: right chip (ESP32-P4), right magic byte. `esp_ota_end` then verifies the whole image (SHA-256).
  - Only then does it switch the boot slot and restart.
  - A failed or interrupted upload leaves the running firmware untouched.
- **Rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`):
  - A new image boots on trial.
  - It marks itself good once it's running properly: the display up, the web server started, and a minute passed without crashing.
  - If it crashes or resets before that, the bootloader goes back to the previous image on the next boot.
  - Without this, a bad update could only be fixed over USB.
- **Version:**
  - The page shows the running version (`git describe`, through `PROJECT_VER`) and its build date.
  - After an update, it shows whether the display is running the new image or rolled back.

## 1. Hardware (derived from `waveshare-esphome`)

| Item | Detail | Source in ESPHome config |
|---|---|---|
| Board | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C, 16 MB flash, PSRAM @ 200 MHz, CPU 360 MHz | `esp32:` / `psram:` |
| Display | 800×800 round, MIPI-DSI, RGB888 (`WAVESHARE-P4-TOUCH-3.4C` model) | `display: mipi_dsi` |
| MIPI PHY power | On-chip LDO channel 3 @ 2.5 V | `esp_ldo:` |
| Backlight | LEDC PWM on GPIO26, **inverted**, 100 Hz | `output:` |
| Touch | GT911 on I2C (SDA GPIO7, SCL GPIO8, 400 kHz); swap_xy + mirror_x | `i2c:` / `touchscreen:` |
| Wi-Fi | ESP32-C6 co-processor via ESP-Hosted over SDIO: reset GPIO54, CMD GPIO19, CLK GPIO18, D0–D3 GPIO14–17 | `esp32_hosted:` |

The panel init sequence, DSI lane count, and timings aren't in the YAML. They live in ESPHome's `mipi_dsi` model definition (PR #10425) and in Waveshare's BSP for this board. Step 1 pulls them from there.

## 2. Technology choices

- **ESP-IDF v5.4+** (plain IDF, CMake, no ESPHome or Arduino). Supports the P4, `esp_lcd` MIPI-DSI, and `esp_wifi_remote` + `esp_hosted`.
- **ESP Component Registry deps:** `lvgl/lvgl` (v9.x), `espressif/esp_lvgl_port`, `espressif/esp_lcd_touch_gt911`, `espressif/esp_wifi_remote`, `espressif/esp_hosted`, `joltwallet/littlefs`, plus the Waveshare BSP or panel driver component if one fits (otherwise a custom panel driver using ESPHome's init sequence).
- **UI:** LVGL 9 with double-buffered framebuffers in PSRAM (800×800 RGB565 ≈ 1.25 MB each).
- **Storage:** NVS for settings, LittleFS partition (about 8 MB) for map PNGs.
- **Map decode:** PNG decoded once into PSRAM (LVGL's lodepng/libpng decoder, or decode at upload time to a raw RGB565 `.bin` for fast boot).
- **Map builder:** Python 3.12 with `requests`, `Pillow`, `pyproj`. Optional GDAL for the offline S-57 path.

## 3. Firmware architecture

```
firmware/
  CMakeLists.txt, sdkconfig.defaults, partitions.csv, idf_component.yml
  main/
    app_main.c          – init order, task creation
    board/              – LDO, DSI panel, backlight, GT911, I2C (one file per peripheral)
    net/
      wifi_mgr.c        – STA/AP state machine, credential fallback
      web_server.c      – esp_http_server, config UI + JSON API + map upload
      web/ (index.html, app.js – embedded via EMBED_FILES)
    ais/
      nmea_client.c     – TCP client task, reconnect/backoff, line framing
      nmea.c            – timestamp/TAG-block stripping, checksum, fragment reassembly
      ais_decode.c      – 6-bit payload decoding for supported msg types
      vessel_db.c       – mutex-protected table in PSRAM, 10-min expiry
    tide/tide.c         – NOAA CO-OPS fetch over HTTPS, next high/low calc
    time/clock.c        – SNTP + POSIX TZ
    geo/geo.c           – haversine distance/bearing, lat/lon ⇄ map-pixel projection
    config/settings.c   – NVS-backed settings struct + change notifications
    ui/
      ui_map.c, ui_detail.c, ui_netinfo.c, ui_common.c (fonts, ship glyph)
  test/                 – host-built Unity tests for nmea/ais/geo (linux target)
tools/
  ais_sim.py            – TCP server that replays recorded or synthetic AIS (incl. timestamped lines)
mapbuilder/
  build_map.py, requirements.txt, README.md
```

**Tasks:** LVGL/UI (core 0), NMEA client (core 1), Wi-Fi manager, HTTP server, tide refresh (every 6 h), and a 1 Hz housekeeping timer for vessel expiry and the clock.

## 4. Feature details

### 4.1 AIS ingest
- TCP client only (no UDP), connecting to the configured `host:port`. Reconnects with exponential backoff and reconnects right away when the config changes.
- **Timestamp tolerance:** each line is scanned for the first `!` or `$`. Anything before it is discarded, which covers NMEA 4.x TAG blocks (`\c:1696300000*5A\!AIVDM…`), leading ISO/epoch timestamps, and other prefixes. Trailing junk after `*hh` is ignored.
- Checksum is validated, and `!AIVDM`/`!AIVDO` sentences are kept (any talker ID). Multi-fragment messages are reassembled by sequence ID and channel, with a 2 s timeout.
- **Decoded types:** 1/2/3 (Class A position), 5 (Class A static/voyage), 18 (Class B position), 19 (Class B extended), 24A/24B (Class B static), 27 (long-range position).
- **Everything else is silently ignored**, including other message types, other sentence types, and bad checksums. Each case increments a counter shown on the network page.
- Vessel record fields: MMSI, name, callsign, type, class (A/B), lat/lon, SOG, COG, true heading, dimensions A/B/C/D, nav status, destination, last-update time.

### 4.2 Vessel table
- Up to about 500 vessels, keyed by MMSI, in PSRAM, behind a mutex.
- The 1 Hz sweep removes vessels not heard from in **10 minutes**.
- The UI takes a snapshot copy for rendering, so the lock isn't held while drawing.

### 4.3 Map screen
- **Map image:** the background is the uploaded chart PNG for the current zoom level. Its scale (meters per pixel) comes from PNG `tEXt` metadata written by the map builder. Ships are projected against the configured center lat/lon, which is set automatically from the map bundle's manifest on upload, so it always matches the chart.
- **Zoom:** on-screen **+ / −** buttons inside the round safe area step through the pre-rendered zoom levels the builder produced (for example about 0.5, 1, 2, and 4 nm radius). Only the active level is decoded into PSRAM. A button is disabled at the first and last level.
- **Ships:**
  - Each ship is drawn as a pointed hull polygon rotated to true heading (falling back to COG when heading = 511).
  - When stopped and no heading is known, it's drawn as a circle.
  - Size = (A+B) / meters-per-pixel, clamped to a minimum of about 14 px so it stays visible and touchable. Width comes from C+D.
  - Color depends on vessel type (cargo, tanker, passenger, fishing, pleasure/sail, other).
  - The label below each ship shows name (or MMSI if unknown) and SOG in kn.
  - Ships off the visible circle aren't drawn.
- **Refresh:** ships redraw every 1–2 s from a snapshot. Drawing uses a single custom-draw layer above the map image, not one LVGL object per ship, which keeps it cheap with many targets.
- **Touch on ship:** hit-test returns the nearest ship within about 25 px and opens the detail page.
- **Border:**
  - **Time** in the configured timezone appears along the top arc.
  - **Tide** appears along the bottom arc: next high/low with time and height, plus a rising/falling arrow.
  - Text is drawn as curved labels following the circle.
- **Static center marker** shows the configured position.

### 4.4 Detail screen
- Shows name, MMSI, course, speed, heading, last update (as "x s/min ago" plus local time), AIS class, vessel type (text from the ITU type table 20–99), and distance plus bearing from the configured location in nm.
- Also shows callsign, dimensions, nav status, and destination when known.
- Layout is a vertically centered list that fits the circle, with a back button and swipe-to-dismiss.
- Values update live while the screen is open. If the vessel expires, the screen shows "lost contact".

### 4.5 Network info screen
- **Entry:** holding one point for **10 s**, with movement under about 20 px. A progress ring appears after about 2 s so the user knows a hold is registering. This hold detection is separate from the ship tap and button handling.
- **When connected:** shows SSID, IP, RSSI, AIS server status (connected, message counts), and tide fetch status.
- **Reset button (behind a confirmation step):** switches to AP mode only. All other settings and the saved map are kept.
  1. SSID `AIS-Display-XXXX`, using the last MAC bytes.
  2. Uses a randomly generated WPA2 password.
  3. Shows SSID, password, and `http://192.168.4.1` on screen.
  4. Runs a captive-portal DNS so phones open the config page automatically.

### 4.6 Wi-Fi manager
- **Boot:** loads the STA credentials from NVS. If none are stored, it starts in AP mode.
- **Credential change:**
  1. The new SSID/password is kept in RAM.
  2. The device tries to connect for 20 s.
  3. On success, the new credentials are committed to NVS.
  4. On failure, it reconnects with the **previous** credentials.
  5. If those also fail, it falls back to AP mode.
- The web UI polls a status endpoint to report the result.

### 4.7 Configuration web page
- Served from `esp_http_server` in both STA and AP modes. It's a single page with embedded HTML/JS and a JSON API:
  - `GET/POST /api/config`: AIS host:port, NOAA tide station ID (validated via the CO-OPS metadata API), timezone (IANA dropdown mapped to a POSIX TZ string), and center lat/lon. The center is filled in by map upload, and stays editable for manual correction.
  - `POST /api/wifi`: SSID/password, using the fallback flow above.
  - `POST /api/map`: streaming upload of the builder's `map_set.zip` (stored, uncompressed entries so the device can unpack them without inflating). The device reads `manifest.json`, validates each PNG (800×800, matching the manifest), and writes the files to LittleFS. It then **sets the configured center lat/lon from the manifest**. A new upload replaces the previous set.
  - `GET /api/status`: network, AIS counters, vessel count, free heap/PSRAM.
- Units are knots, nm and feet.

### 4.8 Tide and time
- **Time:** SNTP (`pool.ntp.org`) plus the POSIX TZ from settings.
- **Tide data:** `https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&interval=hilo&datum=MLLW&station=<id>&begin_date=<today>&range=72&time_zone=gmt&units=english&format=json`, fetched with the IDF certificate bundle.
  - Refreshed every 6 h and whenever the station changes.
  - The device works out the next high and low locally and converts to local time.

## 5. Map builder (`mapbuilder/build_map.py`)

```
python build_map.py --lat 47.6062 --lon -122.3321 [--radii-nm 0.5,1,2,4] [--size 800] -o out_dir/
```

- **Primary source:** NOAA's ENC-based **Chart Display Service** (WMS/REST export at `gis.charttools.noaa.gov`).
  - It renders the official NOAA ENC data with S-52 nautical symbology.
  - The builder requests a bbox in Web Mercator / UTM sized to the radius and stitches tiles if one request exceeds the server's size limit.
- **Offline alternative (`--source enc-local`):**
  1. Downloads the needed ENC cells (`.000`) from `charts.noaa.gov/ENCs/`, picking cells via the ENC product catalog by bbox.
  2. Renders DEPARE, LNDARE, COALNE, DEPCNT, SOUNDG, BOYSPP/LIGHTS, and similar layers with GDAL/OGR plus Pillow, using simplified S-52 colors.
- **Post-processing:**
  1. Crops square at the requested resolution.
  2. Applies an optional circular vignette. The device masks anyway.
  3. Quantizes or optimizes the PNG to keep uploads small.
  4. Writes the `tEXt` chunks `ais:center_lat`, `ais:center_lon`, `ais:m_per_px`, `ais:zoom_index`, `ais:projection` into each PNG as a backup copy of the georeference.
- It **always builds every zoom level** (default 4, set with `--radii-nm`), writing `map_z0.png … map_zN-1.png` and bundling them into `map_set.zip` for upload. The zip contains a `manifest.json` with `center_lat`, `center_lon`, `projection`, and a per-level list of `{file, radius_nm, m_per_px}`.
- **Note:** the builder's projection must match the device's projection. Use local equirectangular, centered on the configured point; at a few nm the error is negligible.

## 6. Build order and milestones

| # | Milestone | Done when |
|---|---|---|
| 1 | **Toolchain + board bring-up**: install ESP-IDF 5.4/5.5, create project and partitions, extract the panel init from ESPHome/Waveshare BSP; LDO, DSI panel, backlight, GT911 | A test pattern shows and touch coordinates log correctly, with the orientation transform verified |
| 2 | **LVGL integration**: esp_lvgl_port, PSRAM buffers, rotation/touch transform | A demo screen is touch-responsive at a smooth frame rate |
| 3 | **Wi-Fi via ESP-Hosted (C6)**: STA connect, check the C6 slave firmware version (may need a C6 firmware update), AP mode | It gets an IP, and AP mode is reachable |
| 4 | **Settings + web server**: NVS settings, config page, JSON API, Wi-Fi fallback flow | All fields persist across reboot, and bad Wi-Fi credentials revert |
| 5 | **AIS stack (host-testable first)**: NMEA framing, timestamp stripping, reassembly, decoder, vessel DB, plus `tools/ais_sim.py` | Unit tests pass on known vectors, and the device tracks simulated ships and expires them after 10 min |
| 6 | **Map builder**: WMS path, georef metadata, multi-zoom; then the optional local-ENC path | It produces correct charts for a test location (verified against a known landmark) |
| 7 | **Map upload + map screen**: LittleFS upload, decode, projection, ship rendering, zoom buttons, labels, hit-test | Simulated ships appear at correct positions, sizes, and headings |
| 8 | **Detail screen** | All fields display and update live, including distance |
| 9 | **Time + tide border UI** | Correct local time and next tide for a chosen station |
| 10 | **Network info screen + reset to AP** | The 10 s hold works without false triggers, and AP credentials show |
| 11 | **Hardening**: reconnect paths, malformed-input fuzzing of the parser, memory soak test with 300+ simulated vessels, watchdog | It runs for 24 h on the simulator with no leaks or resets |

## 7. Risks

- **ESP-Hosted / C6 firmware version mismatch:** the host component version must match the slave firmware on the C6. Plan for an OTA update of the C6 if needed.
- **Panel init sequence:** this board isn't in mainline ESPHome yet (it uses PR #10425). The init sequence comes from that PR or from Waveshare's demo code.
- **NOAA service availability and terms:** the WMS endpoint may change. That's why the offline ENC path is also planned.
- **PSRAM bandwidth:** two framebuffers plus several decoded zoom levels. Keep a single decoded zoom level in RAM and decode on zoom change, or pre-convert to RGB565 at upload.

## 8. Decisions (resolved)

1. **Zoom:** the builder always produces every zoom level, and the device steps through them.
2. **AIS transport:** TCP client only.
3. **Units:** knots, nautical miles, feet.
4. **Reset button:** switches to AP mode only. Settings are not wiped.
5. **Map center:** carried in the map bundle's manifest and applied automatically on upload, so the user doesn't have to enter it. The web UI field stays editable.
