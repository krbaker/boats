// Host preview of the display UI. Runs the real map / overlay code against
// stubbed device services and a synthetic fleet, and writes each screen as a
// raw RGB565 frame (build.sh turns them into PNGs).

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"

#include "adsb.h"
#include "ais_fwd.h"
#include "log_save.h"
#include "map_pack.h"
#include "map_store.h"
#include "map_tiles.h"
#include "nmea_client.h"
#include "settings.h"
#include "tide.h"
#include "ui.h"
#include "rx_stats.h"
#include "sdr_probe.h"
#include "vessel_db.h"
#include "weather.h"
#include "wifi_mgr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CENTER_LAT 47.6005
#define CENTER_LON -122.3550

// ---------------------------------------------------------------------------
// Stubbed services
// ---------------------------------------------------------------------------

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + 3600LL * 1000000; // > 1 h so "stale" maths works
}

static settings_t s_cfg = {
    .center_lat = CENTER_LAT, .center_lon = CENTER_LON, .zoom_index = 0, .brightness = 100,
    .tide_station = "9447130",
    .filter = {.mode = FILTER_HIDE_LABEL, .order = ORDER_SIZE, .include_unknown = true, .label_name = true,
               .label_speed = true},
};
void settings_get(settings_t *out) { *out = s_cfg; }
esp_err_t settings_set(const settings_t *in) { s_cfg = *in; return ESP_OK; }
EventGroupHandle_t settings_subscribe(void) { return NULL; }
void settings_notify_map_changed(void) {}


void nmea_client_get_status(nmea_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = NMEA_STATE_CONNECTED;
    static const uint32_t net_types[28] = {[1] = 402, [2] = 11, [3] = 160, [4] = 55, [5] = 48, [8] = 17, [9] = 2,
                                           [18] = 71, [19] = 4, [20] = 6, [21] = 28, [24] = 12, [27] = 1};
    memcpy(out->stats.by_type, net_types, sizeof(net_types));
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = WIFI_MGR_STA_CONNECTED;
    strcpy(out->sta_ip, "192.168.1.50");
}

// Tide: a semidiurnal tide with a diurnal inequality, roughly Seattle-sized.
static float tide_height(time_t t)
{
    double h = t / 3600.0;
    return (float)(6.5 + 4.5 * cos(2 * M_PI * h / 12.42) + 2.0 * cos(2 * M_PI * h / 24.84 + 1.0));
}

bool clock_is_set(void) { return true; }

// Simulated weather: set_weather() picks a scenario.
static weather_data_t s_wx;
static uint32_t s_wx_gen = 1;
void weather_get(weather_data_t *out) { *out = s_wx; }
uint32_t weather_generation(void) { return s_wx_gen; }
const char *wx_state_str(wx_state_t s) { return s == WX_OK ? "ok" : "?"; }

// A file from test/host/fixtures/weather/ (the preview runs in preview/out/).
static char *fixture(const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), "../../fixtures/weather/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[64 * 1024];
    buf[fread(buf, 1, sizeof(buf) - 1, f)] = '\0';
    fclose(f);
    return buf;
}

