// Vessels overlay (tap the vessel count on the map): a list of all traffic,
// nearest first, how the two receivers (local RTL-SDR and network feed)
// compare, and the map's vessel filter / label options.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "geo.h"
#include "nmea_client.h"
#include "rx_stats.h"
#include "sdr_probe.h"
#include "settings.h"
#include "ui.h"
#include "vessel_db.h"

#define BOX_W       540     // content box; its corners just fit inside the round screen
#define BOX_H       560
#define BOX_Y       24      // offset of the box below the screen centre
#define COL_NAME_W  250
#define COL_NUM_W   ((BOX_W - COL_NAME_W) / 2)
#define POLL_MS     250
#define RX_POLL_MS  1000
enum { TAB_LIST, TAB_RX, TAB_OPTIONS };

typedef struct {
    uint32_t mmsi;
    int idx;        // into s_vessels
    float dist_nm;
} row_t;

static lv_obj_t *s_root, *s_tabs, *s_table, *s_empty;
static lv_obj_t *s_rx_status, *s_rx_table, *s_rx_vessels, *s_type_table;
static bool s_type_used[29];    // by row of s_type_table: the display uses that message type
static lv_obj_t *s_len_lbl, *s_len_slider, *s_speed_lbl, *s_speed_slider;
static lv_obj_t *s_unknown_cb, *s_mode_dd, *s_order_dd, *s_name_cb, *s_speed_cb;
static vessel_t *s_vessels;
static row_t *s_rows;
static int s_nrows;
static uint32_t s_generation;
static vessel_filter_t s_filter;
static bool s_filter_dirty;

// ---------------------------------------------------------------------------
// Vessel list
// ---------------------------------------------------------------------------

static int row_cmp(const void *a, const void *b)
{
    float da = ((const row_t *)a)->dist_nm, db = ((const row_t *)b)->dist_nm;
    return (da > db) - (da < db);
}

static void refresh_list(void)
{
    s_generation = vessel_db_generation();
    int n = vessel_db_snapshot(s_vessels, VESSEL_MAX);
    settings_t cfg;
    settings_get(&cfg);

    // Sort indices by distance; vessels without a position go last.
    s_nrows = 0;
    for (int i = 0; i < n; i++) {
        const vessel_t *v = &s_vessels[i];
        if (v->cls == AIS_CLASS_ATON) continue; // marks are on the map only
        float d = (v->fields & AIS_F_POS) ? geo_distance_nm(cfg.center_lat, cfg.center_lon, v->lat, v->lon) : INFINITY;
        s_rows[s_nrows++] = (row_t){.mmsi = v->mmsi, .idx = i, .dist_nm = d};
    }
    qsort(s_rows, s_nrows, sizeof(s_rows[0]), row_cmp);

    lv_table_set_row_count(s_table, s_nrows + 1);
    for (int r = 0; r < s_nrows; r++) {
        const vessel_t *v = &s_vessels[s_rows[r].idx];
        char buf[32];
        if (v->name[0]) lv_table_set_cell_value(s_table, r + 1, 0, v->name);
        else lv_table_set_cell_value_fmt(s_table, r + 1, 0, "%lu", (unsigned long)v->mmsi);
        float d = s_rows[r].dist_nm;
        if (isinf(d)) strcpy(buf, "-");
        else snprintf(buf, sizeof(buf), d < 10 ? "%.2f nm" : "%.1f nm", d);
        lv_table_set_cell_value(s_table, r + 1, 1, buf);
        if (v->fields & AIS_F_SOG) snprintf(buf, sizeof(buf), "%.1f kn", v->sog_kn);
        else strcpy(buf, "-");
        lv_table_set_cell_value(s_table, r + 1, 2, buf);
    }
    lv_obj_set_hidden(s_empty, s_nrows > 0);
}

static void table_cb(lv_event_t *e)
{
    uint32_t row, col;
    lv_table_get_selected_cell(s_table, &row, &col);
    if (row == LV_TABLE_CELL_NONE || row == 0 || row > (uint32_t)s_nrows) return;
    ui_show_detail(s_rows[row - 1].mmsi);
}

