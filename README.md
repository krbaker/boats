# AIS Round Display

A standalone display for local AIS traffic. It runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C (800×800 round touchscreen). It reads AIS from an NMEA 0183 TCP server, from an RTL-SDR dongle plugged into it (decoding AIS itself), or both at once. It shows the AIS contacts on a NOAA nautical chart, with the time and the next tide around the edge.

```
firmware/        ESP-IDF project (C, LVGL 9) for the display
coprocessor/     ESP-Hosted firmware for the board's ESP32-C6 Wi-Fi chip
enclosure/       OpenSCAD desk stand (and a full case), with ready-to-print STLs
mapbuilder/      Python tool that renders NOAA ENC charts into a map set for the display
tools/ais_sim.py AIS test server (simulated traffic or replay of a capture)
```

## Using the display

| Screen | How to get there | What it shows |
|---|---|---|
| **Map** | Default | The chart for the current zoom level, starting centred on your configured position. Drag with one finger to pan, up to 10 nm from the chart's centre. Each vessel is drawn as a hull: scaled to its reported length and beam, rotated to its heading, and coloured by vessel type. Hulls are to scale up to the third zoom level (2 nm on the standard chart); zoomed out further, they stay at that size so they remain visible. The hull's shape shows its heading, and a dashed line shows its course over ground and where it will be in 6 minutes (the two differ when current or wind sets it sideways). Its name and speed appear underneath. Vessels go dim after 3 minutes without a report (7 minutes if slow or at rest, since those report less often) and disappear after 10 minutes. **+ / −** on the right edge change the zoom level (0.5 to 10 nm radius). The time is at the top edge, the tide level and next high or low at the bottom, and the vessel count (or "AIS offline" / "AIS not set") on the left. **Weather** (if set up):
<ul><li>an icon after the time: sun or moon at its phase, cloud, rain, snow;</li><li>a red **!** when the marine zone has a watch, warning or advisory. When one is new or updated, it also pops up over the screen as a red box with its full text, until you tap its **✕**;</li><li>a yellow arrow at the rim pointing in from where the wind comes from, with its speed in mph (and the gust when it's 10 mph more);</li><li>a blue triangle pointing in from where the waves come from, with their height and period. Many buoys measure no wave direction: then the height and period go at the end of the tide bubble instead, after a small wave icon, and the status table warns that the direction is missing.</li></ul> |
| **Weather** | Tap the time at the top edge | **Forecast** tab: active alerts first (tap one for the full text), then the NOAA marine forecast for your zone. **Waves** tab: the NDBC buoy's waves, wind, temperatures and pressure. **Weather** tab: your WeatherLink station's wind, temperature, humidity, pressure and trend, and rain; the sky from the nearest NWS station; sunrise and sunset; the moon's phase. |
| **Tide** | Tap the tide at the bottom edge | Predicted water level from 6 hours ago to 24 hours ahead, with highs, lows and the current level. |
| **Vessels** | Tap the vessel count on the left edge | **Vessels** tab: every vessel heard, nearest first, with distance and speed; tap one for its details. **Receiver** tab: the RTL-SDR's state, decoder CPU load, gain, noise floor and tuning correction, then packets per channel from the SDR and from the network feed (last minute and since boot), and how many vessels only one of them heard. **Options** tab: minimum length (0–300 ft) and speed (0–50 kn) for vessels on the map, whether vessels that report no size or speed are kept, whether vessels below the limits lose just their label or aren't drawn at all, whose label wins when labels overlap (fastest or largest), and whether labels show names and/or speeds. |
| **Vessel detail** | Tap a vessel | Name, MMSI, call sign, AIS class, vessel type, navigational status, course, heading, speed, distance and bearing from your position, size, destination, and when it was last heard. It updates live. Swipe right or tap **Map** to go back. |
| **Network** | Hold one finger anywhere for **10 s**. A countdown ring appears after 2 s. | The Wi-Fi network, IP address and config page URL, plus AIS feed, RTL-SDR and tide status. **Reset to AP mode** (tap twice to confirm) switches the display to a setup access point and shows its SSID and password. Saved settings are kept. |

Vessel colours: green cargo, red tanker, blue passenger, orange fishing, violet sailing or pleasure, teal tug, amber pilot / SAR / law enforcement / high-speed craft, grey other or unknown. They're chosen to avoid the chart's own colours, and each hull has a dark outline inside a light rim so it stands out on water and land alike.

## Configuration web page

Open `http://<display-ip>/`. The Network screen shows the address. In setup mode it's `http://192.168.4.1/`, and most phones open the page automatically after you join the access point.

- **AIS data server**: host and TCP port of the NMEA 0183 feed, and whether to use it. Timestamped feeds are fine: NMEA 4 TAG blocks, leading ISO or epoch timestamps, and trailing fields are all handled. Unsupported or corrupt sentences are counted and ignored. Decoded message types: 1, 2, 3, 5, 18, 19, 24 and 27.
- **Map**: upload the `map_set.zip` produced by the map builder; the page shows progress. It's stored on the display's microSD card, which must be formatted **FAT32** (cards over 32 GB usually come as exFAT and need reformatting). The chart's centre becomes the configured position. You can still edit the centre latitude and longitude by hand.
- **Tides & time**:
  - **Tide station**: a NOAA CO-OPS tide station ID. Find one on the [station map](https://tidesandcurrents.noaa.gov/map/).
  - **Time zone**: pick one from the list, or enter a custom POSIX TZ rule.
  - **Weather**, each optional:
    - **NOAA marine forecast zone** (such as PZZ135): the forecast and alerts.
    - **NDBC buoy** (such as 46088): waves.
    - **NWS observation station**: the sky for the icon. Empty means the station nearest the chart's centre.
    - **WeatherLink API key and secret** (v2, from weatherlink.com): your Davis station. **Find my stations** lists your stations and their outdoor sensor suites to choose from. The secret is stored on the display and never shown again.
  - **Display brightness**.
- **Wi-Fi**: the display tests a new network for 20 s before saving it. If it can't join, it reconnects to the previous network, or falls back to the setup access point if that fails too. The page reports the result.
- **Display firmware**: shows the running version and its build date, and updates the display from an uploaded `firmware/build/ais_display.bin`. The display restarts into the new firmware on trial. If that doesn't keep running for a minute (it crashes or restarts), it goes back to the previous firmware by itself, and the page says so. A failed or interrupted upload changes nothing.
- **Wi-Fi chip firmware**: shows the ESP32-C6's firmware version, and updates it from an uploaded image (`coprocessor/build/eh_cp_ota_coprocessor_ota.bin`). The display restarts afterwards. A failed update leaves the old firmware running.
- **RTL-SDR receiver**: see below.
- **Restart display**, and **Forget all vessels** (see below).

The page also shows live status: Wi-Fi, AIS feed and message counters, vessel count, remembered vessels, tide, map, microSD card space and memory.

**Remembered vessels:** the display keeps the name, call sign, type and size of every vessel it has heard (up to 20,000) in `vessels/cache.log` on the microSD card. A vessel heard again shows its name with its first position report, instead of waiting up to 6 minutes for its next static data message. What a vessel sends always replaces what was remembered. Destination isn't remembered, since it changes every voyage. Without a card it's kept in memory only, until the next restart. `GET /api/vessels` returns the current vessel table as JSON, including how many messages came from each source.

## RTL-SDR receiver

Plug an **RTL-SDR Blog V3** dongle (R820T2 tuner) with a VHF antenna into the display's USB-A port, and the display decodes AIS on both channels itself, alongside the network feed. Both sources feed the same vessel table. Never connect the dongle to an antenna a VHF radio transmits on; use a splitter made for AIS.

On the config page:

- **Decode AIS from the dongle**: on or off.
- **Gain**:
  - **Auto: best fixed gain** (the default) measures the noise at each gain step when the dongle starts, taking up to 25 s. It picks the highest gain that leaves about 20 dB of headroom, and steps down by itself if a nearby transmitter overloads it.
  - **Tuner AGC**: the tuner's own automatic gain. Usually worse for AIS's short bursts.
  - **Manual**: any of the tuner's 29 steps.
- **Frequency correction (ppm)**: the V3 needs none, and the decoder also corrects itself once it hears packets.
- **Bias-tee**: puts 4.5 V on the antenna socket to power an LNA or active antenna. Leave it off otherwise.
- **Record**: saves up to 10 minutes of the raw signal (8-bit I/Q, 288 kS/s, centred on 162.000 MHz, the same as `rtl_sdr` writes; about 35 MB a minute) to the microSD card. It also saves every AIS sentence heard meanwhile from the network feed and from the SDR, timed to the I/Q. Both download from the page. They're for testing the decoder on a PC; see below.
- **Retransmit**: sends what the dongle decodes on to up to three destinations, each UDP or TCP to an IP address (or host name) and port. Use it to feed AIS sharing services, which assign your station an address and port when you register it (MarineTraffic, AIS Friends, aiscatcher.org), or chart software such as OpenCPN (set it to listen for UDP). TCP connects out to the destination and reconnects by itself. Only the dongle's sentences are sent, never the network feed's. Each destination shows its state, sentences sent and not sent, and any error; the display's Network screen shows how many are sending.

The decoder uses about 60% of one CPU core. The Receiver tab and the config page show how the SDR compares with the network feed.

## Building a map

The builder needs Python 3 and Pillow:

```sh
cd mapbuilder
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python build_map.py --lat 47.6005 --lon -122.3550 -o seattle
# optional: --radii-nm 0.25,0.5,1,2,4,10   one zoom level per view radius (default 0.5,1,2,4,10)
#           --pan-nm 10                      how far from the centre you can pan (default 10)
#           --wash 0.3                       fade the chart toward white so vessels stand out (0 = NOAA colours)
```

Each zoom level is rendered over the whole pan area as one large image (up to 16384 px square), so labels and detail are consistent across it, then cut into 256 px tiles stored as one pack file per level. The closest zoom level needs about 2 GB of RAM while it's cut up. If the chart service starts refusing such big renders, add `--max-render-px 14400` (its advertised limit) and a level that's too big is split into a few blocks. The output is:

- `seattle/ais_map/`: `manifest.json` and `z0.pak … z4.pak`. Copy this folder to the root of the display's microSD card and restart the display, **or**
- `seattle/map_set.zip`: the same files, to upload on the config page.

With the defaults a chart set is around 10 MB (9.3 MB for Seattle; open water and solid land compress to almost nothing) and takes about 2 minutes to build. Charts are rendered in Web Mercator (EPSG:3857); a chart set made by an older version of the builder is rejected and must be rebuilt. The charts come from NOAA's ENC-based Chart Display Service, which draws official NOAA ENC data with standard (S-52) nautical chart symbols. The info, data-quality and overscale overlays are turned off to reduce clutter; `--list-layers` and `--layers` control this. Only US waters are covered.

## Building and flashing the firmware

You need ESP-IDF **v5.5 or later**:

```sh
git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32p4
. ~/esp/esp-idf/export.sh

cd firmware
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

After the first USB flash, later updates can go through the config page (**Display firmware**). Flash over USB again only when the partition table or bootloader changes. A display flashed before web updates existed needs one USB flash to get the two-slot layout (its settings are kept).

On first boot there's no saved Wi-Fi network, so the display starts in setup mode with an `AIS-Display-XXXX` access point and shows the password on screen. Join it, open `http://192.168.4.1/`, and enter your Wi-Fi network, AIS server, tide station and time zone. Then insert a FAT32 microSD card with a chart set on it, or upload one.

### Hardware notes and troubleshooting

- **Chip revision.** The ESPHome config ran the CPU at 360 MHz, which only early (pre-v3.0) ESP32-P4 chips use. `sdkconfig.defaults` therefore builds for revisions below v3. If `esptool.py --chip esp32p4 chip_id` reports v3.x, delete the two `ESP32P4_*REV*` lines from `firmware/sdkconfig.defaults`, then run `rm sdkconfig && idf.py build`. A build for the wrong revision won't boot.
- **Touch lands in the wrong place.** Set the swap and mirror options under `idf.py menuconfig` → *AIS Display*. The defaults (all off) come from the Waveshare BSP. Your ESPHome config used `swap_xy` + `mirror_x`, but ESPHome applies its own transform on top, so the two may not match.
- **No Wi-Fi.** Wi-Fi runs on the board's ESP32-C6 co-processor through ESP-Hosted over SDIO. If the log shows ESP-Hosted version or RPC errors, the C6's firmware doesn't match the host library. Build `coprocessor/` (`. ~/esp/esp-idf/export.sh; cd coprocessor; idf.py set-target esp32c6; idf.py build`; the `set-target` step is only needed on a fresh checkout). Upload `coprocessor/build/eh_cp_ota_coprocessor_ota.bin` on the config page. If the C6's current firmware is too old to accept that, flash it once over its own USB/serial port with `idf.py -p <port> flash`.
- **Pins** come from the ESPHome config and the Waveshare BSP. Display: MIPI-DSI, 2 lanes, JD9365 controller, reset on GPIO27. Backlight: GPIO26 PWM, inverted. Touch: GT911 on I2C, GPIO7/8. C6: SDIO CLK 18, CMD 19, D0–D3 14–17, reset 54.

## Testing without a boat

```sh
python3 tools/ais_sim.py --lat 47.6005 --lon -122.3550            # serves TCP port 10110
python3 tools/ais_sim.py --timestamps tag --noise                 # timestamped feed plus junk lines
python3 tools/ais_sim.py --replay capture.nmea --rate 50          # replay a recorded feed
```

Point the display's AIS server at the machine running the simulator.

Host-side unit tests cover the AIS parser, the AIS demodulator (synthetic GMSK packets at different SNRs and frequency offsets), the geo maths, a simulator→parser round trip, the map-upload zip reader and the tile packs:

```sh
firmware/test/host/run_tests.sh
AIS_CAPTURE=capture.cu8 firmware/test/host/run_tests.sh   # also decode a real I/Q capture
```

To score the demodulator against a reference receiver, record from the config page (I/Q plus the NMEA log), then:

```sh
firmware/test/host/build/ais_capture_score capture.cu8 capture.nmea -v
```

This decodes the I/Q again with the current code and matches each packet the network feed heard against the SDR's, by payload. It reports what fraction the live and offline decoders got, sorts the misses by how strong a signal there was (nothing heard, or a clear burst the decoder should have got), and flags SDR packets from vessels the network never heard as possible false decodes.

To see the screens without the hardware, the host preview runs the real UI code against a simulated fleet and tide and writes PNGs of the map (zoomed, panned, filtered) and the overlays. It needs a firmware build first, and optionally a map builder output directory for the chart:

```sh
firmware/test/host/preview/build.sh mapbuilder/seattle     # PNGs in firmware/test/host/preview/out/
```

## Desk stand

`enclosure/desk_stand.scad` (`desk_stand.stl`) is a one-piece stand that screws into the four M4 threaded holes on the back of the display (a centred 75 × 75 mm square, the VESA 75 pattern): an X-shaped bracket on four posts, a single leg and a 110 × 95 mm base. Nothing wraps around the screen. The display leans back 15° with its bottom edge about 20 mm above the desk, so the USB-C cable can bend down; it then runs in a groove in the base and out of the back. Print it base down with no supports, and fix it with four M4 × 16 mm screws, which bite about 4 mm into the display. Check how deep the threaded holes are first, so the screws can't bottom out against the display; adjust `thread_engage` at the top of the file if needed.

`enclosure/display_case.scad` is an alternative full case (bezel, back lid and a cradle stand) that wraps around the display.

## Licence

GPL-2.0-or-later: you can redistribute and modify this under the terms of the GNU General Public License, version 2 or (at your option) any later version. The full text is in `LICENSE`. The firmware is GPL because the RTL-SDR tuner driver is ported from librtlsdr.