static void set_weather(int scenario)
{
    time_t now = time(NULL);
    memset(&s_wx, 0, sizeof(s_wx));
    // The overlay's text comes from the real saved responses: the Potomac
    // forecast and its Small Craft Advisory, and buoy 46088 (times moved to now).
    char *t = fixture("nws_marine_ANZ535.txt");
    s_wx.have_forecast = t && mf_parse(t, &s_wx.forecast);
    t = fixture("nws_alerts_marine.json");
    s_wx.nalerts = t ? wx_parse_alerts(t, s_wx.alerts, WX_MAX_ALERTS) : 0;
    static time_t alert_ends; // the same in every scene, or each would be a "new" alert and pop up again
    if (!alert_ends) alert_ends = now + 3 * 3600;
    if (s_wx.nalerts > 0) s_wx.alerts[0].ends = alert_ends;
    t = fixture("ndbc_46088.txt");
    buoy_t real;
    if (t && buoy_parse(t, &real)) {
        time_t shift = now - 1200 - real.latest;
        buoy_val_t *v = &real.wave_ft;
        for (buoy_val_t *end = &real.dew_f; v <= end; v++) v->when += shift;
        real.latest += shift;
        s_wx.buoy = real;
    }
    snprintf(s_wx.buoy_id, sizeof(s_wx.buoy_id), "46088");
    snprintf(s_wx.station_name, sizeof(s_wx.station_name), "Home (Live)");
    snprintf(s_wx.obs_station, sizeof(s_wx.obs_station), "KBVY");
    s_wx.forecast_src.state = s_wx.buoy_src.state = s_wx.station_src.state = s_wx.obs_src.state = WX_OK;
    s_wx.have_station = s_wx.have_buoy = s_wx.have_obs = true;
    s_wx.alerts_src.state = WX_OK;
    wl_current_t *c = &s_wx.station;
    buoy_t *b = &s_wx.buoy;
    *c = (wl_current_t){.when = now - 60, .has_wind = true, .wind_mph = 9, .has_temp = true, .temp_f = 57.3f,
                        .has_hum = true, .hum_pct = 51, .has_dew = true, .dew_f = 39.4f, .has_rain = true,
                        .rain_today_in = 0.04f, .has_bar = true, .bar_inhg = 29.90f, .bar_trend_inhg = 0.08f};
    b->latest = now - 1200;
    s_wx.obs = (wx_obs_t){.when = now - 1800};
    snprintf(s_wx.obs.text, sizeof(s_wx.obs.text), "Partly Cloudy");
    if (scenario == 0) { // a breezy afternoon: wind from the NW with gusts, a swell from the SW, an advisory
        c->wind_avg10_mph = 14, c->gust10_mph = 26, c->wind_dir_avg10 = 315;
        b->wave_ft = (buoy_val_t){true, 2.3f, now - 1200};
        b->wave_dir = (buoy_val_t){true, 225, now - 1200};
        b->dom_period_s = (buoy_val_t){true, 8, now - 1200};
        s_wx.obs.cloud = CLOUD_SCATTERED;
        s_wx.alert_flag = true;
    } else if (scenario == 1) { // rain, wind from due north (behind the time bubble), waves from the east
        c->wind_avg10_mph = 8, c->gust10_mph = 11, c->wind_dir_avg10 = 2;
        c->rain_rate_in_hr = 0.08f;
        b->wave_ft = (buoy_val_t){true, 1.2f, now - 1200};
        b->wave_dir = (buoy_val_t){true, 95, now - 1200};
        b->dom_period_s = (buoy_val_t){true, 5, now - 1200};
        s_wx.obs.cloud = CLOUD_OVERCAST;
        s_wx.obs.precip = PRECIP_RAIN;
    } else if (scenario == 3) { // a buoy that measures no wave direction (like 44029): waves in the tide bubble
        c->wind_avg10_mph = 11, c->gust10_mph = 15, c->wind_dir_avg10 = 60;
        b->wave_ft = (buoy_val_t){true, 2.95f, now - 1200};
        b->dom_period_s = (buoy_val_t){true, 9, now - 1200};
        b->wave_dir.ok = false;
        s_wx.obs.cloud = CLOUD_CLEAR;
    } else if (scenario == 4) { // wind and waves from nearly the same way: the arrows overlap
        c->wind_avg10_mph = 14, c->gust10_mph = 26, c->wind_dir_avg10 = 300;
        b->wave_ft = (buoy_val_t){true, 2.3f, now - 1200};
        b->wave_dir = (buoy_val_t){true, 290, now - 1200};
        b->dom_period_s = (buoy_val_t){true, 8, now - 1200};
        s_wx.obs.cloud = CLOUD_CLEAR;
    } else { // calm, waves from the south (behind the tide bubble)
        c->wind_avg10_mph = 0.4f, c->gust10_mph = 2, c->wind_dir_avg10 = 200;
        b->wave_ft = (buoy_val_t){true, 0.7f, now - 1200};
        b->wave_dir = (buoy_val_t){true, 180, now - 1200};
        b->dom_period_s = (buoy_val_t){true, 3, now - 1200};
        s_wx.obs.cloud = CLOUD_BROKEN;
    }
    s_wx_gen++;
}

void tide_get_status(tide_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = TIDE_OK;
    strcpy(out->station, "9447130");
    strcpy(out->station_name, "Seattle");
}

int tide_curve(tide_sample_t *out, int max)
{
    time_t start = time(NULL) / 1800 * 1800 - 7 * 3600;
    int n = 0;
    for (; n < max && n < 112; n++) {
        out[n].t = start + n * 1800;
        out[n].height_ft = tide_height(out[n].t);
    }
    return n;
}

int tide_events(tide_event_t *out, int max)
{
    // Turning points of the synthetic curve, found by scanning minute by minute.
    time_t t = time(NULL) - 12 * 3600;
    int n = 0;
    for (int i = 1; i < 60 * 72 && n < max; i++) {
        float a = tide_height(t + (i - 1) * 60), b = tide_height(t + i * 60), c = tide_height(t + (i + 1) * 60);
        if ((b > a && b >= c) || (b < a && b <= c)) {
            out[n++] = (tide_event_t){.t = t + i * 60, .height_ft = b, .high = b > a};
        }
    }
    return n;
}

bool tide_level(time_t t, float *h)
{
    *h = tide_height(t);
    return true;
}

bool tide_next(time_t now, tide_event_t *out)
{
    tide_event_t ev[32];
    int n = tide_events(ev, 32);
    for (int i = 0; i < n; i++) {
        if (ev[i].t > now) {
            *out = ev[i];
            return true;
        }
    }
    return false;
}

