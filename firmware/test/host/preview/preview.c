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
    if (s_wx.nalerts > 0) s_wx.alerts[0].ends = now + 3 * 3600;
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
#define FLEET_N (int)(sizeof(k_fleet) / sizeof(k_fleet[0]))

static void make_vessel(int i, vessel_t *v)
{
    const fake_t *f = &k_fleet[i];
    memset(v, 0, sizeof(*v));
    v->mmsi = 366000100 + i * 7;
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
}

const char *sdr_state_str(sdr_state_t st) { return st == SDR_READY ? "receiving" : "?"; }

void rx_stats_get(rx_stats_t *out)
{
    *out = (rx_stats_t){.total = {{412, 398}, {61, 57}}, .last_min = {{14, 12}, {3, 4}}};
}

// Screens we don't preview.
lv_obj_t *ui_detail_create(void) { return lv_obj_create(NULL); }
void ui_detail_set_vessel(uint32_t mmsi) {}
lv_obj_t *ui_netinfo_create(void) { return lv_obj_create(NULL); }
void ui_netinfo_on_show(void) {}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

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
    return 0;
}
