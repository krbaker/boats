// Weather overlay (tap the time on the map): the marine forecast with any
// alerts, the buoy's waves, and the weather station, sun and moon
// (WEATHER_PLAN.md). Styled like the vessel overlay.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"

#include "settings.h"
#include "tide.h"
#include "ui.h"
#include "weather.h"

#define BOX_W       540     // as the vessel overlay: its corners just fit inside the round screen
#define BOX_H       560
#define BOX_Y       24
#define POLL_MS     1000
#define OLD_S       (45 * 60)   // a buoy value this much older than the newest row says how old
enum { TAB_FORECAST, TAB_WAVES, TAB_WEATHER };

static lv_obj_t *s_root, *s_tabs, *s_forecast, *s_waves, *s_waves_head, *s_weather, *s_weather_head;
static weather_data_t *s_wx;
static uint32_t s_seen_gen;
static uint32_t s_forecast_sig; // what the forecast tab was built from

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static lv_obj_t *label(lv_obj_t *parent, const char *t, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

// "3:45 PM" today, "Tue 3:45 PM" otherwise.
static void when(time_t t, char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    char hm[16];
    ui_format_time(t, hm, sizeof(hm), false);
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) {
        snprintf(buf, len, "%s", hm);
    } else {
        char day[8];
        strftime(day, sizeof(day), "%a", &a);
        snprintf(buf, len, "%s %s", day, hm);
    }
}

// "4 min ago", "2 h ago".
static void ago(time_t t, char *buf, size_t len)
{
    long s = (long)(time(NULL) - t);
    if (s < 0) s = 0;
    if (s < 90) snprintf(buf, len, "just now");
    else if (s < 5400) snprintf(buf, len, "%ld min ago", (s + 30) / 60);
    else if (s < 2 * 86400) snprintf(buf, len, "%ld h ago", (s + 1800) / 3600);
    else snprintf(buf, len, "%ld days ago", s / 86400);
}

// "TUE NIGHT" -> "Tue Night"
static void title_case(const char *in, char *out, size_t len)
{
    size_t i = 0;
    bool start = true;
    for (; in[i] && i + 1 < len; i++) {
        char c = in[i];
        out[i] = start ? c : (c >= 'A' && c <= 'Z' ? c + 32 : c);
        start = c == ' ' || c == '-' || c == '/';
    }
    out[i] = '\0';
}