// Chart tiles: read synchronously from the real packs (out/sd/ais_map), using
// the firmware's own pack reader and PNG decoder.
typedef struct {
    int level, col, row;
    bool blank;
    lv_image_dsc_t dsc;
} ptile_t;

static map_manifest_t s_manifest;
static bool s_have_chart;
static ptile_t s_tiles[1024];
static int s_ntiles;
static map_pack_t s_pack;
static int s_pack_level = -1;

void map_tiles_reload(void) {}
bool map_tiles_poll(void) { return false; }
map_tiles_state_t map_tiles_state(void) { return s_have_chart ? MAP_TILES_READY : MAP_TILES_NONE; }
const map_manifest_t *map_tiles_manifest(void) { return s_have_chart ? &s_manifest : NULL; }
void map_tiles_frame_begin(void) {}
void map_tiles_frame_end(void) {}
void map_tiles_prefetch(int level, int col, int row) {}

const lv_image_dsc_t *map_tiles_get(int level, int col, int row)
{
    for (int i = 0; i < s_ntiles; i++) {
        ptile_t *t = &s_tiles[i];
        if (t->level == level && t->col == col && t->row == row) return t->blank ? NULL : &t->dsc;
    }
    if (s_ntiles == 1024) return NULL;
    if (s_pack_level != level) {
        char path[96], err[96];
        map_pack_close(&s_pack);
        snprintf(path, sizeof(path), "%s/%s", MAP_DIR, s_manifest.levels[level].file);
        if (!map_pack_open(&s_pack, path, err, sizeof(err))) {
            fprintf(stderr, "%s\n", err);
            return NULL;
        }
        s_pack_level = level;
    }
    ptile_t *t = &s_tiles[s_ntiles++];
    *t = (ptile_t){.level = level, .col = col, .row = row, .blank = true};
    size_t len;
    uint8_t *png = map_pack_read(&s_pack, col, row, &len);
    if (png) t->blank = !map_tile_decode(png, len, &t->dsc);
    free(png);
    return t->blank ? NULL : &t->dsc;
}

// Touch: a scripted finger.
static struct {
    bool down;
    int32_t x, y;
} s_touch;
static lv_indev_t *s_indev;

lv_indev_t *bsp_display_get_input_dev(void) { return s_indev; }

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = s_touch.x;
    data->point.y = s_touch.y;
    data->state = s_touch.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Fleet
typedef struct {
    const char *name;
    float dn, de;       // nm north / east of the centre
    float sog, cog;
    int heading;        // -1 = not available
    int len_m, beam_m;  // 0 = not reported
    uint8_t type;
    int age_s;
} fake_t;

static const fake_t k_fleet[] = {
    {"WENATCHEE", -0.55f, 0.35f, 17.5f, 290, 288, 140, 27, 60, 10},       // ferry, crabbing in the current
    {"TACOMA", 0.80f, -0.95f, 0, 0, 120, 140, 27, 60, 30},                // ferry at the dock
    {"EVER LOVING", -1.20f, -1.10f, 9.8f, 345, 343, 334, 46, 70, 5},      // container ship
    {"POLAR ALASKA", 0.40f, -1.45f, 11.2f, 160, 158, 272, 42, 80, 20},    // tanker
    {"JAMES T QUIGG", -0.30f, -0.20f, 6.1f, 75, 70, 30, 10, 52, 40},      // tug
    {"SEA WOLF", 0.15f, 0.20f, 5.5f, 210, -1, 12, 4, 36, 15},             // sailboat, Class B
    {"ORCA SPIRIT", 0.18f, 0.27f, 22.0f, 30, 32, 20, 6, 60, 15},          // whale-watch, next to SEA WOLF
    {"WAVE DANCER", 0.21f, 0.14f, 0.2f, 0, -1, 0, 0, 37, 50},             // pleasure, no size, near the others
    {"", -0.90f, 0.90f, 3.0f, 120, -1, 0, 0, 0, 25},                      // unnamed, unknown everything
    {"NORTHERN JAEGER", 1.30f, 0.30f, 0.0f, 0, 255, 104, 18, 30, 400},    // fishing, stale
    {"PILOT 2", -1.55f, 0.55f, 14.0f, 315, 312, 18, 5, 50, 8},
    {"KITTIWAKE", 1.10f, -0.20f, 7.5f, 200, -1, 10, 3, 37, 12},
};
// Aids to navigation, one of each look in two rows, and a SAR aircraft.
typedef struct {
    const char *name;
    float dn, de;
    uint8_t type, flags;
} fake_mark_t;

