// The settings tabs of the Network screen: the web page's switches, choices
// and numbers (its text fields and uploads stay on the web page). Every change
// is saved as it's made.

#include <stdio.h>
#include <string.h>

#include "ais_fwd.h"
#include "bsp/esp-bsp.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdr_probe.h"
#include "settings.h"
#include "ui.h"
#include "vessel_db.h"

#define CONFIRM_MS      4000    // a second tap must come within this
#define PPM_SAVE_MS     1000    // the frequency correction is saved once the taps stop
#define CAP_STEP_S      10
#define CAP_MAX_S       600

// The tuner's gains, as on the web page: tenths of a dB, after the two automatic choices.
static const int16_t k_gains[] = {SDR_GAIN_BEST, SDR_GAIN_AUTO, 0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166,
                                  197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445,
                                  480, 496};
#define NGAINS (int)(sizeof(k_gains) / sizeof(k_gains[0]))

// Time zones, as on the web page (IANA name, POSIX rule).
static const char *const k_zones[][2] = {
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},     {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},       {"America/Phoenix", "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},  {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Pacific/Honolulu", "HST10"},                      {"America/Puerto_Rico", "AST4"},
    {"Pacific/Guam", "ChST-10"},                        {"Pacific/Pago_Pago", "SST11"},
    {"America/Halifax", "AST4ADT,M3.2.0,M11.1.0"},      {"America/St_Johns", "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},      {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Mexico_City", "CST6"},                    {"America/Sao_Paulo", "<-03>3"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},      {"Europe/Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},     {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Oslo", "CET-1CEST,M3.5.0,M10.5.0/3"},      {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"}, {"Africa/Johannesburg", "SAST-2"},
    {"Asia/Dubai", "<+04>-4"},                          {"Asia/Kolkata", "IST-5:30"},
    {"Asia/Singapore", "<+08>-8"},                      {"Asia/Hong_Kong", "HKT-8"},
    {"Asia/Tokyo", "JST-9"},                            {"Australia/Perth", "AWST-8"},
    {"Australia/Brisbane", "AEST-10"},                  {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"}, {"UTC", "UTC0"},
};
#define NZONES (int)(sizeof(k_zones) / sizeof(k_zones[0]))

static lv_obj_t *s_net_sw, *s_net_note, *s_sdr_sw, *s_gain_dd, *s_ppm_lbl, *s_bias_sw, *s_bias_note;
static lv_obj_t *s_fwd_sw[FWD_DEST_MAX], *s_fwd_dd[FWD_DEST_MAX], *s_fwd_name[FWD_DEST_MAX], *s_fwd_note[FWD_DEST_MAX];
static lv_obj_t *s_bright_slider, *s_bright_lbl, *s_tz_dd;
static lv_obj_t *s_restart_lbl, *s_forget_lbl, *s_cap_lbl, *s_cap_btn, *s_cap_note;
static uint32_t s_bias_confirm, s_restart_confirm, s_forget_confirm; // lv_tick of the first tap, 0 = none
static uint32_t s_fwd_refused[FWD_DEST_MAX];                         // lv_tick a switch-on was refused
static int16_t s_ppm;                                                 // shown; saved after PPM_SAVE_MS
static uint32_t s_ppm_changed;                                        // 0 = nothing to save
static int s_cap_seconds = 60;
static char s_cap_error[96];

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

static lv_obj_t *text(lv_obj_t *parent, const char *t, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

// A muted line under a setting, wrapping.
static lv_obj_t *note(lv_obj_t *parent)
{
    lv_obj_t *l = text(parent, "", UI_COLOR_MUTED);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_label_set_recolor(l, true);
    return l;
}

static lv_obj_t *row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    return r;
}

static lv_obj_t *heading(lv_obj_t *parent, const char *t)
{
    lv_obj_t *l = text(parent, t, UI_COLOR_ACCENT);
    lv_obj_set_style_pad_top(l, 8, 0);
    return l;
}

static lv_obj_t *switch_in(lv_obj_t *parent, lv_event_cb_t cb, void *user)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 70, 38);
    lv_obj_set_style_bg_color(sw, UI_COLOR_PANEL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, UI_COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
    return sw;
}

static lv_obj_t *switch_row(lv_obj_t *parent, const char *label, lv_event_cb_t cb, void *user)
{
    lv_obj_t *r = row(parent);
    text(r, label, UI_COLOR_TEXT);
    return switch_in(r, cb, user);
}