// A two-column table of name / value, styled like the vessel overlay's.
static lv_obj_t *kv_table(lv_obj_t *parent)
{
    lv_obj_t *t = lv_table_create(parent);
    lv_table_set_column_count(t, 2);
    lv_table_set_column_width(t, 0, 190);
    lv_table_set_column_width(t, 1, BOX_W - 2 * 20 - 190);
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_text_color(t, UI_COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_border_color(t, UI_COLOR_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(t, 9, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(t, 8, LV_PART_ITEMS);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, LV_PART_ITEMS);
    return t;
}

typedef struct {
    lv_obj_t *t;
    int row;
} rows_t;

static void row(rows_t *r, const char *name, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void row(rows_t *r, const char *name, const char *fmt, ...)
{
    char v[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof(v), fmt, ap);
    va_end(ap);
    lv_table_set_cell_value(r->t, r->row, 0, name);
    lv_table_set_cell_value(r->t, r->row, 1, v);
    r->row++;
}

static void rows_end(rows_t *r)
{
    lv_table_set_row_count(r->t, r->row);
    lv_obj_set_hidden(r->t, r->row == 0);
}

static lv_obj_t *column_tab(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(tab, 20, 0);
    lv_obj_set_style_pad_ver(tab, 14, 0);
    lv_obj_set_style_pad_row(tab, 8, 0);
    return tab;
}

// What's shown when a source has nothing yet: not set up, waiting, or failing.
static const char *missing(const wx_source_t *src, const char *setup)
{
    static char buf[160];
    if (src->state == WX_OFF) return setup;
    if (src->state == WX_ERROR) {
        snprintf(buf, sizeof(buf), "Can't get it: %s. Trying again in a few minutes.", src->error);
        return buf;
    }
    return clock_is_set() ? "Fetching..." : "Waiting for the network and the clock...";
}

// ---------------------------------------------------------------------------
// Marine forecast
// ---------------------------------------------------------------------------

static void alert_click_cb(lv_event_t *e)
{
    lv_obj_t *desc = lv_event_get_user_data(e);
    lv_obj_set_hidden(desc, !lv_obj_is_hidden(desc));
}

static uint32_t forecast_signature(const weather_data_t *w)
{
    // FNV-1a over what the tab shows, so it's only rebuilt (and scrolled back
    // to the top) when that changes, not on every weather update.
    uint32_t h = 2166136261u;
    const unsigned char *parts[] = {(const void *)&w->forecast, (const void *)w->alerts,
                                    (const void *)&w->forecast_src.state, (const void *)&w->alerts_src.state};
    const size_t sizes[] = {w->have_forecast ? sizeof(w->forecast) : 0, w->nalerts * sizeof(w->alerts[0]),
                            sizeof(w->forecast_src.state), sizeof(w->alerts_src.state)};
    for (int p = 0; p < 4; p++) {
        for (size_t i = 0; i < sizes[p]; i++) h = (h ^ parts[p][i]) * 16777619u;
    }
    return h ^ (uint32_t)w->nalerts ^ (w->have_forecast ? 0x80000000u : 0);
}

// An alert as a box: red for a watch, warning or advisory, grey otherwise.
// Collapsed (in the menu), the text shows when the box is tapped; otherwise
// (the popup) it's always shown. Returns the text's label.
static lv_obj_t *alert_box(lv_obj_t *parent, const wx_alert_t *a, bool collapsed)
{
    char buf[64];
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, a->flag ? lv_color_hex(0x4a1c22) : UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, a->flag ? UI_COLOR_BAD : UI_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_scrollable(box, false);
    label(box, a->event, &lv_font_montserrat_24, a->flag ? UI_COLOR_BAD : UI_COLOR_TEXT);
    const char *more = collapsed ? "  " LV_SYMBOL_DOWN : "";
    if (a->ends) {
        char t[32];
        when(a->ends, t, sizeof(t));
        snprintf(buf, sizeof(buf), "Until %s%s", t, more);
    } else {
        snprintf(buf, sizeof(buf), "%s", collapsed ? "Tap for details  " LV_SYMBOL_DOWN : "");
    }
    if (buf[0]) label(box, buf, &lv_font_montserrat_16, UI_COLOR_MUTED);
    lv_obj_t *desc = label(box, a->description[0] ? a->description : a->headline, &lv_font_montserrat_16, UI_COLOR_TEXT);
    if (collapsed) {
        lv_obj_set_hidden(desc, true);
        lv_obj_set_clickable(box, true);
        lv_obj_add_event_cb(box, alert_click_cb, LV_EVENT_CLICKED, desc);
    }
    return box;
}

static void build_forecast(const weather_data_t *w)
{
    lv_obj_t *tab = s_forecast;
    lv_obj_clean(tab);
    char buf[128];

    // Alerts first: tap one for the full text.
    for (int i = 0; i < w->nalerts; i++) alert_box(tab, &w->alerts[i], true);

    if (!w->have_forecast) {
        label(tab, missing(&w->forecast_src, "Set a NOAA marine forecast zone on the web page, under \"Tides, weather & time\"."),
              &lv_font_montserrat_20, UI_COLOR_MUTED);
        return;
    }
    const marine_forecast_t *f = &w->forecast;
    label(tab, f->zone_name, &lv_font_montserrat_24, UI_COLOR_TEXT);
    snprintf(buf, sizeof(buf), "%s, issued %s", f->zone, f->issued);
    label(tab, buf, &lv_font_montserrat_16, UI_COLOR_MUTED);
    for (int i = 0; i < f->nheadlines; i++) label(tab, f->headline[i], &lv_font_montserrat_20, UI_COLOR_WARN);
    for (int i = 0; i < f->nperiods; i++) {
        char name[24];
        title_case(f->period[i].name, name, sizeof(name));
        lv_obj_t *n = label(tab, name, &lv_font_montserrat_20, UI_COLOR_ACCENT);
        lv_obj_set_style_pad_top(n, 6, 0);
        label(tab, f->period[i].text, &lv_font_montserrat_20, UI_COLOR_TEXT);
    }
    if (w->forecast_src.state == WX_ERROR) {
        snprintf(buf, sizeof(buf), "Couldn't update: %s", w->forecast_src.error);
        label(tab, buf, &lv_font_montserrat_16, UI_COLOR_WARN);
    }
}

// ---------------------------------------------------------------------------
// Waves (the buoy)
// ---------------------------------------------------------------------------

// " (2 h ago)" when a value comes from well before the newest row.
static const char *age_note(const buoy_t *b, const buoy_val_t *v)
{
    static char buf[24];
    char a[16];
    if (b->latest - v->when < OLD_S) return "";
    ago(v->when, a, sizeof(a));
    snprintf(buf, sizeof(buf), " (%s)", a);
    return buf;
}

static void show_waves(const weather_data_t *w)
{
    char buf[96], a[24];
    rows_t r = {lv_obj_get_child(s_waves, 1), 0};
    if (!w->have_buoy) {
        lv_label_set_text(s_waves_head, missing(&w->buoy_src, "Set an NDBC buoy on the web page, under \"Tides, weather & time\"."));
        lv_obj_set_style_text_color(s_waves_head, UI_COLOR_MUTED, 0);
        rows_end(&r);
        return;
    }
    const buoy_t *b = &w->buoy;
    ago(b->latest, a, sizeof(a));
    snprintf(buf, sizeof(buf), "Buoy %s, %s%s", w->buoy_id, a, w->buoy_src.state == WX_ERROR ? " (can't update)" : "");
    lv_label_set_text(s_waves_head, buf);
    lv_obj_set_style_text_color(s_waves_head, UI_COLOR_MUTED, 0);
    if (b->wave_ft.ok) row(&r, "Waves", "%.1f ft%s", b->wave_ft.v, age_note(b, &b->wave_ft));
    if (b->dom_period_s.ok) row(&r, "Period", "%.0f s dominant%s", b->dom_period_s.v, b->avg_period_s.ok ? "" : age_note(b, &b->dom_period_s));
    if (b->avg_period_s.ok) row(&r, b->dom_period_s.ok ? "" : "Period", "%.1f s average", b->avg_period_s.v);
    if (b->wave_dir.ok) row(&r, "From", "%s (%.0f°)%s", wx_compass((int)b->wave_dir.v), b->wave_dir.v, age_note(b, &b->wave_dir));
    if (!b->wave_ft.ok) row(&r, "Waves", "this buoy doesn't report waves");
    if (b->wind_mph.ok) {
        snprintf(buf, sizeof(buf), "%.0f mph", b->wind_mph.v);
        if (b->wind_dir.ok) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " from %s", wx_compass((int)b->wind_dir.v));
        if (b->gust_mph.ok) snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", gusts %.0f", b->gust_mph.v);
        row(&r, "Wind", "%s%s", buf, age_note(b, &b->wind_mph));
    }
    if (b->water_f.ok) row(&r, "Water", "%.0f °F%s", b->water_f.v, age_note(b, &b->water_f));
    if (b->air_f.ok) row(&r, "Air", "%.0f °F%s", b->air_f.v, age_note(b, &b->air_f));
    if (b->pressure_inhg.ok) row(&r, "Pressure", "%.2f inHg%s", b->pressure_inhg.v, age_note(b, &b->pressure_inhg));
    if (b->pressure_trend_inhg.ok) {
        float d = b->pressure_trend_inhg.v;
        row(&r, "3 h change", "%+.2f inHg, %s", d, wx_trend_name(wx_pressure_trend(d)));
    }
    rows_end(&r);
}

// ---------------------------------------------------------------------------
// Weather (the station, sun and moon)
// ---------------------------------------------------------------------------

static void show_weather(const weather_data_t *w)
{
    char buf[96], a[24], t1[24], t2[24];
    rows_t r = {lv_obj_get_child(s_weather, 1), 0};
    settings_t cfg;
    settings_get(&cfg);
    time_t now = time(NULL);

    if (w->have_station) {
        const wl_current_t *c = &w->station;
        ago(c->when, a, sizeof(a));
        snprintf(buf, sizeof(buf), "%s, %s%s", w->station_name[0] ? w->station_name : "Weather station", a,
                 w->station_src.state == WX_ERROR ? " (can't update)" : "");
        lv_label_set_text(s_weather_head, buf);
        if (c->has_wind) {
            row(&r, "Wind", "%.0f mph from %s (%d°)", c->wind_avg10_mph, wx_compass(c->wind_dir_avg10), c->wind_dir_avg10);
            row(&r, "", "gusts %.0f, now %.0f", c->gust10_mph, c->wind_mph);
        }
        if (c->has_temp) row(&r, "Temperature", "%.1f °F", c->temp_f);
        if (c->has_hum && c->has_dew) row(&r, "Humidity", "%.0f%%, dew point %.0f °F", c->hum_pct, c->dew_f);
        else if (c->has_hum) row(&r, "Humidity", "%.0f%%", c->hum_pct);
        if (c->has_bar) {
            row(&r, "Pressure", "%.2f inHg, %s", c->bar_inhg, wx_trend_name(wx_pressure_trend(c->bar_trend_inhg)));
            row(&r, "", "%+.2f inHg in 3 h", c->bar_trend_inhg);
        }
        if (c->has_rain) row(&r, "Rain", "%.2f in/h, %.2f in today", c->rain_rate_in_hr, c->rain_today_in);
    } else {
        lv_label_set_text(s_weather_head, missing(&w->station_src, "Set up a WeatherLink station on the web page, under \"Tides, weather & time\"."));
    }
    if (w->have_obs) {
        ago(w->obs.when, a, sizeof(a));
        row(&r, "Sky", "%s (NWS %s, %s)", w->obs.text[0] ? w->obs.text : "?", w->obs_station, a);
    }

    if (clock_is_set()) {
        // Today's sunrise and sunset at the chart's centre: search local midnight to midnight.
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        time_t midnight = mktime(&tm), rise, set;
        bool has_rise = sun_event(cfg.center_lat, cfg.center_lon, midnight, midnight + 86400, true, &rise);
        bool has_set = sun_event(cfg.center_lat, cfg.center_lon, midnight, midnight + 86400, false, &set);
        if (has_rise) ui_format_time(rise, t1, sizeof(t1), false);
        if (has_set) ui_format_time(set, t2, sizeof(t2), false);
        row(&r, "Sun", "rises %s, sets %s", has_rise ? t1 : "-", has_set ? t2 : "-");
        moon_t m;
        moon_at(now, &m);
        row(&r, "Moon", "%s, %.0f%% lit", moon_phase_name(m.phase), m.illum * 100);
        time_t full = moon_next(now, true), new_ = moon_next(now, false);
        struct tm ft, nt;
        localtime_r(&full, &ft);
        localtime_r(&new_, &nt);
        char fd[16], nd[16];
        snprintf(fd, sizeof(fd), "%.3s %d", "JanFebMarAprMayJunJulAugSepOctNovDec" + 3 * ft.tm_mon, ft.tm_mday);
        snprintf(nd, sizeof(nd), "%.3s %d", "JanFebMarAprMayJunJulAugSepOctNovDec" + 3 * nt.tm_mon, nt.tm_mday);
        if (full < new_) row(&r, "", "full %s, new %s", fd, nd);
        else row(&r, "", "new %s, full %s", nd, fd);
    }
    rows_end(&r);
}

// ---------------------------------------------------------------------------

static void refresh(bool force)
{
    if (lv_obj_is_hidden(s_root)) return;
    if (!force && weather_generation() == s_seen_gen) return;
    s_seen_gen = weather_generation();
    weather_get(s_wx);
    uint32_t sig = forecast_signature(s_wx);
    if (force || sig != s_forecast_sig) {
        s_forecast_sig = sig;
        build_forecast(s_wx);
    }
    show_waves(s_wx);
    show_weather(s_wx);
}

// ---------------------------------------------------------------------------
// Alert popup: a new or updated watch, warning or advisory shows over
// whatever screen is up, until its X is tapped.
// ---------------------------------------------------------------------------

#define POPUP_W         520
#define POPUP_Y         150     // the box's top: the X sits at a fixed place
#define POPUP_TEXT_H    330     // the text scrolls beyond this
#define MAX_DISMISSED   16

static lv_obj_t *s_popup;
static uint32_t s_popup_id;
static uint32_t s_dismissed[MAX_DISMISSED];
static int s_ndismissed;
static uint32_t s_alert_gen;

// An alert's identity. NWS re-issues an alert when it changes, with a new
// headline ("...issued October 5 at 8:54PM..."), so an update counts as new.
static uint32_t alert_id(const wx_alert_t *a)
{
    uint32_t h = 2166136261u;
    for (const char *p = a->event; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    for (const char *p = a->headline; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return (h ^ (uint32_t)a->ends) | 1; // never 0: that means "no popup"
}

static bool dismissed(uint32_t id)
{
    for (int i = 0; i < s_ndismissed; i++) {
        if (s_dismissed[i] == id) return true;
    }
    return false;
}

static void popup_close(void)
{
    if (s_popup) lv_obj_delete(s_popup);
    s_popup = NULL;
    s_popup_id = 0;
}

static void check_alerts(void);

static void popup_x_cb(lv_event_t *e)
{
    if (s_ndismissed == MAX_DISMISSED) { // forget the oldest
        memmove(s_dismissed, s_dismissed + 1, sizeof(s_dismissed) - sizeof(s_dismissed[0]));
        s_ndismissed--;
    }
    s_dismissed[s_ndismissed++] = s_popup_id;
    popup_close();
    s_alert_gen = 0; // look again now: there may be another new one
    check_alerts();
}

static void popup_open(const wx_alert_t *a, uint32_t id)
{
    popup_close();
    s_popup_id = id;
    s_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, POPUP_W, LV_SIZE_CONTENT);
    lv_obj_align(s_popup, LV_ALIGN_TOP_MID, 0, POPUP_Y);
    lv_obj_set_scrollable(s_popup, false);
    lv_obj_set_clickable(s_popup, true); // touches on it don't reach the map underneath

    lv_obj_t *box = alert_box(s_popup, a, false);
    lv_obj_set_style_pad_right(box, 64, 0); // room for the X
    lv_obj_set_style_shadow_width(box, 24, 0);
    lv_obj_set_style_shadow_color(box, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_60, 0);
    lv_obj_t *text = lv_obj_get_child(box, lv_obj_get_child_count(box) - 1);
    lv_obj_set_style_max_height(text, POPUP_TEXT_H, 0);
    lv_obj_set_scrollable(text, true);
    lv_obj_set_scroll_dir(text, LV_DIR_VER);

    lv_obj_t *x = ui_round_button(s_popup, LV_SYMBOL_CLOSE, 52, 52);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_event_cb(x, popup_x_cb, LV_EVENT_CLICKED, NULL);
}

// Shows the first flagged alert not yet dismissed; closes the popup if its
// alert has gone (expired or cancelled).
static void check_alerts(void)
{
    if (weather_generation() == s_alert_gen) return;
    s_alert_gen = weather_generation();
    weather_get(s_wx);
    if (s_wx->alerts_src.state != WX_OK && s_wx->alerts_src.state != WX_ERROR) {
        popup_close(); // alerts turned off
        return;
    }
    bool still_there = false;
    const wx_alert_t *next = NULL;
    uint32_t next_id = 0;
    for (int i = 0; i < s_wx->nalerts; i++) {
        const wx_alert_t *a = &s_wx->alerts[i];
        if (!a->flag) continue;
        uint32_t id = alert_id(a);
        if (id == s_popup_id) still_there = true;
        if (!next && !dismissed(id)) {
            next = a;
            next_id = id;
        }
    }
    if (s_popup && still_there) return;
    if (next) popup_open(next, next_id);
    else popup_close();
}

static void poll_timer_cb(lv_timer_t *t)
{
    check_alerts();
    refresh(false);
}

static void close_cb(lv_event_t *e)
{
    lv_obj_set_hidden(s_root, true);
}

lv_obj_t *ui_weather_create(lv_obj_t *parent)
{
    s_wx = heap_caps_malloc(sizeof(*s_wx), MALLOC_CAP_SPIRAM);
    s_root = ui_overlay_create(parent, close_cb);

    s_tabs = lv_tabview_create(s_root);
    lv_obj_set_size(s_tabs, BOX_W, BOX_H);
    lv_obj_align(s_tabs, LV_ALIGN_CENTER, 0, BOX_Y);
    lv_obj_set_style_bg_color(s_tabs, UI_COLOR_BG, 0);
    lv_obj_set_style_radius(s_tabs, 24, 0);
    lv_obj_set_style_clip_corner(s_tabs, true, 0);
    lv_tabview_set_tab_bar_size(s_tabs, 56);
    lv_obj_set_scrollable(lv_tabview_get_content(s_tabs), false); // tabs switch from the bar only
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabs);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_24, 0);

    s_forecast = column_tab(lv_tabview_add_tab(s_tabs, "Forecast"));
    s_waves = column_tab(lv_tabview_add_tab(s_tabs, "Waves"));
    s_weather = column_tab(lv_tabview_add_tab(s_tabs, "Weather"));
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_set_style_text_color(lv_obj_get_child(bar, i), UI_COLOR_TEXT, 0);
    }
    // Waves and Weather: a heading line, then a table filled in place.
    s_waves_head = label(s_waves, "", &lv_font_montserrat_16, UI_COLOR_MUTED);
    kv_table(s_waves);
    s_weather_head = label(s_weather, "", &lv_font_montserrat_16, UI_COLOR_MUTED);
    kv_table(s_weather);

    lv_timer_create(poll_timer_cb, POLL_MS, NULL);
    return s_root;
}

void ui_weather_open(void)
{
    lv_obj_set_hidden(s_root, false);
    lv_tabview_set_active(s_tabs, TAB_FORECAST, LV_ANIM_OFF);
    refresh(true);
    lv_obj_scroll_to_y(s_forecast, 0, LV_ANIM_OFF);
}