static const fake_mark_t k_marks[] = {
    {"N CARD", 0.80f, -0.95f, 20, 0},
    {"E CARD", 0.80f, -0.76f, 21, 0},
    {"S CARD", 0.80f, -0.57f, 22, 0},
    {"W CARD", 0.80f, -0.38f, 23, 0},
    {"PORT", 0.80f, -0.19f, 24, 0},
    {"STBD", 0.80f, 0.00f, 25, 0},
    {"PREF P", 0.58f, -0.95f, 26, 0},
    {"PREF S", 0.58f, -0.76f, 27, 0},
    {"ISO DGR", 0.58f, -0.57f, 28, 0},
    {"SAFE WTR", 0.58f, -0.38f, 29, 0},
    {"SPECIAL", 0.58f, -0.19f, 30, 0},
    {"LIGHT", 0.58f, 0.00f, 5, 0},
    {"RACON", 0.36f, -0.95f, 2, 0},
    {"STRUCT", 0.36f, -0.76f, 3, 0},
    {"AtoN", 0.36f, -0.57f, 0, 0},
    {"VIRTUAL", 0.36f, -0.38f, 24, AIS_ATON_VIRTUAL},
    {"OFF POS", 0.36f, -0.19f, 25, AIS_ATON_OFF_POS},
};
#define MARKS_N (int)(sizeof(k_marks) / sizeof(k_marks[0]))
#define VESSELS_N (int)(sizeof(k_fleet) / sizeof(k_fleet[0]))
#define FLEET_N (VESSELS_N + MARKS_N + 2)   // + a SAR aircraft and an AIS-SART

static void make_mark(int i, vessel_t *v)
{
    const fake_mark_t *m = &k_marks[i];
    memset(v, 0, sizeof(*v));
    v->mmsi = 993660000 + i;
    strcpy(v->name, m->name);
    v->cls = AIS_CLASS_ATON;
    v->lat = CENTER_LAT + m->dn / 60.0;
    v->lon = CENTER_LON + m->de / 60.0 / cos(CENTER_LAT * M_PI / 180);
    v->fields = AIS_F_POS | AIS_F_NAME | AIS_F_ATON;
    v->aton_type = m->type;
    v->aton_flags = m->flags;
    v->last_heard_us = esp_timer_get_time() - 60 * 1000000LL;
}

static void make_aircraft(vessel_t *v)
{
    memset(v, 0, sizeof(*v));
    v->mmsi = 111366101;
    v->cls = AIS_CLASS_SAR;
    v->lat = CENTER_LAT - 0.05 / 60.0;
    v->lon = CENTER_LON + 0.95 / 60.0 / cos(CENTER_LAT * M_PI / 180);
    v->fields = AIS_F_POS | AIS_F_SOG | AIS_F_COG | AIS_F_ALT;
    v->sog_kn = 120;
    v->cog_deg = 250;
    v->alt_m = 300;
    v->last_heard_us = esp_timer_get_time() - 5 * 1000000LL;
}

// An AIS-SART going off, south-east of the centre.
static void make_sart(vessel_t *v)
{
    memset(v, 0, sizeof(*v));
    v->mmsi = 970111222;
    v->lat = CENTER_LAT - 0.10 / 60.0;
    v->lon = CENTER_LON + 0.12 / 60.0 / cos(CENTER_LAT * M_PI / 180);
    v->fields = AIS_F_POS | AIS_F_NAVSTAT;
    v->nav_status = 14;
    v->cls = AIS_CLASS_A;
    v->last_heard_us = esp_timer_get_time() - 20 * 1000000LL;
}

// Safety messages: the SART's, a shore notice, a man-overboard device's test.
int vessel_db_safety_msgs(safety_msg_t *out, int max)
{
    int64_t now = esp_timer_get_time();
    static const struct { uint32_t mmsi; const char *text; ais_safety_t kind; uint32_t count; int age_s; } k[] = {
        {970111222, "SART ACTIVE", AIS_SAFETY_DISTRESS, 7, 20},
        {3669702, "BUOY 12 OFF STATION. MARINERS USE CAUTION", AIS_SAFETY_TEXT, 1, 300},
        {972000123, "MOB TEST", AIS_SAFETY_TEST, 2, 900},
        {974010086, "T4F842D5A60FFBFF", AIS_SAFETY_TEXT, 3, 1200},
    };
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++, n++) {
        out[n] = (safety_msg_t){.mmsi = k[i].mmsi, .kind = k[i].kind, .count = k[i].count,
                                .last_us = now - k[i].age_s * 1000000LL, .first_us = now - 3600 * 1000000LL,
                                .last_wall = time(NULL) - k[i].age_s};
        strcpy(out[n].text, k[i].text);
    }
    return n;
}
uint32_t vessel_db_safety_generation(void) { return 1; }