static lv_obj_t *dropdown_in(lv_obj_t *parent, const char *options, int32_t w, lv_event_cb_t cb, void *user)
{
    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_obj_set_width(dd, w);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_t *parts[] = {dd, list};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(parts[i], UI_COLOR_PANEL, 0);
        lv_obj_set_style_text_color(parts[i], UI_COLOR_TEXT, 0);
        lv_obj_set_style_border_color(parts[i], UI_COLOR_MUTED, 0);
    }
    lv_obj_set_style_bg_color(list, UI_COLOR_ACCENT, LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_max_height(list, 300, 0);
    lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, user);
    return dd;
}

// An action button; a stepper button (repeat) also steps while held.
static lv_obj_t *button(lv_obj_t *parent, const char *t, int32_t w, lv_event_cb_t cb, void *user, bool repeat)
{
    lv_obj_t *b = ui_round_button(parent, t, w, 56);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_label_set_recolor(l, true);
    lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_DISABLED);
    if (repeat) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_SHORT_CLICKED, user);
        lv_obj_add_event_cb(b, cb, LV_EVENT_LONG_PRESSED_REPEAT, user);
    } else {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    }
    return b;
}

static lv_obj_t *column_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(tab, 28, 0);
    lv_obj_set_style_pad_ver(tab, 16, 0);
    lv_obj_set_style_pad_row(tab, 10, 0);
    return tab;
}

static void set_checked(lv_obj_t *o, bool on)
{
    if (lv_obj_has_state(o, LV_STATE_CHECKED) != on) lv_obj_set_state(o, LV_STATE_CHECKED, on);
}

static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

static bool confirmed(uint32_t *since)
{
    if (*since && lv_tick_elaps(*since) < CONFIRM_MS) {
        *since = 0;
        return true;
    }
    *since = lv_tick_get() | 1;
    return false;
}

// Change the settings: fn edits a copy, which is then saved (and announced).
static void change(void (*fn)(settings_t *, intptr_t), intptr_t v)
{
    settings_t cfg;
    settings_get(&cfg);
    fn(&cfg, v);
    settings_set(&cfg);
}

// ---------------------------------------------------------------------------
// AIS
// ---------------------------------------------------------------------------

static void set_net(settings_t *c, intptr_t v) { c->net_enabled = v; }
static void set_sdr(settings_t *c, intptr_t v) { c->sdr_enabled = v; }
static void set_gain(settings_t *c, intptr_t v) { c->sdr_gain = (int16_t)v; }
static void set_ppm(settings_t *c, intptr_t v) { c->sdr_ppm_tenths = (int16_t)v; }
static void set_bias(settings_t *c, intptr_t v) { c->sdr_bias_tee = v; }

