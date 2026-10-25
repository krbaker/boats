// A weather station heard over AIS (met_db): tapped on the map, everything it
// reports, in a popup over the map. It stays up to date while open.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "geo.h"
#include "met_db.h"
#include "settings.h"
#include "tide.h"
#include "ui.h"

#define POPUP_W     520
#define POPUP_Y     130
#define TEXT_MAX_H  360     // the readings scroll beyond this

static lv_obj_t *s_popup, *s_title, *s_sub, *s_body;
static uint32_t s_mmsi;
static uint8_t s_site;
static double s_lat, s_lon;
static lv_timer_t *s_timer;

static const char *trend_str(uint8_t t)
{
    return t == AIS_TREND_RISING ? ", rising" : t == AIS_TREND_FALLING ? ", falling" : t == AIS_TREND_STEADY ? ", steady" : "";
}

static float f_of(float c) { return c * 9 / 5 + 32; }

// When a reading was taken: AIS gives only the day of the month and the UTC
// time, so it's this month's (or last month's, for a day still to come).
static void reading_time(const ais_met_t *m, char *buf, size_t len)
{
    if (!(m->fields & AIS_MET_TIME)) {
        buf[0] = '\0';
        return;
    }
    if (!clock_is_set()) {
        snprintf(buf, len, "%02u:%02u UTC", m->hour, m->minute);
        return;
    }
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    int ago_min = (tm.tm_mday - m->day) * 1440 + (tm.tm_hour - m->hour) * 60 + (tm.tm_min - m->minute);
    if (ago_min < -1440) { // a day still to come: last month's
        struct tm last = tm;
        last.tm_mday = 0; // normalises to the last day of last month
        mktime(&last);
        ago_min += last.tm_mday * 1440;
    }
    ui_format_time(now - tm.tm_sec - (time_t)ago_min * 60, buf, len, false);
}

static void fill(const met_station_t *st)
{
    const ais_met_t *m = &st->m;
    char buf[1024];
    int n = 0;
#define ADD(...) (n += snprintf(buf + n, n < (int)sizeof(buf) ? sizeof(buf) - n : 0, __VA_ARGS__))

    if (m->fields & AIS_MET_NAME) lv_label_set_text(s_title, m->name);
    else lv_label_set_text_fmt(s_title, "Weather station %lu", (unsigned long)st->mmsi);

    char when[32], sub[128];
    reading_time(m, when, sizeof(when));
    int64_t age = (esp_timer_get_time() - st->last_us) / 1000000;
    int k = snprintf(sub, sizeof(sub), "%s%s", when[0] ? "Reading at " : "", when);
    snprintf(sub + k, sizeof(sub) - k, "%sheard %lld %s ago", k ? ", " : "", (long long)(age < 120 ? age : age / 60),
             age < 120 ? "s" : "min");
    if (m->fields & AIS_MET_POS) {
        settings_t cfg;
        settings_get(&cfg);
        size_t l = strlen(sub);
        snprintf(sub + l, sizeof(sub) - l, "\n%.1f nm %03.0f\xc2\xb0 from the chart's centre",
                 geo_distance_nm(cfg.center_lat, cfg.center_lon, m->lat, m->lon),
                 geo_bearing_deg(cfg.center_lat, cfg.center_lon, m->lat, m->lon));
    }
    lv_label_set_text(s_sub, sub);

    if (m->fields & AIS_MET_WIND) {
        ADD("Wind  %.0f kn (%.0f mph) from %03u\xc2\xb0", m->wind_kn, m->wind_kn * 1.15078f, m->wind_dir);
        if (m->fields & AIS_MET_GUST) ADD(", gusts %.0f kn", m->gust_kn);
        ADD("\n");
    } else if (m->fields & AIS_MET_GUST) {
        ADD("Gusts  %.0f kn (%.0f mph)\n", m->gust_kn, m->gust_kn * 1.15078f);
    }
    if (m->fields & AIS_MET_AIR_TEMP) ADD("Air  %.0f\xc2\xb0""F (%.1f\xc2\xb0""C)\n", f_of(m->air_c), m->air_c);
    if (m->fields & AIS_MET_DEW) ADD("Dew point  %.0f\xc2\xb0""F (%.1f\xc2\xb0""C)\n", f_of(m->dew_c), m->dew_c);
    if (m->fields & AIS_MET_HUMIDITY) ADD("Humidity  %u%%\n", m->humidity);
    if (m->fields & AIS_MET_PRESSURE) {
        ADD("Pressure  %.2f inHg (%u hPa)%s\n", m->pressure_hpa * 0.02953f, m->pressure_hpa, trend_str(m->pressure_trend));
    }
    if (m->fields & AIS_MET_VIS) ADD("Visibility  %.1f nm\n", m->vis_nm);
    if (m->fields & AIS_MET_WATER_LEVEL) {
        ADD("Water level  %.2f ft (%.2f m)%s\n", m->water_m * 3.28084f, m->water_m, trend_str(m->water_trend));
    }
    if (m->fields & AIS_MET_CURRENT) {
        ADD("Current  %.1f kn", m->current_kn);
        if (m->current_dir < 360) ADD(" toward %03u\xc2\xb0", m->current_dir);
        ADD("\n");
    }
    const struct { uint32_t f; const char *name; float h; uint8_t s; uint16_t d; } waves[] = {
        {AIS_MET_WAVES, "Waves", m->wave_m, m->wave_s, m->wave_dir},
        {AIS_MET_SWELL, "Swell", m->swell_m, m->swell_s, m->swell_dir},
    };
    for (int i = 0; i < 2; i++) {
        if (!(m->fields & waves[i].f)) continue;
        ADD("%s  %.1f ft (%.1f m)", waves[i].name, waves[i].h * 3.28084f, waves[i].h);
        if (waves[i].s) ADD(", %u s", waves[i].s);
        if (waves[i].d < 360) ADD(", from %03u\xc2\xb0", waves[i].d);
        ADD("\n");
    }
    if (m->fields & AIS_MET_SEA_STATE) ADD("Sea state  Beaufort %u\n", m->sea_state);
    if (m->fields & AIS_MET_WATER_TEMP) ADD("Water  %.0f\xc2\xb0""F (%.1f\xc2\xb0""C)\n", f_of(m->water_c), m->water_c);
    if (m->fields & AIS_MET_SALINITY) ADD("Salinity  %.1f ppt\n", m->salinity);
    if (!n) ADD("No readings yet");
    else buf[n - 1] = '\0'; // no last newline
#undef ADD
    lv_label_set_text(s_body, buf);
}