// Weather stations heard over AIS: calm, 15 kn, and 65 kn (a pennant), near the centre.
static met_station_t k_met[3];
static int met_stations(void)
{
    static const struct { const char *name; float dn, de, kn, gust; int dir; } k[] = {
        {"ALKI POINT", 0.10f, -0.20f, 15, 22, 315}, {"", -0.12f, -0.25f, 1, 0, 0}, {"STORM BUOY", 0.18f, 0.20f, 65, 80, 200},
    };
    for (int i = 0; i < 3; i++) {
        met_station_t *st = &k_met[i];
        memset(st, 0, sizeof(*st));
        st->mmsi = 3669990 + i;
        st->m.site = i == 1 ? AIS_MET_SITE_IMO : 5 + i;
        st->m.lat = CENTER_LAT + k[i].dn / 60.0;
        st->m.lon = CENTER_LON + k[i].de / 60.0 / cos(CENTER_LAT * M_PI / 180);
        st->m.wind_kn = k[i].kn;
        st->m.gust_kn = k[i].gust;
        st->m.wind_dir = (uint16_t)k[i].dir;
        st->m.fields = AIS_MET_POS | AIS_MET_WIND | AIS_MET_GUST | AIS_MET_TIME;
        if (k[i].name[0]) {
            strcpy(st->m.name, k[i].name);
            st->m.fields |= AIS_MET_NAME;
        }
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        st->m.day = tm.tm_mday, st->m.hour = tm.tm_hour, st->m.minute = tm.tm_min;
        if (i == 0) {
            st->m.air_c = 11.5f, st->m.humidity = 82, st->m.pressure_hpa = 1012, st->m.pressure_trend = AIS_TREND_FALLING;
            st->m.water_m = 2.31f, st->m.water_trend = AIS_TREND_RISING, st->m.water_c = 10.8f, st->m.vis_nm = 6.5f;
            st->m.fields |= AIS_MET_AIR_TEMP | AIS_MET_HUMIDITY | AIS_MET_PRESSURE | AIS_MET_WATER_LEVEL |
                            AIS_MET_WATER_TEMP | AIS_MET_VIS;
        }
        st->last_us = esp_timer_get_time() - 120 * 1000000LL;
    }
    return 3;
}
int met_db_snapshot(met_station_t *out, int max)
{
    int n = met_stations();
    memcpy(out, k_met, n * sizeof(*out));
    return n;
}
bool met_db_get(uint32_t mmsi, uint8_t site, double lat, double lon, met_station_t *out)
{
    met_stations();
    for (int i = 0; i < 3; i++) {
        if (k_met[i].mmsi == mmsi) {
            *out = k_met[i];
            return true;
        }
    }
    return false;
}

static void make_vessel(int i, vessel_t *v)
{
    if (i == VESSELS_N + MARKS_N + 1) {
        make_sart(v);
        return;
    }
    if (i >= VESSELS_N + MARKS_N) {
        make_aircraft(v);
        return;
    }
    if (i >= VESSELS_N) {
        make_mark(i - VESSELS_N, v);
        return;
    }
    const fake_t *f = &k_fleet[i];
    memset(v, 0, sizeof(*v));
    static const uint16_t mids[] = {366, 636, 366, 538, 351, 366, 316, 232, 366, 477};
    v->mmsi = mids[i % 10] * 1000000u + 100 + i * 7; // a few flags besides the US
    strcpy(v->name, f->name);
    v->cls = f->heading < 0 ? AIS_CLASS_B : AIS_CLASS_A;
    v->lat = CENTER_LAT + f->dn / 60.0;
    v->lon = CENTER_LON + f->de / 60.0 / cos(CENTER_LAT * M_PI / 180);
    v->fields = AIS_F_POS | AIS_F_SOG | AIS_F_COG;
    if (i == 8) v->fields = AIS_F_POS;
    v->sog_kn = f->sog;
    v->cog_deg = f->cog;
    if (f->heading >= 0) {
        v->heading = (uint16_t)f->heading;
        v->fields |= AIS_F_HEADING;
    }
    if (f->len_m) {
        v->dim_a = (uint16_t)(f->len_m * 2 / 3);
        v->dim_b = (uint16_t)(f->len_m - v->dim_a);
        v->dim_c = v->dim_d = (uint8_t)(f->beam_m / 2);
        v->fields |= AIS_F_DIM;
    }
    v->ship_type = f->type;
    if (f->type) v->fields |= AIS_F_SHIPTYPE;
    v->last_heard_us = esp_timer_get_time() - (int64_t)f->age_s * 1000000;
}

int vessel_db_snapshot(vessel_t *out, int max)
{
    int n = FLEET_N < max ? FLEET_N : max;
    for (int i = 0; i < n; i++) make_vessel(i, &out[i]);
    return n;
}

bool vessel_db_get(uint32_t mmsi, vessel_t *out)
{
    for (int i = 0; i < FLEET_N; i++) {
        make_vessel(i, out);
        if (out->mmsi == mmsi) return true;
    }
    return false;
}

int vessel_db_count(void) { return FLEET_N; }
uint32_t vessel_db_generation(void) { return 1; }

void vessel_db_source_counts(int *net_only, int *sdr_only, int *both)
{
    *net_only = 9;
    *sdr_only = 1;
    *both = 7;
}

