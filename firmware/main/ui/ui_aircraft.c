// Aircraft from dump1090 (ADS-B): the list (from the status bubble on the
// map, nearest first) and the detail screen for one aircraft.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "adsb.h"
#include "geo.h"
#include "settings.h"
#include "ui.h"

#define DEG         "\xC2\xB0"
#define BOX_W       540     // the list; its corners just fit inside the round screen
#define BOX_H       480
#define BOX_Y       40      // offset of the list below the screen centre
#define COL_NAME_W  170
#define COL_DIST_W  120
#define COL_ALT_W   130
#define COL_SPEED_W (BOX_W - COL_NAME_W - COL_DIST_W - COL_ALT_W)
#define POLL_MS     1000

// Call sign, else registration, else the address.
static void aircraft_name(const adsb_aircraft_t *a, char *buf, size_t len)
{
    if (a->flight[0]) snprintf(buf, len, "%s", a->flight);
    else if (a->reg[0]) snprintf(buf, len, "%s", a->reg);
    else snprintf(buf, len, "%06lX", (unsigned long)a->addr);
}

static void altitude_str(const adsb_aircraft_t *a, char *buf, size_t len)
{
    if (a->fields & ADSB_F_GROUND) snprintf(buf, len, "ground");
    else if (a->fields & ADSB_F_ALT) snprintf(buf, len, "%ld ft", (long)a->alt_ft);
    else snprintf(buf, len, "-");
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

static lv_obj_t *s_root, *s_sub, *s_table, *s_empty;
static adsb_target_t *s_list;
static int s_nlist;
static uint32_t s_list_gen;
static bool s_emerg[ADSB_MAX + 1];     // by table row: drawn red

static void refresh_list(void)
{
    s_list_gen = adsb_generation();
    s_nlist = adsb_snapshot(s_list, ADSB_MAX);
    settings_t cfg;
    settings_get(&cfg);
    adsb_status_t st;
    adsb_get_status(&st);

    if (st.state == ADSB_ERROR) lv_label_set_text_fmt(s_sub, "#f2545b dump1090: %s#", st.error);
    else lv_label_set_text_fmt(s_sub, "Within %u nm: %d of %d heard", cfg.adsb_range_nm, s_nlist, st.heard);

    lv_table_set_row_count(s_table, s_nlist + 1);
    for (int r = 0; r < s_nlist; r++) {
        const adsb_aircraft_t *a = &s_list[r].a;
        char buf[24];
        aircraft_name(a, buf, sizeof(buf));
        lv_table_set_cell_value(s_table, r + 1, 0, buf);
        float d = s_list[r].dist_nm;
        lv_table_set_cell_value_fmt(s_table, r + 1, 1, d < 10 ? "%.2f nm" : "%.1f nm", d);
        altitude_str(a, buf, sizeof(buf));
        lv_table_set_cell_value(s_table, r + 1, 2, buf);
        if (a->fields & ADSB_F_GS) lv_table_set_cell_value_fmt(s_table, r + 1, 3, "%.0f kn", a->gs_kn);
        else lv_table_set_cell_value(s_table, r + 1, 3, "-");
        s_emerg[r + 1] = adsb_is_emergency(a);
    }
    lv_obj_set_hidden(s_empty, s_nlist > 0);
    if (!s_nlist) {
        lv_label_set_text(s_empty, st.state == ADSB_OK ? "No aircraft in range" : st.state == ADSB_ERROR
                                                                                     ? "Can't reach dump1090"
                                                                                     : "Waiting for dump1090...");
    }
}

static void table_cb(lv_event_t *e)
{
    uint32_t row, col;
    lv_table_get_selected_cell(s_table, &row, &col);
    if (row == LV_TABLE_CELL_NONE || row == 0 || row > (uint32_t)s_nlist) return;
    ui_show_aircraft(s_list[row - 1].id);
}

// Header row muted on the panel colour; emergencies red.
static void table_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (base->part != LV_PART_ITEMS) return;
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(t);
    if (base->id1 == 0) {
        lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
        if (fill) {
            fill->color = UI_COLOR_PANEL;
            fill->opa = LV_OPA_COVER;
        }
        if (label) label->color = UI_COLOR_MUTED;
    } else if (label && base->id1 <= ADSB_MAX && s_emerg[base->id1]) {
        label->color = UI_COLOR_BAD;
    }
}