static void popup_close(void)
{
    if (s_popup) lv_obj_delete(s_popup);
    s_popup = NULL;
    if (s_timer) lv_timer_pause(s_timer);
}

static void x_cb(lv_event_t *e)
{
    popup_close();
}

static void timer_cb(lv_timer_t *t)
{
    met_station_t st;
    if (!s_popup) return;
    if (met_db_get(s_mmsi, s_site, s_lat, s_lon, &st)) fill(&st);
    else popup_close(); // forgotten (no reading for MET_EXPIRY_S)
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

void ui_met_open(const met_station_t *st)
{
    popup_close();
    s_mmsi = st->mmsi;
    s_site = st->m.site;
    s_lat = st->m.lat;
    s_lon = st->m.lon;

    s_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, POPUP_W, LV_SIZE_CONTENT);
    lv_obj_align(s_popup, LV_ALIGN_TOP_MID, 0, POPUP_Y);
    lv_obj_set_scrollable(s_popup, false);
    lv_obj_set_clickable(s_popup, true); // touches on it don't reach the map underneath

    lv_obj_t *box = lv_obj_create(s_popup);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_style_pad_right(box, 64, 0); // room for the X
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_shadow_width(box, 24, 0);
    lv_obj_set_style_shadow_color(box, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_60, 0);
    lv_obj_set_scrollable(box, false);

    s_title = label(box, &lv_font_montserrat_24, UI_COLOR_ACCENT);
    s_sub = label(box, &lv_font_montserrat_16, UI_COLOR_MUTED);
    s_body = label(box, &lv_font_montserrat_20, UI_COLOR_TEXT);
    lv_obj_set_style_text_line_space(s_body, 6, 0);
    lv_obj_set_style_max_height(s_body, TEXT_MAX_H, 0);
    lv_obj_set_scrollable(s_body, true);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);

    lv_obj_t *x = ui_round_button(s_popup, LV_SYMBOL_CLOSE, 52, 52);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_event_cb(x, x_cb, LV_EVENT_CLICKED, NULL);

    fill(st);
    if (!s_timer) s_timer = lv_timer_create(timer_cb, 1000, NULL);
    lv_timer_resume(s_timer);
}