// The RTL-SDR receiver, decoding.
void sdr_get_status(sdr_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = SDR_READY;
    strcpy(out->tuner, "R820T (RTL-SDR Blog V3)");
    out->rate = 288000;
    out->cpu_pct = 23.4f;
    out->demod.noise_dbfs[0] = out->demod.noise_dbfs[1] = -38.6f;
    out->demod.ppm = -2.2f;
    out->gain = 372;
    out->gain_best = true;
    static const uint32_t sdr_types[28] = {[1] = 61, [3] = 22, [4] = 9, [5] = 6, [8] = 2, [18] = 14, [21] = 3, [24] = 1};
    memcpy(out->parser.by_type, sdr_types, sizeof(sdr_types));
}

const char *sdr_state_str(sdr_state_t st) { return st == SDR_READY ? "receiving" : "?"; }

void rx_stats_get(rx_stats_t *out)
{
    *out = (rx_stats_t){.total = {{412, 398}, {61, 57}}, .last_min = {{14, 12}, {3, 4}}};
}

// Screens we don't preview.
lv_obj_t *ui_detail_create(void) { return lv_obj_create(NULL); }
void ui_detail_set_vessel(uint32_t mmsi) {}
// The Network / settings screen's services.
void wifi_mgr_ap(bool on) {}
void wifi_mgr_new_ap_password(void) {}
void log_save_start(void) {}
log_save_state_t log_save_state(char *msg, size_t len) { if (msg) msg[0] = '\0'; return LOG_SAVE_IDLE; }
void ais_fwd_get_status(int dest, fwd_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = dest == 0 ? FWD_STATE_SENDING : FWD_STATE_OFF;
}
const char *ais_fwd_state_str(fwd_state_t st) { return st == FWD_STATE_SENDING ? "sending" : "off"; }
void vessel_db_save(void) {}
void vessel_db_forget_all(void) {}
bool sdr_capture_start(int seconds, char *err, size_t errlen) { return true; }
const char *nmea_state_str(nmea_state_t st) { return "connected"; }

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Aircraft from dump1090: one of each kind on a ring round the centre, one
// squawking 7700, one not heard for a while. Shown once s_cfg.adsb_enabled is set.
static const struct {
    const char *flight;
    uint8_t category;
    int alt_ft, gs;
    uint16_t squawk;
} k_planes[] = {
    {"N172SP", 0xA1, 1500, 105, 1200}, {"EJA512", 0xA2, 9000, 280, 4521}, {"ASA123", 0xA3, 4800, 230, 4433},
    {"UPS2912", 0xA4, 6200, 250, 3341}, {"BAW49", 0xA5, 11000, 310, 2216}, {"VV100", 0xA6, 3000, 420, 0},
    {"N911KC", 0xA7, 800, 90, 1200}, {"N4GL", 0xB1, 3500, 55, 0}, {"", 0xB2, 2000, 8, 0},
    {"", 0xB4, 600, 25, 1200}, {"", 0xB6, 300, 20, 0}, {"", 0, 5500, 160, 0},
    {"QXE2204", 0xA3, 3100, 190, 7700},
};
#define PLANES_N (int)(sizeof(k_planes) / sizeof(k_planes[0]))

static void make_plane(int i, adsb_target_t *t)
{
    memset(t, 0, sizeof(*t));
    adsb_aircraft_t *a = &t->a;
    a->addr = 0xA00000 + i * 0x1111;
    strcpy(a->flight, k_planes[i].flight);
    if (i == 2) strcpy(a->type, "B739"), strcpy(a->reg, "N123AS");
    a->category = k_planes[i].category;
    bool emerg = i == PLANES_N - 1;
    float ang = (emerg ? 250 : i * 360.0f / PLANES_N) * (float)M_PI / 180, r = emerg ? 0.75f : 1.35f;
    a->lat = CENTER_LAT + r * cosf(ang) / 60.0;
    a->lon = CENTER_LON + r * sinf(ang) / 60.0 / cos(CENTER_LAT * M_PI / 180);
    a->alt_ft = k_planes[i].alt_ft;
    a->gs_kn = k_planes[i].gs;
    a->track_deg = fmodf(i * 360.0f / PLANES_N + 90 + i * 17, 360);
    a->vrate_fpm = i == PLANES_N - 1 ? -1800 : 0;
    a->squawk = k_planes[i].squawk;
    a->fields = ADSB_F_POS | ADSB_F_ALT | ADSB_F_GS | ADSB_F_TRACK | ADSB_F_VRATE | (a->squawk ? ADSB_F_SQUAWK : 0);
    if (i == PLANES_N - 1) a->emergency = ADSB_EMERG_GENERAL;
    t->id = adsb_id(a);
    t->dist_nm = r;
    (void)emerg;
    t->pos_us = esp_timer_get_time() - (i == 11 ? 40 : 1) * 1000000LL; // the plain one: stale
}

