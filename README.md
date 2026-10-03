# AIS Round Display

A standalone display for local AIS traffic. It runs on the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C (800×800 round touchscreen) and reads AIS from an NMEA 0183 TCP server. It shows the AIS contacts on a NOAA nautical chart, with the time and the next tide around the edge.

```
firmware/        ESP-IDF project (C, LVGL 9) for the display
mapbuilder/      Python tool that renders NOAA ENC charts into a map set for the display
tools/ais_sim.py AIS test server (simulated traffic or replay of a capture)
```

## Using the display

| Screen | How to get there | What it shows |
|---|---|---|
| **Map** | Default | The chart for the current zoom level, centred on your configured position (pink cross). Each vessel is drawn as a hull: scaled to its reported length and beam, rotated to its heading, and coloured by vessel type. A line ahead of it shows where it will be in 6 minutes. Its name and speed appear underneath. Vessels go dim after 3 minutes without a report and disappear after 10 minutes. **+ / −** on the right change the zoom level. The time is at the top edge, the next high or low tide at the bottom, and the vessel count or AIS status on the left. |
| **Vessel detail** | Tap a vessel | Name, MMSI, call sign, AIS class, vessel type, navigational status, course, heading, speed, distance and bearing from your position, size, destination, and when it was last heard. It updates live. Swipe right or tap **Map** to go back. |
| **Network** | Hold one finger anywhere for **10 s**. A countdown ring appears after 2 s. | The Wi-Fi network, IP address and config page URL, plus AIS and tide status. **Reset to AP mode** (tap twice to confirm) switches the display to a setup access point and shows its SSID and password. Saved settings are kept. |

Vessel colours: green cargo, red tanker, blue passenger, orange fishing, magenta sailing or pleasure, cyan tug, yellow pilot / SAR / law enforcement / high-speed craft, grey other or unknown.

## Configuration web page

Open `http://<display-ip>/`. The Network screen shows the address. In setup mode it's `http://192.168.4.1/`, and most phones open the page automatically after you join the access point.

- **AIS data server**: host and TCP port of the NMEA 0183 feed. Timestamped feeds are fine: NMEA 4 TAG blocks, leading ISO or epoch timestamps, and trailing fields are all handled. Unsupported or corrupt sentences are counted and ignored. Decoded message types: 1, 2, 3, 5, 18, 19, 24 and 27.
- **Map**: upload the `map_set.zip` produced by the map builder. The chart's centre becomes the configured position. You can still edit the centre latitude and longitude by hand.
- **Tides & time**:
  - **Tide station**: a NOAA CO-OPS tide station ID. Find one on the [station map](https://tidesandcurrents.noaa.gov/map/).
  - **Time zone**: pick one from the list, or enter a custom POSIX TZ rule.
  - **Display brightness**.
- **Wi-Fi**: the display tests a new network for 20 s before saving it. If it can't join, it reconnects to the previous network, or falls back to the setup access point if that fails too. The page reports the result.

The page also shows live status: Wi-Fi, AIS feed and message counters, vessel count, tide, map, storage and memory. `GET /api/vessels` returns the current vessel table as JSON.

## Building a map

```sh
cd mapbuilder
python3 build_map.py --lat 47.6005 --lon -122.3550 -o seattle
# optional: --radii-nm 0.25,0.5,1,2,4   (one zoom level per radius; default 0.5,1,2,4)
```

This writes `seattle/map_z0.png … map_z3.png`, `manifest.json` and **`seattle/map_set.zip`**. Upload the zip on the config page. The charts come from NOAA's ENC-based Chart Display Service, which draws official NOAA ENC data with standard (S-52) nautical chart symbols. The info, data-quality and overscale overlays are turned off to reduce clutter; `--list-layers` and `--layers` control this. Only US waters are covered. No pip packages are needed.

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

On first boot there's no saved Wi-Fi network, so the display starts in setup mode with an `AIS-Display-XXXX` access point and shows the password on screen. Join it, open `http://192.168.4.1/`, and enter your Wi-Fi network, AIS server, tide station and time zone. Then upload a map.

### Hardware notes and troubleshooting

- **Chip revision.** The ESPHome config ran the CPU at 360 MHz, which only early (pre-v3.0) ESP32-P4 chips use. `sdkconfig.defaults` therefore builds for revisions below v3. If `esptool.py --chip esp32p4 chip_id` reports v3.x, delete the two `ESP32P4_*REV*` lines from `firmware/sdkconfig.defaults`, then run `rm sdkconfig && idf.py build`. A build for the wrong revision won't boot.
- **Touch lands in the wrong place.** Set the swap and mirror options under `idf.py menuconfig` → *AIS Display*. The defaults (all off) come from the Waveshare BSP. Your ESPHome config used `swap_xy` + `mirror_x`, but ESPHome applies its own transform on top, so the two may not match.
- **No Wi-Fi.** Wi-Fi runs on the board's ESP32-C6 co-processor through ESP-Hosted over SDIO. If the log shows ESP-Hosted version or RPC errors, the C6's firmware is too old for the host library. Flash the C6 with the matching ESP-Hosted slave firmware (see the [esp-hosted-mcu docs](https://github.com/espressif/esp-hosted-mcu)).
- **Pins** come from the ESPHome config and the Waveshare BSP. Display: MIPI-DSI, 2 lanes, JD9365 controller, reset on GPIO27. Backlight: GPIO26 PWM, inverted. Touch: GT911 on I2C, GPIO7/8. C6: SDIO CLK 18, CMD 19, D0–D3 14–17, reset 54.

## Testing without a boat

```sh
python3 tools/ais_sim.py --lat 47.6005 --lon -122.3550            # serves TCP port 10110
python3 tools/ais_sim.py --timestamps tag --noise                 # timestamped feed plus junk lines
python3 tools/ais_sim.py --replay capture.nmea --rate 50          # replay a recorded feed
```

Point the display's AIS server at the machine running the simulator.

Host-side unit tests cover the AIS parser, the geo maths, a simulator→parser round trip and the map-upload zip reader:

```sh
firmware/test/host/run_tests.sh
```
