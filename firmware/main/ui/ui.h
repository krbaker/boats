// Display UI: map, vessel detail and network screens.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "lvgl.h"
#include "settings.h"

#define UI_SIZE     800
#define UI_CENTER   (UI_SIZE / 2)

// Palette
#define UI_COLOR_BG         lv_color_hex(0x0d1b2a)
#define UI_COLOR_PANEL      lv_color_hex(0x1b2b3f)
#define UI_COLOR_TEXT       lv_color_hex(0xe8eef5)
#define UI_COLOR_MUTED      lv_color_hex(0x8fa3b8)
#define UI_COLOR_ACCENT     lv_color_hex(0x3fa7f5)
#define UI_COLOR_OK         lv_color_hex(0x3ccf7a)
#define UI_COLOR_BAD        lv_color_hex(0xf2545b)
#define UI_COLOR_WARN       lv_color_hex(0xf5b13f)

// Start the UI (call once the display is running).
void ui_start(void);

// Firmware update: show a static "Updating firmware" screen and stop drawing,
// so the flash writes can't make the display flicker. ui_updating_end() goes
// back to the UI if the update fails. Call from any task except LVGL's.
void ui_updating_begin(void);
void ui_updating_end(void);

// Screen navigation (call from the LVGL task, i.e. event handlers / timers).
void ui_show_map(void);
void ui_show_detail(uint32_t mmsi);
void ui_show_netinfo(void);

// Screens
lv_obj_t *ui_map_create(void);
void ui_map_on_show(void);
void ui_map_set_filter(const vessel_filter_t *f);   // apply live (not saved)
lv_obj_t *ui_detail_create(void);
void ui_detail_set_vessel(uint32_t mmsi);
lv_obj_t *ui_netinfo_create(void);
void ui_netinfo_on_show(void);

// Overlays on the map screen
lv_obj_t *ui_vessels_create(lv_obj_t *parent);
void ui_vessels_open(void);
lv_obj_t *ui_tide_create(lv_obj_t *parent);
void ui_tide_open(void);
lv_obj_t *ui_weather_create(lv_obj_t *parent);
void ui_weather_open(void);

// Shared widgets
lv_obj_t *ui_round_button(lv_obj_t *parent, const char *text, int32_t w, int32_t h);
// Hidden full-screen round panel with a close button at the top.
lv_obj_t *ui_overlay_create(lv_obj_t *parent, lv_event_cb_t close_cb);
lv_color_t ui_ship_color(uint8_t ship_type);
void ui_format_time(time_t t, char *buf, size_t len, bool seconds);