// Header row: bold-ish, not selectable.
static void table_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (base->part != LV_PART_ITEMS || base->id1 != 0) return;
    lv_draw_fill_dsc_t *fill = lv_draw_task_get_fill_dsc(t);
    if (fill) {
        fill->color = UI_COLOR_PANEL;
        fill->opa = LV_OPA_COVER;
    }
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(t);
    if (label) label->color = UI_COLOR_MUTED;
}

static void poll_timer_cb(lv_timer_t *t)
{
    if (lv_obj_is_hidden(s_root) || lv_tabview_get_tab_active(s_tabs) != TAB_LIST) return;
    if (vessel_db_generation() != s_generation) refresh_list();
}

// ---------------------------------------------------------------------------
// Receivers: local RTL-SDR against the network feed
// ---------------------------------------------------------------------------

static void refresh_rx(void)
{
    sdr_status_t r;
    sdr_get_status(&r);
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "RTL-SDR: %s%s%s", r.tuner, r.tuner[0] ? ", " : "", sdr_state_str(r.state));
    if (r.state == SDR_READY || r.state == SDR_CAPTURING) {
        n += snprintf(buf + n, sizeof(buf) - n, "\nDecoder: %.0f%% of one CPU core", r.cpu_pct);
        if (r.dropped_bytes) n += snprintf(buf + n, sizeof(buf) - n, ", %.1f s of samples dropped", r.dropped_bytes / (2.0 * r.rate));
        if (r.sweep_step) {
            n += snprintf(buf + n, sizeof(buf) - n, "\nChoosing the gain (%d/%d)...", r.sweep_step, r.sweep_steps);
        } else if (r.gain == SDR_GAIN_AUTO) {
            n += snprintf(buf + n, sizeof(buf) - n, "\nGain: tuner AGC");
        } else {
            n += snprintf(buf + n, sizeof(buf) - n, "\nGain: %.1f dB%s", r.gain / 10.0, r.gain_best ? " (auto)" : "");
            if (r.gain_steps_down) n += snprintf(buf + n, sizeof(buf) - n, ", lowered for a strong signal");
        }
        snprintf(buf + n, sizeof(buf) - n, "\nNoise %.1f dBFS, tuning %+.1f ppm", (r.demod.noise_dbfs[0] + r.demod.noise_dbfs[1]) / 2,
                 r.demod.ppm);
    }
    lv_label_set_text(s_rx_status, buf);

    rx_stats_t x;
    rx_stats_get(&x);
    for (int src = 0; src < RX_NSRC; src++) {
        int col = src == RX_SDR ? 1 : 2;
        lv_table_set_cell_value_fmt(s_rx_table, 1, col, "%lu", (unsigned long)x.last_min[src][0]);
        lv_table_set_cell_value_fmt(s_rx_table, 2, col, "%lu", (unsigned long)x.last_min[src][1]);
        lv_table_set_cell_value_fmt(s_rx_table, 3, col, "%lu", (unsigned long)(x.total[src][0] + x.total[src][1]));
    }
    int net_only, sdr_only, both;
    vessel_db_source_counts(&net_only, &sdr_only, &both);
    lv_table_set_cell_value_fmt(s_rx_table, 4, 1, "%d", sdr_only);
    lv_table_set_cell_value_fmt(s_rx_table, 4, 2, "%d", net_only);
    lv_label_set_text_fmt(s_rx_vessels, "Vessels heard by both: %d", both);

    // Every message type heard since start-up, from each receiver.
    static const char *const names[28] = {
        "Too short", "Position A", "Position A", "Position A", "Base station", "Static A", "Binary addr.",
        "Binary ack", "Binary bcast", "SAR aircraft", "UTC inquiry", "UTC response", "Safety addr.", "Safety ack",
        "Safety bcast", "Interrogation", "Assign mode", "DGNSS", "Position B", "Ext. pos. B", "Data link",
        "Aid to nav.", "Channel mgmt", "Group assign", "Static B", "Binary 1-slot", "Binary multi", "Long range",
    };
    nmea_status_t ns;
    nmea_client_get_status(&ns);
    int row = 1;
    for (int t = 0; t < 28; t++) {
        uint32_t sdr = r.parser.by_type[t], net = ns.stats.by_type[t];
        if (!sdr && !net) continue;
        lv_table_set_row_count(s_type_table, row + 1);
        lv_table_set_cell_value_fmt(s_type_table, row, 0, "%d %s", t, names[t]);
        lv_table_set_cell_value_fmt(s_type_table, row, 1, "%lu", (unsigned long)sdr);
        lv_table_set_cell_value_fmt(s_type_table, row, 2, "%lu", (unsigned long)net);
        s_type_used[row++] = ais_msg_type_used(t);
    }
    lv_table_set_row_count(s_type_table, row);
}