static void poll_timer_cb(lv_timer_t *t)
{
    if (lv_obj_is_hidden(s_root)) return;
    if (adsb_generation() != s_list_gen) refresh_list();
}

static void close_cb(lv_event_t *e)
{
    lv_obj_set_hidden(s_root, true);
}

lv_obj_t *ui_aircraft_create(lv_obj_t *parent)
{
    s_list = heap_caps_malloc(ADSB_MAX * sizeof(adsb_target_t), MALLOC_CAP_SPIRAM);
    s_root = ui_overlay_create(parent, close_cb);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "Aircraft");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 124);

    s_sub = lv_label_create(s_root);
    lv_label_set_recolor(s_sub, true);
    lv_obj_set_width(s_sub, BOX_W);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_sub, UI_COLOR_MUTED, 0);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 162);

    s_table = lv_table_create(s_root);
    lv_obj_set_size(s_table, BOX_W, BOX_H);
    lv_obj_align(s_table, LV_ALIGN_CENTER, 0, BOX_Y);
    lv_table_set_column_count(s_table, 4);
    lv_table_set_column_width(s_table, 0, COL_NAME_W);
    lv_table_set_column_width(s_table, 1, COL_DIST_W);
    lv_table_set_column_width(s_table, 2, COL_ALT_W);
    lv_table_set_column_width(s_table, 3, COL_SPEED_W);
    lv_table_set_cell_value(s_table, 0, 0, "Call sign");
    lv_table_set_cell_value(s_table, 0, 1, "Distance");
    lv_table_set_cell_value(s_table, 0, 2, "Altitude");
    lv_table_set_cell_value(s_table, 0, 3, "Speed");
    lv_obj_set_style_bg_opa(s_table, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_table, 0, 0);
    lv_obj_set_style_bg_opa(s_table, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_table, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_table, UI_COLOR_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_border_side(s_table, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(s_table, 12, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(s_table, 14, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_table, table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_set_send_draw_task_events(s_table, true);
    lv_obj_add_event_cb(s_table, table_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_empty = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_empty, UI_COLOR_MUTED, 0);
    lv_obj_center(s_empty);

    lv_timer_create(poll_timer_cb, POLL_MS, NULL);
    return s_root;
}

void ui_aircraft_open(void)
{
    refresh_list();
    lv_obj_scroll_to_y(s_table, 0, LV_ANIM_OFF);
    lv_obj_set_hidden(s_root, false);
}

// ---------------------------------------------------------------------------
// One aircraft
// ---------------------------------------------------------------------------

enum {
    ROW_ADDR, ROW_REG, ROW_TYPE, ROW_CATEGORY, ROW_SQUAWK, ROW_ALT, ROW_VRATE, ROW_SPEED, ROW_TRACK,
    ROW_DISTANCE, ROW_BEARING, ROW_UPDATED, ROW_COUNT
};

static const char *const k_row_names[ROW_COUNT] = {
    "ICAO address", "Registration", "Aircraft type", "Category", "Squawk", "Altitude", "Climb / descent",
    "Ground speed", "Track", "Distance", "Bearing", "Last position",
};

static lv_obj_t *s_scr, *s_title, *s_swatch, *s_values[ROW_COUNT];
static uint32_t s_id;
static adsb_target_t s_last;    // last known state, kept once it's out of range or lost
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
    char buf[80];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (strcmp(lv_label_get_text(s_values[row]), buf) != 0) lv_label_set_text(s_values[row], buf);
}

static void refresh_detail(void)
{
    adsb_target_t t;
    bool live = adsb_get(s_id, &t);
    if (live) {
        s_last = t;
        s_have = true;
    } else if (s_have) {
        t = s_last;
    } else {
        lv_label_set_text(s_title, "Aircraft lost");
        return;
    }
    const adsb_aircraft_t *a = &t.a;
    char buf[24];
    aircraft_name(a, buf, sizeof(buf));
    if (strcmp(lv_label_get_text(s_title), buf) != 0) lv_label_set_text(s_title, buf);
    bool emerg = adsb_is_emergency(a);
    lv_obj_set_style_bg_color(s_swatch, emerg ? lv_color_hex(0xE0242B) : lv_color_hex(0x6D8BB0), 0);

    const char *na = "-";
    set_row(ROW_ADDR, "%06lX%s", (unsigned long)a->addr, a->non_icao ? " (not ICAO)" : "");
    set_row(ROW_REG, "%s", a->reg[0] ? a->reg : na);
    set_row(ROW_TYPE, "%s", a->type[0] ? a->type : na);
    const char *cat = adsb_category_str(a->category);
    set_row(ROW_CATEGORY, "%s", cat[0] ? cat : na);
    const char *what = adsb_emergency_str(a);
    if (a->fields & ADSB_F_SQUAWK) {
        set_row(ROW_SQUAWK, "%04u%s%s%s%s", a->squawk, what ? "  " : "", what && emerg ? "#f2545b " : "", what ? what : "",
                what && emerg ? "#" : "");
    } else if (what) {
        set_row(ROW_SQUAWK, emerg ? "#f2545b %s#" : "%s", what);
    } else {
        set_row(ROW_SQUAWK, "%s", na);
    }
    altitude_str(a, buf, sizeof(buf));
    set_row(ROW_ALT, "%s", strcmp(buf, "ground") == 0 ? "On the ground" : buf);
    if (!(a->fields & ADSB_F_VRATE)) set_row(ROW_VRATE, "%s", na);
    else if (abs(a->vrate_fpm) < 100) set_row(ROW_VRATE, "level");
    else set_row(ROW_VRATE, "%s %d ft/min", a->vrate_fpm > 0 ? "climbing" : "descending", abs(a->vrate_fpm));
    if (a->fields & ADSB_F_GS) set_row(ROW_SPEED, "%.0f kn", a->gs_kn);
    else set_row(ROW_SPEED, "%s", na);
    if (a->fields & ADSB_F_TRACK) set_row(ROW_TRACK, "%.0f" DEG, a->track_deg);
    else set_row(ROW_TRACK, "%s", na);

    settings_t cfg;
    settings_get(&cfg);
    double d = geo_distance_nm(cfg.center_lat, cfg.center_lon, a->lat, a->lon);
    set_row(ROW_DISTANCE, d < 10 ? "%.2f nm" : "%.1f nm", d);
    set_row(ROW_BEARING, "%.0f" DEG, geo_bearing_deg(cfg.center_lat, cfg.center_lon, a->lat, a->lon));

    if (!live) {
        set_row(ROW_UPDATED, "#f2545b out of range or lost#");
        return;
    }
    int age = (int)((esp_timer_get_time() - t.pos_us) / 1000000);
    if (age < 60) set_row(ROW_UPDATED, "%d s ago", age);
    else set_row(ROW_UPDATED, "%d min %d s ago", age / 60, age % 60);
}

static void detail_timer_cb(lv_timer_t *t)
{
    if (lv_screen_active() == s_scr) refresh_detail();
}

lv_obj_t *ui_aircraft_detail_create(void)
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

    lv_timer_create(detail_timer_cb, 1000, NULL);
    return s_scr;
}

void ui_aircraft_detail_set(uint32_t id)
{
    s_id = id;
    s_have = false;
    for (int i = 0; i < ROW_COUNT; i++) lv_label_set_text(s_values[i], "");
    refresh_detail();
}
