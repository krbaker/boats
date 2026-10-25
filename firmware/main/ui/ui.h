// Display UI: map, vessel and aircraft detail, and network screens.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "lvgl.h"
#include "settings.h"
#include "vessel_db.h"

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
void ui_show_aircraft(uint32_t id);    // an ADS-B aircraft, by adsb_id()

// Screens
lv_obj_t *ui_map_create(void);
void ui_map_on_show(void);
void ui_map_set_filter(const vessel_filter_t *f);   // apply live (not saved)
lv_obj_t *ui_detail_create(void);
void ui_detail_set_vessel(uint32_t mmsi);
lv_obj_t *ui_netinfo_create(void);
void ui_netinfo_on_show(void);
lv_obj_t *ui_aircraft_detail_create(void);
void ui_aircraft_detail_set(uint32_t id);

// Overlays on the map screen
lv_obj_t *ui_vessels_create(lv_obj_t *parent);
void ui_vessels_open(void);
lv_obj_t *ui_tide_create(lv_obj_t *parent);
void ui_tide_open(void);
lv_obj_t *ui_weather_create(lv_obj_t *parent);
void ui_weather_open(void);
lv_obj_t *ui_aircraft_create(lv_obj_t *parent);    // the ADS-B aircraft list
void ui_aircraft_open(void);

// AIS safety broadcasts (ui_safety.c): popups over any screen, and helpers
// for listing them.
void ui_safety_init(void);
uint32_t ui_safety_msg_id(const safety_msg_t *m);
void ui_safety_sender(const safety_msg_t *m, char *buf, size_t len);   // name/MMSI, distance and bearing
const char *ui_safety_sender_kind(uint32_t mmsi);                      // "AIS-SART", ... or NULL

// A weather station heard over AIS (ui_met.c): its readings, in a popup.
#include "met_db.h"
void ui_met_open(const met_station_t *st);

// The settings tabs of the Network screen (ui_settings.c)
void ui_settings_build(lv_obj_t *tabs);
void ui_settings_refresh(void);

// Shared widgets
lv_obj_t *ui_round_button(lv_obj_t *parent, const char *text, int32_t w, int32_t h);
// Hidden full-screen round panel with a close button at the top.
lv_obj_t *ui_overlay_create(lv_obj_t *parent, lv_event_cb_t close_cb);
lv_color_t ui_ship_color(uint8_t ship_type);
void ui_format_time(time_t t, char *buf, size_t len, bool seconds);