static void net_cb(lv_event_t *e) { change(set_net, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
static void sdr_cb(lv_event_t *e) { change(set_sdr, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }

static void gain_cb(lv_event_t *e)
{
    uint32_t i = lv_dropdown_get_selected(lv_event_get_target(e));
    if (i < NGAINS) change(set_gain, k_gains[i]);
}

// The bias-tee puts 4.5 V on the antenna socket: turning it on takes a second tap.
static void bias_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (on && !confirmed(&s_bias_confirm)) {
        set_checked(sw, false);
        lv_label_set_text(s_bias_note, "#f2545b Tap again to turn it on.# It puts 4.5 V on the antenna socket: only "
                                       "for an LNA or active antenna powered through the coax.");
        return;
    }
    s_bias_confirm = 0;
    change(set_bias, on);
}

static void ppm_step_cb(lv_event_t *e)
{
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int v = s_ppm + step;
    if (v < -2000 || v > 2000) return;
    s_ppm = (int16_t)v;
    s_ppm_changed = lv_tick_get() | 1;
    char buf[16];
    snprintf(buf, sizeof(buf), "%+.1f ppm", s_ppm / 10.0);
    lv_label_set_text(s_ppm_lbl, buf);
}

static void build_ais(lv_obj_t *tab)
{
    column_tab(tab);
    s_net_sw = switch_row(tab, "Network feed", net_cb, NULL);
    s_net_note = note(tab);
    heading(tab, "RTL-SDR dongle");
    s_sdr_sw = switch_row(tab, "Decode AIS", sdr_cb, NULL);

    char opts[NGAINS * 12] = "Auto (best fixed)\nTuner AGC";
    for (int i = 2; i < NGAINS; i++) {
        size_t n = strlen(opts);
        snprintf(opts + n, sizeof(opts) - n, "\n%.1f dB", k_gains[i] / 10.0);
    }
    lv_obj_t *r = row(tab);
    text(r, "Gain", UI_COLOR_TEXT);
    s_gain_dd = dropdown_in(r, opts, 240, gain_cb, NULL);

    r = row(tab);
    text(r, "Correction", UI_COLOR_TEXT);
    lv_obj_t *step = row(r);
    lv_obj_set_width(step, LV_SIZE_CONTENT);
    button(step, LV_SYMBOL_MINUS, 56, ppm_step_cb, (void *)(intptr_t)-1, true);
    s_ppm_lbl = text(step, "", UI_COLOR_TEXT);
    lv_obj_set_width(s_ppm_lbl, 110);
    lv_obj_set_style_text_align(s_ppm_lbl, LV_TEXT_ALIGN_CENTER, 0);
    button(step, LV_SYMBOL_PLUS, 56, ppm_step_cb, (void *)(intptr_t)1, true);

    s_bias_sw = switch_row(tab, "Bias-tee", bias_cb, NULL);
    s_bias_note = note(tab);
}

// ---------------------------------------------------------------------------
// Retransmit
// ---------------------------------------------------------------------------

static void set_fwd_on(settings_t *c, intptr_t v) { c->fwd[v & 0xff].enabled = v >> 8; }
static void set_fwd_proto(settings_t *c, intptr_t v) { c->fwd[v & 0xff].proto = (uint8_t)(v >> 8); }

static void fwd_sw_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    settings_t cfg;
    settings_get(&cfg);
    if (on && (!cfg.fwd[i].host[0] || !cfg.fwd[i].port)) { // the address is typed on the web page
        set_checked(sw, false);
        s_fwd_refused[i] = lv_tick_get() | 1;
        return;
    }
    change(set_fwd_on, i | (on << 8));
}

static void fwd_dd_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    change(set_fwd_proto, i | (lv_dropdown_get_selected(lv_event_get_target(e)) ? FWD_TCP << 8 : FWD_UDP << 8));
}

static void build_fwd(lv_obj_t *tab)
{
    column_tab(tab);
    lv_obj_t *n = note(tab);
    lv_label_set_text(n, "Sends what the dongle decodes on. Addresses and ports are set on the web page.");
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        s_fwd_name[i] = heading(tab, "");
        lv_label_set_long_mode(s_fwd_name[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_fwd_name[i], lv_pct(100));
        lv_obj_t *r = row(tab);
        s_fwd_dd[i] = dropdown_in(r, "UDP\nTCP", 120, fwd_dd_cb, (void *)(intptr_t)i);
        s_fwd_note[i] = text(r, "", UI_COLOR_MUTED);
        lv_obj_set_flex_grow(s_fwd_note[i], 1);
        lv_label_set_long_mode(s_fwd_note[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(s_fwd_note[i], &lv_font_montserrat_16, 0);
        lv_label_set_recolor(s_fwd_note[i], true);
        s_fwd_sw[i] = switch_in(r, fwd_sw_cb, (void *)(intptr_t)i);
    }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

static void set_bright(settings_t *c, intptr_t v) { c->brightness = (uint8_t)v; }
static void set_tz(settings_t *c, intptr_t v)
{
    strlcpy(c->tz_name, k_zones[v][0], sizeof(c->tz_name));
    strlcpy(c->tz_posix, k_zones[v][1], sizeof(c->tz_posix));
}

// Brightness follows the finger, and is saved when it lets go.
static void bright_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(s_bright_slider);
    lv_label_set_text_fmt(s_bright_lbl, "Brightness  %d%%", v);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) bsp_display_brightness_set(v);
    else change(set_bright, v);
}

static void tz_cb(lv_event_t *e)
{
    uint32_t i = lv_dropdown_get_selected(lv_event_get_target(e));
    if (i < NZONES) change(set_tz, i); // the last one, "Other", is set on the web page
}

static void build_display(lv_obj_t *tab)
{
    column_tab(tab);
    s_bright_lbl = text(tab, "", UI_COLOR_TEXT);
    s_bright_slider = lv_slider_create(tab);
    lv_slider_set_range(s_bright_slider, 5, 100);
    lv_obj_set_width(s_bright_slider, lv_pct(92));
    lv_obj_set_height(s_bright_slider, 16);
    lv_obj_set_style_pad_ver(s_bright_slider, 12, LV_PART_MAIN); // bigger touch target
    lv_obj_set_style_margin_bottom(s_bright_slider, 12, 0);
    lv_obj_add_event_cb(s_bright_slider, bright_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_bright_slider, bright_cb, LV_EVENT_RELEASED, NULL);

    char opts[NZONES * 24 + 32] = "";
    for (int i = 0; i < NZONES; i++) {
        size_t n = strlen(opts);
        snprintf(opts + n, sizeof(opts) - n, "%s%s", i ? "\n" : "", k_zones[i][0]);
        for (char *c = opts + n; *c; c++) if (*c == '_') *c = ' ';
    }
    strlcat(opts, "\nOther (web page)", sizeof(opts));
    text(tab, "Time zone", UI_COLOR_TEXT);
    s_tz_dd = dropdown_in(tab, opts, 400, tz_cb, NULL);
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(300)); // let the button redraw
    vessel_db_save();
    esp_restart();
}