int adsb_snapshot(adsb_target_t *out, int max)
{
    if (!s_cfg.adsb_enabled) return 0;
    int n = PLANES_N < max ? PLANES_N : max;
    for (int i = 0; i < n; i++) make_plane(PLANES_N - 1 - i, &out[i]); // nearest (the emergency) first
    return n;
}

bool adsb_get(uint32_t id, adsb_target_t *out)
{
    for (int i = 0; i < PLANES_N; i++) {
        make_plane(i, out);
        if (out->id == id) return true;
    }
    return false;
}

uint32_t adsb_generation(void) { return s_cfg.adsb_enabled; }

void adsb_get_status(adsb_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_cfg.adsb_enabled ? ADSB_OK : ADSB_OFF;
    out->heard = s_cfg.adsb_enabled ? 41 : 0;
    out->shown = s_cfg.adsb_enabled ? PLANES_N : 0;
}

const char *adsb_state_str(adsb_state_t st) { return st == ADSB_OK ? "connected" : "off"; }

static uint16_t s_fb[800 * 800];

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    lv_display_flush_ready(d);
}

static void run_ms(int ms)
{
    uint32_t end = tick_ms() + ms;
    while (tick_ms() < end) {
        lv_timer_handler();
        usleep(5000);
    }
    lv_refr_now(NULL);
}

static void snap(const char *name)
{
    run_ms(300);
    char path[64];
    snprintf(path, sizeof(path), "%s.raw", name);
    FILE *f = fopen(path, "wb");
    fwrite(s_fb, 2, 800 * 800, f);
    fclose(f);
}

static void tap(int32_t x, int32_t y)
{
    s_touch = (typeof(s_touch)){true, x, y};
    run_ms(80);
    s_touch.down = false;
    run_ms(80);
}

static void drag(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    s_touch = (typeof(s_touch)){true, x0, y0};
    run_ms(60);
    for (int i = 1; i <= 10; i++) {
        s_touch.x = x0 + (x1 - x0) * i / 10;
        s_touch.y = y0 + (y1 - y0) * i / 10;
        run_ms(40);
    }
    s_touch.down = false;
    run_ms(80);
}

// Point on a pill at the given angle (0 = 3 o'clock, clockwise).
static void tap_edge(float deg)
{
    tap((int32_t)(UI_CENTER + 372 * cosf(deg * (float)M_PI / 180)), (int32_t)(UI_CENTER + 372 * sinf(deg * (float)M_PI / 180)));
}

static lv_obj_t *find_class(lv_obj_t *root, const lv_obj_class_t *cls)
{
    if (lv_obj_check_type(root, cls)) return root;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *r = find_class(lv_obj_get_child(root, i), cls);
        if (r) return r;
    }
    return NULL;
}

static void hide_overlays(lv_obj_t *scr)
{
    // Overlays are the round full-screen children with a close button; hide any open ones.
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        if (lv_obj_get_width(c) == UI_SIZE && lv_obj_get_child_count(c) >= 2 && lv_obj_get_style_bg_opa(c, 0) == LV_OPA_90)
            lv_obj_set_hidden(c, true);
    }
}

