// Vessel detail screen: everything we know about one AIS contact.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"

#include "geo.h"
#include "settings.h"
#include "ui.h"
#include "vessel_db.h"

#define DEG "\xC2\xB0"

enum {
    ROW_MMSI, ROW_CALLSIGN, ROW_CLASS, ROW_TYPE, ROW_STATUS, ROW_COURSE, ROW_HEADING, ROW_SPEED,
    ROW_DISTANCE, ROW_BEARING, ROW_SIZE, ROW_DEST, ROW_UPDATED, ROW_COUNT
};

static const char *const k_row_names[ROW_COUNT] = {
    "MMSI", "Call sign", "AIS class", "Vessel type", "Status", "Course", "Heading", "Speed",
    "Distance", "Bearing", "Size", "Destination", "Last update",
};

static lv_obj_t *s_scr, *s_title, *s_swatch, *s_values[ROW_COUNT];
static uint32_t s_mmsi;
static vessel_t s_last;     // last known state, kept if the contact expires
static bool s_have;

static void back_cb(lv_event_t *e)
{
    ui_show_map();
}

static void gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) ui_show_map();
}

static void set_row(int row, const char *fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(lv_label_get_text(s_values[row]), buf) != 0) lv_label_set_text(s_values[row], buf);
}

static void refresh(void)
{
    vessel_t v;
    bool live = vessel_db_get(s_mmsi, &v);
    if (live) {
        s_last = v;
        s_have = true;
    } else if (s_have) {
        v = s_last;
    } else {
        lv_label_set_text(s_title, "Contact lost");
        return;
    }

    if (v.name[0]) lv_label_set_text(s_title, v.name);
    else lv_label_set_text_fmt(s_title, "MMSI %lu", (unsigned long)v.mmsi);
    bool aton = v.cls == AIS_CLASS_ATON, sar = vessel_is_aircraft(&v);
    lv_obj_set_style_bg_color(s_swatch, aton ? lv_color_hex(0xC0289A) : sar ? lv_color_hex(0xFF8A1F)
                                                                           : ui_ship_color(v.ship_type), 0);

    const char *na = "-";
    set_row(ROW_MMSI, "%lu", (unsigned long)v.mmsi);
    set_row(ROW_CALLSIGN, "%s", v.callsign[0] ? v.callsign : na);
    set_row(ROW_CLASS, "%s", v.cls == AIS_CLASS_UNKNOWN ? na : ais_class_str(v.cls));
    // Marks: what kind, and whether it's a real one on its station. Aircraft: the altitude.
    if (aton) {
        set_row(ROW_TYPE, "%s", ais_aton_type_str(v.aton_type));
        set_row(ROW_STATUS, "%s%s", v.aton_flags & AIS_ATON_VIRTUAL ? "Virtual (AIS only)" : "On station",
                v.aton_flags & AIS_ATON_OFF_POS ? ", #f2545b OFF POSITION#" : "");
    } else if (sar) {
        set_row(ROW_TYPE, v.cls == AIS_CLASS_SAR ? "SAR aircraft" : "Aircraft");
        if (v.fields & AIS_F_ALT) set_row(ROW_STATUS, "Altitude %.0f ft%s", v.alt_m * 3.28084f, v.alt_m >= 4094 ? "+" : "");
        else set_row(ROW_STATUS, "%s", na);
    } else {
        set_row(ROW_TYPE, "%s", (v.fields & AIS_F_SHIPTYPE) ? ais_ship_type_str(v.ship_type) : na);
        set_row(ROW_STATUS, "%s", (v.fields & AIS_F_NAVSTAT) ? ais_nav_status_str(v.nav_status) : na);
    }
    if (v.fields & AIS_F_COG) set_row(ROW_COURSE, "%.1f" DEG, v.cog_deg);
    else set_row(ROW_COURSE, "%s", na);
    if (v.fields & AIS_F_HEADING) set_row(ROW_HEADING, "%u" DEG, v.heading);
    else set_row(ROW_HEADING, "%s", na);
    if (v.fields & AIS_F_SOG) set_row(ROW_SPEED, "%.1f kn", v.sog_kn);
    else set_row(ROW_SPEED, "%s", na);

    if (v.fields & AIS_F_POS) {
        settings_t cfg;
        settings_get(&cfg);
        double d = geo_distance_nm(cfg.center_lat, cfg.center_lon, v.lat, v.lon);
        set_row(ROW_DISTANCE, d < 10 ? "%.2f nm" : "%.1f nm", d);
        set_row(ROW_BEARING, "%.0f" DEG, geo_bearing_deg(cfg.center_lat, cfg.center_lon, v.lat, v.lon));
    } else {
        set_row(ROW_DISTANCE, "no position yet");
        set_row(ROW_BEARING, "%s", na);
    }
    if (v.fields & AIS_F_DIM) set_row(ROW_SIZE, "%u x %u m", v.dim_a + v.dim_b, v.dim_c + v.dim_d);
    else set_row(ROW_SIZE, "%s", na);
    set_row(ROW_DEST, "%s", v.destination[0] ? v.destination : na);

    if (!live) {
        set_row(ROW_UPDATED, "#f2545b contact lost#");
        return;
    }
    int age = (int)((esp_timer_get_time() - v.last_heard_us) / 1000000);
    char when[24] = "";
    if (v.last_heard_wall) {
        when[0] = ' ';
        when[1] = '(';
        ui_format_time(v.last_heard_wall, when + 2, sizeof(when) - 3, true);
        strlcat(when, ")", sizeof(when));
    }
    if (age < 60) set_row(ROW_UPDATED, "%d s ago%s", age, when);
    else set_row(ROW_UPDATED, "%d min %d s ago%s", age / 60, age % 60, when);
}

static void timer_cb(lv_timer_t *t)
{
    if (lv_screen_active() == s_scr) refresh();
}

lv_obj_t *ui_detail_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, UI_COLOR_BG, 0);
    lv_obj_set_scrollable(s_scr, false);
    lv_obj_set_gesture_bubble(s_scr, false);
    lv_obj_add_event_cb(s_scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    s_swatch = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_swatch);
    lv_obj_set_size(s_swatch, 24, 24);
    lv_obj_set_style_radius(s_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_swatch, LV_OPA_COVER, 0);
    lv_obj_align(s_swatch, LV_ALIGN_TOP_MID, 0, 62);

    s_title = lv_label_create(s_scr);
    lv_obj_set_width(s_title, 520);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_title, UI_COLOR_TEXT, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 100);

    const int top = 162, row_h = 34;
    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_t *k = lv_label_create(s_scr);
        lv_label_set_text(k, k_row_names[i]);
        lv_obj_set_width(k, 220);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(k, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(k, UI_COLOR_MUTED, 0);
        lv_obj_set_pos(k, 160, top + i * row_h);

        s_values[i] = lv_label_create(s_scr);
        lv_label_set_text(s_values[i], "");
        lv_label_set_recolor(s_values[i], true);
        lv_obj_set_width(s_values[i], 320);
        lv_label_set_long_mode(s_values[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(s_values[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_values[i], UI_COLOR_TEXT, 0);
        lv_obj_set_pos(s_values[i], 400, top + i * row_h);
    }

    lv_obj_t *back = ui_round_button(s_scr, LV_SYMBOL_LEFT " Map", 160, 64);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_timer_create(timer_cb, 1000, NULL);
    return s_scr;
}

void ui_detail_set_vessel(uint32_t mmsi)
{
    s_mmsi = mmsi;
    s_have = false;
    for (int i = 0; i < ROW_COUNT; i++) lv_label_set_text(s_values[i], "");
    refresh();
}
