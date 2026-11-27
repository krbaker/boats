# Plan: more boards, a headless receiver, and a TCP NMEA server

Today the firmware runs on one board, the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C (800×800 round). This plan adds three more, plus a TCP server so a display can show what a remote receiver hears.

| Board | Screen | Network | What's new |
|---|---|---|---|
| **ESP32-P4-WIFI6-Touch-LCD-3.4C** (today) | 800×800 round | Wi-Fi (C6) | nothing, except the TCP server |
| **ESP32-P4-WIFI6-Touch-LCD-4C** | 720×720 round | Wi-Fi (C6) | screen layout scaled down from 800 px |
| **ESP32-P4-WIFI6-Touch-LCD-10.1**, landscape | 1280×800 rectangle | Wi-Fi (C6) | bubbles in a bar along the bottom; wind and wave arrows at the screen edge |
| **ESP32-P4-WIFI6-POE-ETH** (headless) | none | Ethernet (PoE) first, Wi-Fi fallback | no screen, no weather, tide, sun/moon or buoy fetching |

## Decisions so far

- **10.1" runs in landscape** (1280×800).
- **On the rectangle,** the bubbles go in a bar **along the bottom**, left to right: **Vessels** (count / status), **Time** (with the weather icon and alert), **Tide** (with the wave text when the buoy has no direction), **Zoom** (− radius +). The wind and wave **arrows stay**: each is drawn where its bearing meets the edge of the screen, pointing in.
- **Headless** skips everything that only feeds the screen: weather (NWS forecast, alerts, observations, WeatherLink), tide, sun and moon, the NDBC buoy, **ADS-B** and the chart. It keeps the SDR decoder, the network client, the vessel table (and its card cache), Retransmit, the TCP server and the web page.
- **Headless network:** Ethernet when the cable has a link. Without one, Wi-Fi as today (saved network, then the setup access point).
- **Headless status:** on the web page only. The board has no LED the firmware can drive.
- **Headless "Reset to AP mode":** power-cycle it three times quickly.
- **The TCP server sends only the dongle's sentences**, the same rule as Retransmit: never the upstream network feed, so two units pointed at each other can't loop.

## One firmware per board

The display driver, LCD size and BSP component are fixed at build time, so each board gets its **own build**. A single image that detects its board isn't practical: the BSPs pick their panel with Kconfig, and the headless build drops LVGL entirely (about 1 MB of code and 6+ MB of PSRAM buffers).

- **Kconfig choice** `AIS_BOARD` in `main/Kconfig.projbuild`: `AIS_BOARD_LCD34C`, `AIS_BOARD_LCD4C`, `AIS_BOARD_LCD10`, `AIS_BOARD_HEADLESS`. Derived symbols: `AIS_HAS_DISPLAY`, `AIS_DISPLAY_ROUND`, `AIS_HAS_ETH`.
- **sdkconfig overlays**, one per board, on top of the shared `sdkconfig.defaults`:
  `sdkconfig.defaults.lcd34c`, `.lcd4c`, `.lcd10`, `.headless`. Each sets the board choice and its BSP's options (for example `CONFIG_BSP_LCD_TYPE_720_720_4_INCH`).
- **Build:** one build directory per board, so they don't overwrite each other:
  ```
  idf.py -B build-lcd10 -D SDKCONFIG=build-lcd10/sdkconfig \
         -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.lcd10" build
  ```
  A small `build.sh <board>` wraps this. Plain `idf.py build` keeps building the 3.4C, as now.
- **Dependencies:** `idf_component.yml` pulls each BSP only for its board (component manager `rules:` on `$CONFIG{...}`), and LVGL only when there's a display. If the manager's Kconfig rules don't work for this, the fallback is a per-board manifest selected in `CMakeLists.txt`.
- **Wrong-board guard:** each build stamps its board into the app description (`project_name` = `ais_display_lcd10` etc.). A web firmware upload reads it from the first block of the image and **refuses a different board's firmware**, before anything is written to flash. The page shows the running board next to the version.
- **Code layout:** `main/ui/`, `map/map_tiles`, `weather/`, `tide/`, `adsb/` and `components/weather_parse` are compiled only when `AIS_HAS_DISPLAY`. `fw_update.c` (the only non-UI file that includes `ui.h`) calls the UI through a hook that's a no-op when headless. `app_main.c` starts tasks according to the board.

## Screen geometry

Today `UI_SIZE` is 800 and `UI_CENTER` 400, used in `ui.c`, `ui.h` and 28 places in `ui_map.c`, plus pixel constants (`WIND_TAIL_R 356`, `VALUE_X 400`, …) written for 800 px.