// Types the display doesn't use, muted (after the header row's styling).
static void type_table_draw_cb(lv_event_t *e)
{
    lv_draw_task_t *t = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base = lv_draw_task_get_draw_dsc(t);
    if (base->part != LV_PART_ITEMS || base->id1 == 0 || base->id1 >= 29 || s_type_used[base->id1]) return;
    lv_draw_label_dsc_t *label = lv_draw_task_get_label_dsc(t);
    if (label) label->color = UI_COLOR_MUTED;
}

static void rx_timer_cb(lv_timer_t *t)
{
    if (lv_obj_is_hidden(s_root) || lv_tabview_get_tab_active(s_tabs) != TAB_RX) return;
    refresh_rx();
}

static void style_table(lv_obj_t *t)
{
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(t, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_color(t, UI_COLOR_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(t, 12, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(t, 14, LV_PART_ITEMS);
    lv_obj_add_event_cb(t, table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_set_send_draw_task_events(t, true);
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, lv_color_t color);

static void build_rx(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(tab, 28, 0);
    lv_obj_set_style_pad_ver(tab, 16, 0);
    lv_obj_set_style_pad_row(tab, 12, 0);

    s_rx_status = text(tab, "", UI_COLOR_TEXT);
    lv_obj_set_width(s_rx_status, lv_pct(100));
    lv_label_set_long_mode(s_rx_status, LV_LABEL_LONG_WRAP);

    s_rx_table = lv_table_create(tab);
    lv_table_set_column_count(s_rx_table, 3);
    lv_table_set_column_width(s_rx_table, 0, 210);
    lv_table_set_column_width(s_rx_table, 1, 130);
    lv_table_set_column_width(s_rx_table, 2, 130);
    static const char *const rows[] = {"Packets", "Ch A, last min", "Ch B, last min", "Since boot", "Vessels only here"};
    for (int i = 0; i < 5; i++) lv_table_set_cell_value(s_rx_table, i, 0, rows[i]);
    lv_table_set_cell_value(s_rx_table, 0, 1, "SDR");
    lv_table_set_cell_value(s_rx_table, 0, 2, "Network");
    style_table(s_rx_table);

    s_rx_vessels = text(tab, "", UI_COLOR_MUTED);

    // Message types, below (the tab scrolls when there are many).
    s_type_table = lv_table_create(tab);
    lv_table_set_column_count(s_type_table, 3);
    lv_table_set_column_width(s_type_table, 0, 210);
    lv_table_set_column_width(s_type_table, 1, 130);
    lv_table_set_column_width(s_type_table, 2, 130);
    lv_table_set_cell_value(s_type_table, 0, 0, "Message types");
    lv_table_set_cell_value(s_type_table, 0, 1, "SDR");
    lv_table_set_cell_value(s_type_table, 0, 2, "Network");
    style_table(s_type_table);
    lv_obj_add_event_cb(s_type_table, type_table_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

static void show_filter_values(void)
{
    if (s_filter.min_len_ft) lv_label_set_text_fmt(s_len_lbl, "Min length: %u ft", s_filter.min_len_ft);
    else lv_label_set_text(s_len_lbl, "Min length: off");
    if (s_filter.min_speed_kn) lv_label_set_text_fmt(s_speed_lbl, "Min speed: %u kn", s_filter.min_speed_kn);
    else lv_label_set_text(s_speed_lbl, "Min speed: off");
}

static void option_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    if (obj == s_len_slider) {
        s_filter.min_len_ft = (uint16_t)((lv_slider_get_value(obj) + 5) / 10 * 10);
    } else if (obj == s_speed_slider) {
        s_filter.min_speed_kn = (uint8_t)lv_slider_get_value(obj);
    } else if (obj == s_unknown_cb) {
        s_filter.include_unknown = lv_obj_has_state(obj, LV_STATE_CHECKED);
    } else if (obj == s_mode_dd) {
        s_filter.mode = lv_dropdown_get_selected(obj) ? FILTER_HIDE_VESSEL : FILTER_HIDE_LABEL;
    } else if (obj == s_order_dd) {
        s_filter.order = lv_dropdown_get_selected(obj) ? ORDER_SIZE : ORDER_SPEED;
    } else if (obj == s_name_cb) {
        s_filter.label_name = lv_obj_has_state(obj, LV_STATE_CHECKED);
    } else if (obj == s_speed_cb) {
        s_filter.label_speed = lv_obj_has_state(obj, LV_STATE_CHECKED);
    }
    s_filter_dirty = true;
    show_filter_values();
    ui_map_set_filter(&s_filter);
}

static lv_obj_t *text(lv_obj_t *parent, const char *t, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *slider(lv_obj_t *parent, int max)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_slider_set_range(s, 0, max);
    lv_obj_set_width(s, lv_pct(90));
    lv_obj_set_height(s, 16);
    lv_obj_set_style_pad_ver(s, 12, LV_PART_MAIN); // bigger touch target
    lv_obj_set_style_margin_bottom(s, 8, 0);
    lv_obj_add_event_cb(s, option_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

static lv_obj_t *checkbox(lv_obj_t *parent, const char *t)
{
    lv_obj_t *c = lv_checkbox_create(parent);
    lv_checkbox_set_text(c, t);
    lv_obj_set_style_text_color(c, UI_COLOR_TEXT, 0);
    lv_obj_add_event_cb(c, option_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return c;
}

static lv_obj_t *dropdown_row(lv_obj_t *parent, const char *label, const char *options)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    text(row, label, UI_COLOR_TEXT);
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, options);
    lv_obj_set_width(dd, 200);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_t *parts[] = {dd, list};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(parts[i], UI_COLOR_PANEL, 0);
        lv_obj_set_style_text_color(parts[i], UI_COLOR_TEXT, 0);
        lv_obj_set_style_border_color(parts[i], UI_COLOR_MUTED, 0);
    }
    lv_obj_set_style_bg_color(list, UI_COLOR_ACCENT, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_add_event_cb(dd, option_cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static void set_checked(lv_obj_t *obj, bool on)
{
    lv_obj_set_state(obj, LV_STATE_CHECKED, on);
}

static void build_options(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(tab, 28, 0);
    lv_obj_set_style_pad_ver(tab, 16, 0);
    lv_obj_set_style_pad_row(tab, 10, 0);

    text(tab, "Hide vessels that are smaller or slower than:", UI_COLOR_MUTED);
    s_len_lbl = text(tab, "", UI_COLOR_TEXT);
    s_len_slider = slider(tab, 300);
    s_speed_lbl = text(tab, "", UI_COLOR_TEXT);
    s_speed_slider = slider(tab, 50);
    s_unknown_cb = checkbox(tab, "Keep vessels that report no size / speed");
    s_mode_dd = dropdown_row(tab, "Hidden vessels", "Show, no label\nDon't draw");
    s_order_dd = dropdown_row(tab, "Label priority", "Fastest\nLargest");
    text(tab, "Vessel labels", UI_COLOR_MUTED);
    s_name_cb = checkbox(tab, "Name");
    s_speed_cb = checkbox(tab, "Speed");
}

static void load_options(void)
{
    settings_t cfg;
    settings_get(&cfg);
    s_filter = cfg.filter;
    s_filter_dirty = false;
    lv_slider_set_value(s_len_slider, s_filter.min_len_ft, LV_ANIM_OFF);
    lv_slider_set_value(s_speed_slider, s_filter.min_speed_kn, LV_ANIM_OFF);
    set_checked(s_unknown_cb, s_filter.include_unknown);
    lv_dropdown_set_selected(s_mode_dd, s_filter.mode == FILTER_HIDE_VESSEL);
    lv_dropdown_set_selected(s_order_dd, s_filter.order == ORDER_SIZE);
    set_checked(s_name_cb, s_filter.label_name);
    set_checked(s_speed_cb, s_filter.label_speed);
    show_filter_values();
}

// ---------------------------------------------------------------------------

static void close_cb(lv_event_t *e)
{
    if (s_filter_dirty) {
        // Saved once on close rather than on every slider step, to spare the flash.
        settings_t cfg;
        settings_get(&cfg);
        cfg.filter = s_filter;
        settings_set(&cfg);
        s_filter_dirty = false;
    }
    lv_obj_set_hidden(s_root, true);
}

static void tab_changed_cb(lv_event_t *e)
{
    uint32_t tab = lv_tabview_get_tab_active(s_tabs);
    if (tab == TAB_LIST) refresh_list();
    else if (tab == TAB_RX) refresh_rx();
}

lv_obj_t *ui_vessels_create(lv_obj_t *parent)
{
    s_vessels = heap_caps_malloc(VESSEL_MAX * sizeof(vessel_t), MALLOC_CAP_SPIRAM);
    s_rows = heap_caps_malloc(VESSEL_MAX * sizeof(row_t), MALLOC_CAP_SPIRAM);

    s_root = ui_overlay_create(parent, close_cb);

    s_tabs = lv_tabview_create(s_root);
    lv_obj_set_size(s_tabs, BOX_W, BOX_H);
    lv_obj_align(s_tabs, LV_ALIGN_CENTER, 0, BOX_Y);
    lv_obj_set_style_bg_color(s_tabs, UI_COLOR_BG, 0);
    lv_obj_set_style_radius(s_tabs, 24, 0);
    lv_obj_set_style_clip_corner(s_tabs, true, 0);
    lv_tabview_set_tab_bar_size(s_tabs, 56);
    // Tabs switch from the tab bar only, so horizontal drags on the sliders don't swipe tabs.
    lv_obj_set_scrollable(lv_tabview_get_content(s_tabs), false);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabs);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(s_tabs, tab_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *list_tab = lv_tabview_add_tab(s_tabs, "Vessels");
    lv_obj_t *rx_tab = lv_tabview_add_tab(s_tabs, "Receiver");
    lv_obj_t *opt_tab = lv_tabview_add_tab(s_tabs, "Options");
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_set_style_text_color(lv_obj_get_child(bar, i), UI_COLOR_TEXT, 0);
    }

    lv_obj_set_style_pad_all(list_tab, 0, 0);
    s_table = lv_table_create(list_tab);
    lv_obj_set_size(s_table, BOX_W, lv_pct(100));
    lv_table_set_column_count(s_table, 3);
    lv_table_set_column_width(s_table, 0, COL_NAME_W);
    lv_table_set_column_width(s_table, 1, COL_NUM_W);
    lv_table_set_column_width(s_table, 2, COL_NUM_W);
    lv_table_set_cell_value(s_table, 0, 0, "Name / MMSI");
    lv_table_set_cell_value(s_table, 0, 1, "Distance");
    lv_table_set_cell_value(s_table, 0, 2, "Speed");
    style_table(s_table);
    lv_obj_add_event_cb(s_table, table_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_empty = text(list_tab, "No vessels heard yet", UI_COLOR_MUTED);
    lv_obj_center(s_empty);

    build_rx(rx_tab);
    build_options(opt_tab);

    lv_timer_create(poll_timer_cb, POLL_MS, NULL);
    lv_timer_create(rx_timer_cb, RX_POLL_MS, NULL);
    return s_root;
}

void ui_vessels_open(void)
{
    load_options();
    lv_tabview_set_active(s_tabs, TAB_LIST, LV_ANIM_OFF);
    refresh_list();
    lv_obj_scroll_to_y(s_table, 0, LV_ANIM_OFF);
    lv_obj_set_hidden(s_root, false);
}
