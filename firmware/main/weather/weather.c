// Weather fetching: see weather.h and WEATHER_PLAN.md.
//
// One task fetches every source in turn, each on its own timer. Responses are
// parsed into scratch copies and swapped into s_data under the lock, so a
// reader always sees complete data. A failed fetch keeps the last good data
// and marks the source as in error.

#include "weather.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "settings.h"
#include "tide.h"
#include "weatherlink.h"
#include "wifi_mgr.h"

static const char *TAG = "weather";

#define RESP_MAX            (64 * 1024)
#define USER_AGENT          "ais-display/1.0 (marine weather display)" // NWS asks every client to identify itself
#define RETRY_S             (5 * 60)

#define FORECAST_EVERY_S    (30 * 60)
#define ALERTS_EVERY_S      (5 * 60)
#define BUOY_EVERY_S        (10 * 60)
#define STATION_EVERY_S     60
#define OBS_EVERY_S         (15 * 60)

static weather_data_t *s_data;          // what readers see
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_generation;
static EventGroupHandle_t s_events;

// Fetch-task state: when each source is next due, and lookups it caches.
typedef enum { SRC_FORECAST, SRC_ALERTS, SRC_BUOY, SRC_STATION, SRC_OBS, SRC_COUNT } src_t;
static time_t s_due[SRC_COUNT];
static char s_forecast_dir[16] = "coastal";     // where the zone's forecast was found
static char s_obs_station[16];                  // nearest NWS station, for the chart's centre
static double s_obs_lat = 999, s_obs_lon = 999;
static char s_station_name[48];
static uint32_t s_station_named;                // the WeatherLink station s_station_name is for

const char *wx_state_str(wx_state_t s)
{
    switch (s) {
    case WX_OFF: return "off";
    case WX_WAITING: return "waiting";
    case WX_OK: return "ok";
    case WX_ERROR: return "error";
    }
    return "?";
}

static wx_source_t *source(weather_data_t *d, src_t s)
{
    wx_source_t *all[] = {&d->forecast_src, &d->alerts_src, &d->buoy_src, &d->station_src, &d->obs_src};
    return all[s];
}

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void)
{
    s_generation++;
    xSemaphoreGive(s_lock);
}

static void set_state(src_t s, wx_state_t st, const char *err)
{
    lock();
    wx_source_t *src = source(s_data, s);
    src->state = st;
    if (err) snprintf(src->error, sizeof(src->error), "%s", err);
    if (st == WX_OK) {
        src->fetched = time(NULL);
        src->error[0] = '\0';
    }
    unlock();
}

// A failed fetch: keep the data, say why, and try again in a while.
static void failed(src_t s, const char *what, const char *err)
{
    ESP_LOGW(TAG, "%s: %s", what, err);
    set_state(s, WX_ERROR, err);
    s_due[s] = time(NULL) + RETRY_S;
}

// GET url into a PSRAM buffer (caller frees). NULL on failure, with err set;
// *status is the HTTP status (0 if there was no answer).
static char *http_get(const char *url, int *status, char *err, size_t errlen)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = USER_AGENT,
    };
    *status = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    char *buf = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        snprintf(err, errlen, "can't connect");
        goto out;
    }
    esp_http_client_fetch_headers(c);
    *status = esp_http_client_get_status_code(c);
    if (*status != 200) {
        snprintf(err, errlen, *status == 404 ? "not found" : "HTTP %d", *status);
        goto out;
    }
    buf = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) {
        snprintf(err, errlen, "out of memory");
        goto out;
    }
    int total = 0;
    for (;;) {
        int n = esp_http_client_read(c, buf + total, RESP_MAX - 1 - total);
        if (n <= 0) break;
        total += n;
        if (total >= RESP_MAX - 1) break;
    }
    buf[total] = '\0';
out:
    esp_http_client_cleanup(c);
    return buf;
}

static void *psram_alloc(size_t n)
{
    return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
}

// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------