- `ui.h` gets `UI_W`, `UI_H` (from `BSP_LCD_H_RES` / `V_RES`, after rotation) and `UI_CX`, `UI_CY`. `UI_SIZE` / `UI_CENTER` go.
- **Round 720:** radii and positions written for 800 become `UI_PX(356)` = scaled by `UI_W / 800`. Fonts stay as they are (720 px on a 4" screen is larger per pixel than 800 on 3.4", so the same fonts read bigger). Tap targets keep their minimum sizes.
- **Map tiles:** `map_tiles.c` works out which tiles are visible from the view's width and height, not a circle. 1280×800 needs about 6×5 tiles plus the prefetch ring: `MAX_WANT 64` is enough; `CACHE_TILES 96` (12 MiB) stays.
- **Other screens** (Weather, Tide, Vessels, Detail, Aircraft, Network, Safety): they're centred boxes inset to fit the circle. On the rectangle they're centred and use the full height, with the box width capped so lines don't run 1280 px wide. Each is checked in the host preview at all three sizes.

## Rectangular map overlay (10.1")

- **Bottom bar:** the arclabel pills (`pill_label`, `pill_layout`) are round-only. A second implementation draws the same pills as straight rounded rectangles in a row along the bottom, using normal labels, in this order: **Vessels** (`PILL_STATUS`), **Time** (`PILL_TIME`), **Tide** (`PILL_TIDE`), **Zoom** (`PILL_ZOOM_OUT`, `PILL_ZOOM`, `PILL_ZOOM_IN`). They keep the same text, icons, colours and taps (time → Weather, tide → Tide, count → Vessels, ± → zoom). The wave text that goes in the tide bubble when the buoy has no direction still goes there.
- **Arrows at the edge:** the wind and wave arrows, today at a fixed radius (`WIND_TAIL_R` … `WIND_TIP_R`), are placed where their bearing from the chart's centre crosses the screen edge, inset so they stay inside, and above the bottom bar.
- **Panning and the "10 nm from centre" limit** don't change. Zoom levels stay in nautical-mile radius, measured to the **shorter** half-dimension (400 px), so "2 nm" still means 2 nm to the top edge.
- **The 10 s hold for the Network screen** and its countdown ring work the same.
- **Before building it:** a mockup of the bar (sizes and spacing) from the host preview, to check how it looks.

## Headless (ESP32-P4-WIFI6-POE-ETH)