int main(int argc, char **argv)
{
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    map_store_mount(); // "sd" (build.sh puts the flags there, if FLAGS is set)
    s_have_chart = map_store_get_manifest(&s_manifest);
    if (!s_have_chart) fprintf(stderr, "no chart set in %s; previewing without one\n", MAP_DIR);

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = lv_display_create(800, 800);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_fb, NULL, sizeof(s_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);
    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touch_read_cb);

    s_cfg.zoom_index = 2;
    set_weather(0);
    lv_obj_t *scr = ui_map_create();
    lv_screen_load(scr);
    ui_map_on_show();
    run_ms(1300); // let the 1 s refresh timer fill in the border text
    snap("alert_popup"); // the advisory is new: it pops up over the map
    tap(800 / 2 + 520 / 2 - 8 - 26, 150 + 8 + 26); // its X
    snap("map");

    // Zoom in twice with the "+" pill, then drag the chart up and to the left.
    tap_edge(-21);
    tap_edge(-21);
    snap("map_zoomed");
    drag(500, 500, 250, 300);
    snap("map_panned");
    tap_edge(21);  // zoom back out
    tap_edge(21);

    tap_edge(180); // vessel count -> Vessels overlay
    snap("vessels");
    lv_tabview_set_active(find_class(scr, &lv_tabview_class), 1, LV_ANIM_OFF);
    lv_obj_send_event(find_class(scr, &lv_tabview_class), LV_EVENT_VALUE_CHANGED, NULL);
    snap("receiver");
    drag(400, 640, 400, 260); // down to the message types
    run_ms(600);
    snap("receiver_types");
    lv_tabview_set_active(find_class(scr, &lv_tabview_class), 2, LV_ANIM_OFF);
    snap("options");
    hide_overlays(scr);

    tap_edge(90);  // tide
    snap("tide");
    hide_overlays(scr);

    tap_edge(270); // the time -> weather overlay
    snap("weather_forecast");
    lv_obj_t *wtabs = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr) && !wtabs; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        if (!lv_obj_is_hidden(c) && lv_obj_get_width(c) == UI_SIZE) wtabs = find_class(c, &lv_tabview_class);
    }
    tap(400, 250); // the advisory box: expand it
    snap("weather_alert_open");
    lv_tabview_set_active(wtabs, 1, LV_ANIM_OFF);
    snap("weather_waves");
    lv_tabview_set_active(wtabs, 2, LV_ANIM_OFF);
    snap("weather_weather");
    hide_overlays(scr);

    // Filters: hide small / slow vessels completely, speed priority, names only.
    s_cfg.filter = (vessel_filter_t){.min_len_ft = 100, .min_speed_kn = 5, .mode = FILTER_HIDE_VESSEL,
                                     .order = ORDER_SPEED, .include_unknown = false, .label_name = true};
    ui_map_set_filter(&s_cfg.filter);
    snap("map_filtered");

    // Zoomed all the way out.
    for (int i = 0; i < 6; i++) tap_edge(21);
    snap("map_widest");

    // Weather on the map in other conditions (the 1 s refresh picks them up).
    s_cfg.filter = (vessel_filter_t){.mode = FILTER_HIDE_LABEL, .order = ORDER_SIZE, .include_unknown = true,
                                     .label_name = true, .label_speed = true};
    ui_map_set_filter(&s_cfg.filter);
    for (int i = 0; i < 4; i++) tap_edge(-21);
    set_weather(1);
    run_ms(1100);
    snap("map_weather_rain");
    set_weather(2);
    run_ms(1100);
    snap("map_weather_calm");
    set_weather(3);
    run_ms(1100);
    snap("map_weather_no_wave_dir");
    set_weather(4);
    run_ms(1100);
    snap("map_weather_overlap");

    // The Network screen and its settings tabs.
    lv_obj_t *net = ui_netinfo_create();
    lv_screen_load(net);
    ui_netinfo_on_show();
    run_ms(1100);
    lv_obj_t *net_tabs = find_class(lv_screen_active(), &lv_tabview_class);
    static const char *const net_names[] = {"net_network", "net_ais", "net_retransmit", "net_display", "net_system",
                                            "net_adsb"};
    for (int i = 0; i < 6; i++) {
        lv_tabview_set_active(net_tabs, i, LV_ANIM_OFF);
        run_ms(1100);
        snap(net_names[i]);
    }

    // Safety messages: the Messages tab, then the distress popup over the map.
    lv_screen_load(scr);
    ui_map_on_show();
    run_ms(1100);
    snap("map_sart");
    tap_edge(180); // vessel count -> Vessels overlay
    lv_obj_t *vtabs = find_class(scr, &lv_tabview_class);
    lv_tabview_set_active(vtabs, 3, LV_ANIM_OFF);
    lv_obj_send_event(vtabs, LV_EVENT_VALUE_CHANGED, NULL);
    run_ms(1100);
    snap("vessels_messages");
    hide_overlays(scr);
    ui_safety_init();
    run_ms(1200);
    snap("safety_popup");
    // Weather stations, and one's readings.
    lv_obj_clean(lv_layer_top());
    run_ms(1100);
    snap("map_met");
    met_stations();
    ui_met_open(&k_met[0]);
    run_ms(1100);
    snap("met_popup");
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_layer_top()); i++) lv_obj_set_hidden(lv_obj_get_child(lv_layer_top(), i), true);

    // Aircraft from dump1090: the map, the status bubble's choice, the list and one aircraft.
    s_cfg.adsb_enabled = true;
    strcpy(s_cfg.adsb_url, "http://192.168.1.60:8080/data/aircraft.json");
    s_cfg.adsb_range_nm = 10;
    lv_screen_load(scr);
    ui_map_on_show();
    tap_edge(21);  // out to 2 nm
    tap_edge(21);
    run_ms(1100);
    snap("map_aircraft");
    tap_edge(180);
    snap("status_chooser");
    tap(400, 400 + 44); // "Aircraft", the lower button
    snap("aircraft_list");
    hide_overlays(scr);
    lv_obj_t *acs = ui_aircraft_detail_create(); // (ui_start makes it on the display)
    lv_screen_load(acs);
    ui_aircraft_detail_set(adsb_id(&(adsb_aircraft_t){.addr = 0xA00000 + (PLANES_N - 1) * 0x1111})); // the emergency
    run_ms(1100);
    snap("aircraft_detail");
    ui_aircraft_detail_set(adsb_id(&(adsb_aircraft_t){.addr = 0xA00000 + 2 * 0x1111}));
    run_ms(1100);
    snap("aircraft_detail_airliner");
    lv_screen_load(net);
    ui_netinfo_on_show();
    lv_tabview_set_active(net_tabs, 5, LV_ANIM_OFF);
    run_ms(1100);
    snap("net_adsb");
    return 0;
}