static void fetch_forecast(const settings_t *cfg)
{
    // The zone's forecast is under coastal/, near_shore/ (Great Lakes) or
    // offshore/, in a folder named after the zone's first two letters.
    static const char *dirs[] = {"coastal", "near_shore", "offshore"};
    char lower[8] = "", err[64] = "";
    for (int i = 0; cfg->wx_zone[i] && i < 7; i++) lower[i] = tolower((unsigned char)cfg->wx_zone[i]);
    // A file that isn't there answers 301 (to the same path with a "/") rather than 404.
    char *body = NULL;
    int status = 0;
    bool missing = false;
    for (int attempt = 0; attempt < 4 && !body; attempt++) {
        const char *dir = attempt == 0 ? s_forecast_dir : dirs[attempt - 1];
        if (attempt > 0 && strcmp(dir, s_forecast_dir) == 0) continue;
        char url[128];
        snprintf(url, sizeof(url), "https://tgftp.nws.noaa.gov/data/forecasts/marine/%s/%.2s/%s.txt", dir, lower, lower);
        body = http_get(url, &status, err, sizeof(err));
        missing = status == 404 || status == 301 || status == 302;
        if (body) snprintf(s_forecast_dir, sizeof(s_forecast_dir), "%s", dir);
        else if (!missing) break; // a real failure, not a missing file: don't try the others
    }
    if (!body) return failed(SRC_FORECAST, "forecast", missing ? "no forecast for this zone" : err);
    marine_forecast_t *f = psram_alloc(sizeof(*f));
    bool ok = f && mf_parse(body, f);
    free(body);
    if (!ok) {
        free(f);
        return failed(SRC_FORECAST, "forecast", "couldn't read the forecast");
    }
    lock();
    s_data->forecast = *f;
    s_data->have_forecast = true;
    unlock();
    free(f);
    set_state(SRC_FORECAST, WX_OK, NULL);
    ESP_LOGI(TAG, "forecast for %s, issued %s: %d periods, %d headlines", cfg->wx_zone, s_data->forecast.issued,
             s_data->forecast.nperiods, s_data->forecast.nheadlines);
    s_due[SRC_FORECAST] = time(NULL) + FORECAST_EVERY_S;
}

static void fetch_alerts(const settings_t *cfg)
{
    char url[96], err[64];
    int status;
    snprintf(url, sizeof(url), "https://api.weather.gov/alerts/active/zone/%s", cfg->wx_zone);
    char *body = http_get(url, &status, err, sizeof(err));
    if (!body) return failed(SRC_ALERTS, "alerts", err);
    wx_alert_t *a = psram_alloc(WX_MAX_ALERTS * sizeof(*a));
    int n = a ? wx_parse_alerts(body, a, WX_MAX_ALERTS) : -1;
    free(body);
    if (n < 0) {
        free(a);
        return failed(SRC_ALERTS, "alerts", "couldn't read the alerts");
    }
    lock();
    memcpy(s_data->alerts, a, n * sizeof(*a));
    s_data->nalerts = n;
    s_data->alert_flag = false;
    for (int i = 0; i < n; i++) s_data->alert_flag |= a[i].flag;
    unlock();
    free(a);
    set_state(SRC_ALERTS, WX_OK, NULL);
    s_due[SRC_ALERTS] = time(NULL) + ALERTS_EVERY_S;
}

static void fetch_buoy(const settings_t *cfg)
{
    char url[96], err[64];
    int status;
    snprintf(url, sizeof(url), "https://www.ndbc.noaa.gov/data/realtime2/%s.txt", cfg->wx_buoy);
    char *body = http_get(url, &status, err, sizeof(err));
    if (!body) return failed(SRC_BUOY, "buoy", status == 404 ? "no data for this buoy" : err);
    buoy_t b;
    bool ok = buoy_parse(body, &b);
    free(body);
    if (!ok) return failed(SRC_BUOY, "buoy", "couldn't read the buoy's data");
    lock();
    s_data->buoy = b;
    snprintf(s_data->buoy_id, sizeof(s_data->buoy_id), "%s", cfg->wx_buoy);
    s_data->have_buoy = true;
    unlock();
    set_state(SRC_BUOY, WX_OK, NULL);
    s_due[SRC_BUOY] = time(NULL) + BUOY_EVERY_S;
}

static void fetch_station(const settings_t *cfg)
{
    char secret[48], err[64], path[32];
    settings_get_wl_secret(secret);
    if (!secret[0]) {
        set_state(SRC_STATION, WX_OFF, "no API secret saved");
        s_due[SRC_STATION] = time(NULL) + RETRY_S;
        return;
    }
    // The station's name, once per station.
    if (s_station_named != cfg->wl_station) {
        snprintf(path, sizeof(path), "stations/%lu", (unsigned long)cfg->wl_station);
        char *body = weatherlink_get(path, cfg->wl_key, secret, err, sizeof(err));
        wl_station_t st;
        if (body && wl_parse_stations(body, &st, 1) == 1) {
            snprintf(s_station_name, sizeof(s_station_name), "%s", st.name);
            s_station_named = cfg->wl_station;
        }
        free(body);
    }
    snprintf(path, sizeof(path), "current/%lu", (unsigned long)cfg->wl_station);
    char *body = weatherlink_get(path, cfg->wl_key, secret, err, sizeof(err));
    if (!body) return failed(SRC_STATION, "WeatherLink", err);
    wl_current_t c;
    bool ok = wl_parse_current(body, cfg->wl_tx, &c);
    free(body);
    if (!ok) {
        snprintf(err, sizeof(err), "transmitter %d isn't reporting", cfg->wl_tx);
        return failed(SRC_STATION, "WeatherLink", err);
    }
    lock();
    s_data->station = c;
    snprintf(s_data->station_name, sizeof(s_data->station_name), "%s", s_station_name);
    s_data->have_station = true;
    unlock();
    set_state(SRC_STATION, WX_OK, NULL);
    s_due[SRC_STATION] = time(NULL) + STATION_EVERY_S;
}