- **No display:** no `bsp_display_start`, no LVGL, no UI tasks. Status is on the web page only.
- **Not started:** `weather_start`, `tide_start`, `adsb_start`, `map_tiles_init`. Their settings are hidden on the web page and ignored if posted.
- **Kept:** SDR (`sdr_probe_start`, USB host on the board's USB-A), `nmea_client` (a headless unit can also take a network feed), `vessel_db` and its card cache, Retransmit, the TCP server, the web server, firmware updates for the P4 and the C6.
- **Web page:** `/api/status` and `/api/config` report `board` and a `features` list (`display`, `weather`, `tide`, `adsb`, `map`, `eth`). The page hides the sections the board doesn't have. The status shows vessels from `/api/vessels` as a table, since there's no screen to see them on.
- **Board file:** no BSP covers this board, so the headless build has its own `main/board/headless.c` for SD, USB and Ethernet.

### Board pins (from Waveshare's schematic, ESP32-P4-WIFI6-POE-ETH)

| What | Pins | Notes |
|---|---|---|
| microSD, 4-bit SDMMC | CLK 43, CMD 44, D0 39, D1 40, D2 41, D3 42 | Card power through a transistor on **GPIO45**. No card-detect pin. |
| USB-A host | P4's USB 2.0 HS port | 5 V comes through a DIO7003 power switch whose enable is tied on. No GPIO. |
| Ethernet, IP101 (RMII) | MDC 31, MDIO 52, PHY reset 51, REF_CLK in 50, TXEN 49, TXD0 34, TXD1 35, CRS_DV 28, RXD0 29, RXD1 30 | ESP-IDF's P4 defaults. PHY address set by straps: use auto-detect. |
| ESP32-C6 (ESP-Hosted, SDIO) | GPIO14–19 | As on the 3.4C. The C6 reset GPIO is to confirm on the board (54 on the 3.4C). |
| BOOT button | GPIO35 | Shared with Ethernet TXD1, and it's the boot-mode strap: **can't be read while running**. |
| RESET button | chip enable | |
| LED | — | Only a 5 V power LED. |
| I2C | SCL 8, SDA 7 | Audio codec. Not used. |
| Free on the 2×20 header | 20–27, 32, 33, 46–48 among others | |

## Ethernet, with Wi-Fi fallback

- **Driver:** the P4's internal EMAC with the IP101 PHY (`esp_eth`), DHCP. Pins from Waveshare's schematic (to confirm: MDC, MDIO, PHY reset, 50 MHz REF_CLK).
- **`wifi_mgr` becomes `net_mgr`,** with Ethernet in front of the Wi-Fi state machine that exists today:
  - At boot, wait up to 5 s for an Ethernet link. Link up → DHCP on Ethernet, and Wi-Fi isn't started.
  - No link, or the link drops for 10 s → start Wi-Fi exactly as today (saved network, then the setup access point and its 10-minute timeout).
  - Link back → move to Ethernet and stop Wi-Fi.
- **Everything that waits for the network** (forwarder, TCP server, NMEA client, web server, the connectivity probe) uses `net_mgr`'s "have an address" instead of the Wi-Fi one, so they work on either.
- **Status:** Network section on the web page says Ethernet or Wi-Fi, address, link speed.
- **Reset to AP mode** without a screen: **power-cycle three times quickly** (each power-on less than ~5 s after the last). A counter in RTC memory / NVS counts boots that didn't reach 5 s of uptime and clears itself at 5 s. On the third, the unit does what the Network screen's button does. (The BOOT button can't be used: see the pin table.)

## TCP NMEA server (all boards)

So a display can show what a headless receiver elsewhere hears: the receiver serves its sentences, and the display's existing **AIS data server** setting connects to it. Nothing new is needed on the display side.

- **What's sent:** the dongle's `!AIVDM` sentences with CR LF, a multi-part message as a unit: the same stream Retransmit sends. Never the upstream feed.
- **How:** one more output of the forwarder in `ais_fwd.c`. It already groups fragments and queues them without blocking the decoder. Its task gains a listening socket and up to **4 clients**, non-blocking, each with its own pending buffer like the TCP destinations. A client that takes nothing for 30 s is dropped. Nothing is buffered for a client that's just connected: it gets sentences from then on.
- **Settings:** `srv_on` (off by default), `srv_port` (default **10110**, the usual NMEA port). Saving applies live, through `SETTINGS_EV_FWD`.
- **Sockets:** listener + 4 clients = 5 more. `CONFIG_LWIP_MAX_SOCKETS` goes from 20 to 24.
- **Status:** clients connected (with their addresses), sentences sent, dropped. In `/api/status` as `nmea_server`, in the web page's Retransmit section, and as one line on the Network screen's Retransmit tab.
- **Loop safety:** a unit with both the TCP server on and an AIS data server set never re-serves what it receives, since only dongle sentences are served.

## Steps

| # | Step | Done when |
|---|---|---|
| 1 | **TCP NMEA server** on today's board: settings, server in the forwarder, status on the web page and Network screen. | `nc <display> 10110` from two PCs both show the dongle's sentences; a second display with its AIS data server pointed at the first shows the same vessels. Killing a client doesn't affect the other. |
| 2 | **Board selection and build variants:** Kconfig choice, sdkconfig overlays, `build.sh`, conditional dependencies, board stamp and the wrong-board guard. | The 3.4C build behaves exactly as before. Uploading another board's image is refused with a message. |
| 3 | **Screen geometry:** `UI_W` / `UI_H`, scaled constants, tiles from width and height; **4C round 720** build. Host preview at 720. | The 4C build runs on the board with the same layout as the 3.4C, scaled. |
| 4 | **Bottom bar mockup** in the host preview at 1280×800: Vessels, Time, Tide, Zoom. | You've seen it and it looks right. |
| 5 | **10.1" build:** its BSP, the bottom bar, edge arrows, the other screens fitted to the rectangle. | Every screen works on the 10.1" board; taps, zoom, pan and the 10 s hold behave as on the round one. |
| 6 | **Headless build:** no UI or LVGL, the screen-only tasks not started, the web page's feature flags, power-cycle AP reset, board file for SD and USB. Runs on Wi-Fi first. | The POE-ETH decodes from the dongle, serves TCP, retransmits, and its web page shows only what it has. |
| 7 | **Ethernet with Wi-Fi fallback** (`net_mgr`). | Pulling the cable moves it to Wi-Fi within ~15 s, plugging it back moves it to Ethernet, and the TCP clients reconnect each time. |
| 8 | **Docs:** README per board, build instructions, a "remote receiver" section. | |

Step 1 is independent and useful on the current board, so it goes first. Steps 2–3 change nothing visible on the 3.4C but touch most UI files, so they're best done in one go and checked against the 3.4C before going on.

## Open points

- **10.1" fonts:** the round screens' fonts may look small at 10" from across a cabin. Decide after seeing it on the board.

## Progress (2026-11-26)

- **Step 1 built, host-tested, not yet on the display.**
  - **Settings:** `srv_enabled`, `srv_port` (NVS `srv_on`, `srv_port`; default off, 10110). In `/api/config` as `nmea_server: {enabled, port}`. Port 80 is refused (the web page). A change raises `SETTINGS_EV_FWD`.
  - **Server:** in `ais_fwd.c`, in the forwarder task. Listener + 4 client slots, non-blocking. A fifth client is closed and counted as `refused`. A client stuck 30 s is dropped. Port in use → error, retried with the destinations' back-off.
  - **Status:** `/api/status` `nmea_server` (state, port, sent, per minute, dropped, refused, error, clients with address, connected time, sent, dropped). On the web page under Retransmit; a row with a switch on the display's Retransmit tab; one line on the Network screen.
  - **Host test:** `test_ais_fwd.c` covers two clients, a multi-part message, a client leaving, the fifth turned away, port in use, Wi-Fi loss and turning it off.
  - **lwIP:** `CONFIG_LWIP_MAX_SOCKETS` 20 → 24, `CONFIG_LWIP_MAX_ACTIVE_TCP` 16 → 24.
- **mDNS** (added with step 1): `espressif/mdns` 1.15. The `mdns_name` setting (default `ais`, NVS `mdns_name`, `SETTINGS_EV_NAME`) is the `.local` name and the DHCP host name. Advertises `_http._tcp` (80) and, while the server is on, `_nmea-0183._tcp`. `main/net/mdns_adv.c`, its task in PSRAM; the component's task and memory in PSRAM, console CLI and browsing off. The name in use (after any clash) is in `/api/status` `wifi.mdns_host`, on the web page and the Network screen.
  - Adding the dependency re-solved the others: `main/idf_component.yml` now holds `esp_hosted` (3.0.9, to match `coprocessor/`), `button`, `esp_lv_decoder` and `zlib` at their old versions. Only the skipped optional `freetype` and `libpng` moved in the lock.
- **Step 2 built** (2026-11-27): `AIS_BOARD` in `CMakeLists.txt` (default `lcd34c`; `build.sh <board>` passes it with a build directory per board) picks `sdkconfig.defaults.<board>` and the project name: `ais_display` for the 3.4C, so displays in use still take its updates, and `ais_display_<board>` otherwise. The existing project-name check in `fw_update.c` is the wrong-board guard, from the first block of the upload. Configure stops if a build directory's sdkconfig is for another board. Kconfig: the `AIS_BOARD` choice, `AIS_BOARD_NAME`, `AIS_BOARD_TITLE`, `AIS_HAS_DISPLAY`, `AIS_DISPLAY_ROUND`, `AIS_HAS_ETH`. lcd4c, lcd10 and headless stop at compile time with an `#error` naming their step. The web page shows the board.
  - **Deferred to step 6:** per-board component dependencies (no BSP or LVGL for headless). Nothing needs them until there's a board without the BSP.
  - **Open:** the chip revision. `sdkconfig.defaults` builds for P4 revisions below v3 (the 3.4C here). Newer boards may be v3.x, which needs those two lines moved into the board overlays: check with `esptool.py chip_id` on each board.
- **Step 3 built** (2026-11-27), not yet on the 4C. `ui.h`: `UI_SIZE` is 720 for lcd4c, and `UI_PX(n)` scales a length laid out for 800. Scaled: the boxed screens (Vessels, Weather, Aircraft, Network), popups, the detail screens' rows and columns, the tide graph, the map's message box. Measured in from the rim instead: the wind and wave arrows (the bubbles are a fixed thickness). Not scaled: fonts, icons, tap targets. At 720 only: the Network tab bar in the 16 px font (six tabs), and table cells with 10 px padding (`ui_vessels.c`). At 800 everything comes out as before.
  - **Charts:** the same chart set on both. The 4C shows 90% of each level's radius, and the zoom bubble says so (1.8 nm for a 2 nm level).
  - **Chip revision:** now per board. The pre-v3 lines are in `sdkconfig.defaults.lcd34c`; the 4C (v3.2 here) uses ESP-IDF's defaults (v3.1+, 400 MHz).
  - **Host preview:** `SIZE=720 test/host/preview/build.sh` renders every screen at 720 into `out-720/`.
