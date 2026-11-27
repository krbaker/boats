// What differs between the boards (BOARDS_PLAN.md) below the UI: the microSD
// card and the USB host port. The boards with a screen use the Waveshare BSP
// (board_display.c); the screenless ones have no BSP (board_headless.c).

#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Mounts the microSD card at /sdcard (FAT). It stays mounted: it shares the
// SDMMC host with the Wi-Fi co-processor.
esp_err_t board_sdcard_mount(void);

// Starts the USB host on the USB-A port, for the RTL-SDR dongle.
esp_err_t board_usb_host_start(void);

// True if the user asked for the setup access point by powering the board on
// three times, each less than 5 s after the last: a screenless board's
// "Reset to AP mode". Always false on the boards with a screen, which have the
// Network screen's button. Call once, after settings_init() (it uses NVS).
bool board_ap_requested(void);