// The nearest NWS observation station to the chart's centre, looked up once
// per centre; then its latest observation, for the icon's cloud and rain/snow.
static void fetch_obs(const settings_t *cfg)
{
    char url[160], err[64];
    int status;
    if (!s_obs_station[0] || cfg->center_lat != s_obs_lat || cfg->center_lon != s_obs_lon) {
        snprintf(url, sizeof(url), "https://api.weather.gov/points/%.4f,%.4f", cfg->center_lat, cfg->center_lon);
        char *body = http_get(url, &status, err, sizeof(err));
        char list[128] = "";
        bool ok = body && wx_parse_points(body, list, sizeof(list));
        free(body);
        if (!ok) return failed(SRC_OBS, "NWS points", status == 404 ? "the chart's centre is outside the US" : err);
        snprintf(url, sizeof(url), "%s?limit=1", list);
        body = http_get(url, &status, err, sizeof(err));
        bool answered = body != NULL;
        ok = answered && wx_parse_first_station(body, s_obs_station, sizeof(s_obs_station));
        free(body);
        if (!ok) return failed(SRC_OBS, "NWS stations", answered ? "no station nearby" : err);
        s_obs_lat = cfg->center_lat;
        s_obs_lon = cfg->center_lon;
        ESP_LOGI(TAG, "nearest NWS station: %s", s_obs_station);
    }
    snprintf(url, sizeof(url), "https://api.weather.gov/stations/%s/observations/latest", s_obs_station);
    char *body = http_get(url, &status, err, sizeof(err));
    if (!body) return failed(SRC_OBS, "NWS observation", err);
    wx_obs_t o;
    bool ok = wx_parse_observation(body, &o);
    free(body);
    if (!ok) return failed(SRC_OBS, "NWS observation", "couldn't read the observation");
    lock();
    s_data->obs = o;
    snprintf(s_data->obs_station, sizeof(s_data->obs_station), "%s", s_obs_station);
    s_data->have_obs = true;
    unlock();
    set_state(SRC_OBS, WX_OK, NULL);
    s_due[SRC_OBS] = time(NULL) + OBS_EVERY_S;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

// Which sources are set up. One that isn't is shown as off with its data cleared.
static bool configured(const settings_t *cfg, src_t s)
{
    switch (s) {
    case SRC_FORECAST:
    case SRC_ALERTS: return cfg->wx_zone[0];
    case SRC_BUOY: return cfg->wx_buoy[0];
    case SRC_STATION: return cfg->wl_key[0] && cfg->wl_station;
    case SRC_OBS: return true; // from the chart's centre
    default: return false;
    }
}

// Settings changed: forget what no longer applies, and fetch everything now.
static void reset(const settings_t *cfg)
{
    lock();
    weather_data_t *d = s_data;
    if (!cfg->wx_zone[0] || strcmp(d->forecast.zone, cfg->wx_zone) != 0) {
        d->have_forecast = false;
        d->nalerts = 0;
        d->alert_flag = false;
    }
    if (strcmp(d->buoy_id, cfg->wx_buoy) != 0) d->have_buoy = false;
    if (!configured(cfg, SRC_STATION)) d->have_station = false;
    for (int s = 0; s < SRC_COUNT; s++) {
        wx_source_t *src = source(d, s);
        src->state = configured(cfg, s) ? WX_WAITING : WX_OFF;
        src->error[0] = '\0';
    }
    unlock();
    for (int s = 0; s < SRC_COUNT; s++) s_due[s] = 0;
    s_station_named = 0;
    snprintf(s_forecast_dir, sizeof(s_forecast_dir), "coastal");
}

static void weather_task(void *arg)
{
    settings_t cfg;
    settings_get(&cfg);
    reset(&cfg);
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events, SETTINGS_EV_WEATHER | SETTINGS_EV_CENTER, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(1000));
        if (bits) {
            settings_get(&cfg);
            reset(&cfg);
        }
        if (!wifi_mgr_has_ip() || !clock_is_set()) continue; // HTTPS needs both
        time_t now = time(NULL);
        static void (*const fetch[SRC_COUNT])(const settings_t *) = {fetch_forecast, fetch_alerts, fetch_buoy,
                                                                      fetch_station, fetch_obs};
        for (int s = 0; s < SRC_COUNT; s++) {
            if (configured(&cfg, s) && now >= s_due[s]) fetch[s](&cfg);
        }
    }
}

void weather_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_data = heap_caps_calloc(1, sizeof(*s_data), MALLOC_CAP_SPIRAM);
    assert(s_data);
    s_events = settings_subscribe();
    xTaskCreate(weather_task, "weather", 8192, NULL, 3, NULL);
}

void weather_get(weather_data_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = *s_data;
    xSemaphoreGive(s_lock);
}

uint32_t weather_generation(void)
{
    return s_generation;
}