static void restart_cb(lv_event_t *e)
{
    if (!confirmed(&s_restart_confirm)) return;
    lv_label_set_text(s_restart_lbl, "Restarting...");
    xTaskCreate(restart_task, "ui_restart", 2048, NULL, 5, NULL);
}

static void forget_cb(lv_event_t *e)
{
    if (!confirmed(&s_forget_confirm)) return;
    vessel_db_forget_all();
}

static void cap_step_cb(lv_event_t *e)
{
    int v = s_cap_seconds + (int)(intptr_t)lv_event_get_user_data(e) * CAP_STEP_S;
    if (v < CAP_STEP_S || v > CAP_MAX_S) return;
    s_cap_seconds = v;
    lv_label_set_text_fmt(s_cap_lbl, "%d s", v);
}

static void cap_cb(lv_event_t *e)
{
    if (!sdr_capture_start(s_cap_seconds, s_cap_error, sizeof(s_cap_error))) return;
    s_cap_error[0] = '\0';
}

static void build_system(lv_obj_t *tab)
{
    column_tab(tab);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_restart_lbl = lv_obj_get_child(button(tab, "", 320, restart_cb, NULL, false), 0);
    s_forget_lbl = lv_obj_get_child(button(tab, "", 320, forget_cb, NULL, false), 0);

    heading(tab, "Record an RTL-SDR capture");
    lv_obj_t *r = row(tab);
    lv_obj_set_width(r, LV_SIZE_CONTENT);
    button(r, LV_SYMBOL_MINUS, 56, cap_step_cb, (void *)(intptr_t)-1, true);
    s_cap_lbl = text(r, "", UI_COLOR_TEXT);
    lv_label_set_text_fmt(s_cap_lbl, "%d s", s_cap_seconds);
    lv_obj_set_width(s_cap_lbl, 90);
    lv_obj_set_style_text_align(s_cap_lbl, LV_TEXT_ALIGN_CENTER, 0);
    button(r, LV_SYMBOL_PLUS, 56, cap_step_cb, (void *)(intptr_t)1, true);
    s_cap_btn = button(r, "Record", 140, cap_cb, NULL, false);
    s_cap_note = note(tab);
    lv_obj_set_style_text_align(s_cap_note, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------------------

void ui_settings_build(lv_obj_t *tabs)
{
    build_ais(lv_tabview_add_tab(tabs, "AIS"));
    build_fwd(lv_tabview_add_tab(tabs, "Retransmit"));
    build_display(lv_tabview_add_tab(tabs, "Display"));
    build_system(lv_tabview_add_tab(tabs, "System"));
}

// Show the saved settings and how things are going (each second, and when the
// screen opens). Controls being used are left alone.
void ui_settings_refresh(void)
{
    settings_t cfg;
    settings_get(&cfg);
    char buf[128];

    // AIS
    set_checked(s_net_sw, cfg.net_enabled);
    if (cfg.ais_host[0]) snprintf(buf, sizeof(buf), "Server %s:%u (set on the web page)", cfg.ais_host, cfg.ais_port);
    else strcpy(buf, "No server set: enter one on the web page");
    set_text(s_net_note, buf);
    set_checked(s_sdr_sw, cfg.sdr_enabled);
    if (!lv_dropdown_is_open(s_gain_dd)) {
        for (int i = 0; i < NGAINS; i++) {
            if (k_gains[i] == cfg.sdr_gain && lv_dropdown_get_selected(s_gain_dd) != (uint32_t)i)
                lv_dropdown_set_selected(s_gain_dd, i);
        }
    }
    if (s_ppm_changed && lv_tick_elaps(s_ppm_changed) > PPM_SAVE_MS) {
        s_ppm_changed = 0;
        change(set_ppm, s_ppm);
    } else if (!s_ppm_changed) {
        s_ppm = cfg.sdr_ppm_tenths;
    }
    snprintf(buf, sizeof(buf), "%+.1f ppm", s_ppm / 10.0);
    set_text(s_ppm_lbl, buf);
    if (s_bias_confirm && lv_tick_elaps(s_bias_confirm) > CONFIRM_MS) s_bias_confirm = 0;
    if (!s_bias_confirm) {
        set_checked(s_bias_sw, cfg.sdr_bias_tee);
        set_text(s_bias_note, cfg.sdr_bias_tee ? "#f5b13f 4.5 V on the antenna socket#" : "Powers an LNA or active antenna");
    }

    // Retransmit
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        const fwd_dest_t *d = &cfg.fwd[i];
        if (d->host[0] && d->port) snprintf(buf, sizeof(buf), "%d   %s:%u", i + 1, d->host, d->port);
        else snprintf(buf, sizeof(buf), "%d   not set up", i + 1);
        set_text(s_fwd_name[i], buf);
        set_checked(s_fwd_sw[i], d->enabled);
        if (!lv_dropdown_is_open(s_fwd_dd[i]) && lv_dropdown_get_selected(s_fwd_dd[i]) != d->proto)
            lv_dropdown_set_selected(s_fwd_dd[i], d->proto);
        fwd_status_t fs;
        ais_fwd_get_status(i, &fs);
        if (s_fwd_refused[i] && lv_tick_elaps(s_fwd_refused[i]) < CONFIRM_MS) {
            strcpy(buf, "#f2545b Set its address on the web page#");
        } else {
            s_fwd_refused[i] = 0;
            snprintf(buf, sizeof(buf), "%s%s%s", d->enabled ? ais_fwd_state_str(fs.state) : "off",
                     d->enabled && fs.error[0] ? ": " : "", d->enabled ? fs.error : "");
        }
        set_text(s_fwd_note[i], buf);
    }

    // Display
    if (!lv_obj_has_state(s_bright_slider, LV_STATE_PRESSED)) {
        if (lv_slider_get_value(s_bright_slider) != cfg.brightness)
            lv_slider_set_value(s_bright_slider, cfg.brightness, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "Brightness  %u%%", cfg.brightness);
        set_text(s_bright_lbl, buf);
    }
    if (!lv_dropdown_is_open(s_tz_dd)) {
        int z = NZONES; // "Other": a rule typed on the web page
        for (int i = 0; i < NZONES; i++) {
            if (strcmp(cfg.tz_name, k_zones[i][0]) == 0 && strcmp(cfg.tz_posix, k_zones[i][1]) == 0) z = i;
        }
        if (lv_dropdown_get_selected(s_tz_dd) != (uint32_t)z) lv_dropdown_set_selected(s_tz_dd, z);
    }

    // System
    if (s_restart_confirm && lv_tick_elaps(s_restart_confirm) > CONFIRM_MS) s_restart_confirm = 0;
    if (s_forget_confirm && lv_tick_elaps(s_forget_confirm) > CONFIRM_MS) s_forget_confirm = 0;
    if (strcmp(lv_label_get_text(s_restart_lbl), "Restarting...") != 0)
        set_text(s_restart_lbl, s_restart_confirm ? "#f2545b Tap again to restart#" : "Restart display");
    set_text(s_forget_lbl, s_forget_confirm ? "#f2545b Tap again to forget all#" : "Forget all vessels");

    sdr_status_t r;
    sdr_get_status(&r);
    bool ready = r.state == SDR_READY;
    if (ready) lv_obj_remove_state(s_cap_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_cap_btn, LV_STATE_DISABLED);
    if (r.state == SDR_CAPTURING) {
        s_cap_error[0] = '\0';
        snprintf(buf, sizeof(buf), "Recording... %.0f%%", r.cap_total ? 100.0 * r.cap_bytes / r.cap_total : 0);
    } else if (s_cap_error[0]) {
        snprintf(buf, sizeof(buf), "#f2545b %s#", s_cap_error);
    } else if (r.cap_error[0]) {
        snprintf(buf, sizeof(buf), "#f2545b %s#", r.cap_error);
    } else if (r.cap_file_bytes) {
        snprintf(buf, sizeof(buf), "Last capture: %.0f MB, download it from the web page", r.cap_file_bytes / 1048576.0);
    } else {
        strcpy(buf, ready ? "Saved to the microSD card" : "Needs the dongle receiving");
    }
    set_text(s_cap_note, buf);
}
