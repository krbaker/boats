#include "tide.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "settings.h"
#include "wifi_mgr.h"

static const char *TAG = "tide";

#define MAX_EVENTS      32
#define REFRESH_S       (6 * 3600)
#define RETRY_S         300
#define RESP_MAX        8192

static tide_event_t s_events[MAX_EVENTS];
static tide_status_t s_status;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_settings_ev;

bool clock_is_set(void)
{
    return time(NULL) > 1700000000;
}

static void apply_tz(void)
{
    settings_t cfg;
    settings_get(&cfg);
    setenv("TZ", cfg.tz_posix[0] ? cfg.tz_posix : "UTC0", 1);
    tzset();
    ESP_LOGI(TAG, "timezone %s (%s)", cfg.tz_name, cfg.tz_posix);
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm).
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

// "2024-10-03 04:12" in UTC -> time_t
static time_t parse_gmt(const char *s)
{
    int y, mo, d, h, mi;
    if (sscanf(s, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return 0;
    return (time_t)days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60;
}

// GET url into a heap buffer (caller frees). Returns NULL on failure, with err set.
static char *http_get(const char *url, char *err, size_t errlen)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .user_agent = "ais-display/1.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char *buf = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        snprintf(err, errlen, "connection failed");
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        snprintf(err, errlen, "HTTP %d", status);
        goto out;
    }
    buf = malloc(RESP_MAX);
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

static void fetch_station_name(const char *station)
{
    char url[160], err[32];
    snprintf(url, sizeof(url), "https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations/%s.json", station);
    char *body = http_get(url, err, sizeof(err));
    if (!body) return;
    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *stations = cJSON_GetObjectItem(root, "stations");
    cJSON *name = cJSON_GetObjectItem(cJSON_GetArrayItem(stations, 0), "name");
    if (cJSON_IsString(name)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_status.station_name, name->valuestring, sizeof(s_status.station_name));
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(root);
}

static bool fetch_predictions(const char *station)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char url[320], err[64] = "";
    snprintf(url, sizeof(url),
             "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&application=ais_display"
             "&begin_date=%04d%02d%02d&range=72&datum=MLLW&station=%s&time_zone=gmt&units=english&interval=hilo"
             "&format=json",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, station);

    char *body = http_get(url, err, sizeof(err));
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    free(body);

    tide_event_t events[MAX_EVENTS];
    int n = 0;
    cJSON *preds = cJSON_GetObjectItem(root, "predictions");
    if (cJSON_IsArray(preds)) {
        cJSON *p;
        cJSON_ArrayForEach(p, preds) {
            cJSON *t = cJSON_GetObjectItem(p, "t"), *v = cJSON_GetObjectItem(p, "v"), *ty = cJSON_GetObjectItem(p, "type");
            if (!cJSON_IsString(t) || !cJSON_IsString(v) || !cJSON_IsString(ty) || n >= MAX_EVENTS) continue;
            events[n].t = parse_gmt(t->valuestring);
            events[n].height_ft = strtof(v->valuestring, NULL);
            events[n].high = ty->valuestring[0] == 'H';
            if (events[n].t) n++;
        }
    } else if (root) {
        cJSON *e = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "error"), "message");
        snprintf(err, sizeof(err), "%s", cJSON_IsString(e) ? e->valuestring : "unexpected response");
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (n > 0) {
        memcpy(s_events, events, n * sizeof(events[0]));
        s_status.count = n;
        s_status.state = TIDE_OK;
        s_status.fetched = now;
        s_status.error[0] = '\0';
    } else {
        s_status.state = TIDE_ERROR;
        strlcpy(s_status.error, err[0] ? err : "no predictions", sizeof(s_status.error));
    }
    xSemaphoreGive(s_lock);

    if (n > 0) {
        ESP_LOGI(TAG, "station %s: %d tide events", station, n);
    } else {
        ESP_LOGW(TAG, "station %s: %s", station, s_status.error);
    }
    return n > 0;
}

static void tide_task(void *arg)
{
    time_t next_fetch = 0;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_settings_ev, SETTINGS_EV_TIDE | SETTINGS_EV_TZ, pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(5000));
        if (bits & SETTINGS_EV_TZ) apply_tz();
        if (bits & SETTINGS_EV_TIDE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.count = 0;
            s_status.station_name[0] = '\0';
            xSemaphoreGive(s_lock);
            next_fetch = 0;
        }

        settings_t cfg;
        settings_get(&cfg);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_status.station, cfg.tide_station, sizeof(s_status.station));
        if (!cfg.tide_station[0]) {
            s_status.state = TIDE_NO_STATION;
            s_status.count = 0;
        } else if (s_status.count == 0 && s_status.state != TIDE_ERROR) {
            s_status.state = TIDE_WAITING;
        }
        xSemaphoreGive(s_lock);

        if (!cfg.tide_station[0] || !wifi_mgr_has_ip() || !clock_is_set()) continue;
        time_t now = time(NULL);
        if (now < next_fetch) continue;

        if (!s_status.station_name[0]) fetch_station_name(cfg.tide_station);
        next_fetch = now + (fetch_predictions(cfg.tide_station) ? REFRESH_S : RETRY_S);
    }
}

void tide_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_settings_ev = settings_subscribe();
    apply_tz();

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);

    xTaskCreate(tide_task, "tide", 8192, NULL, 3, NULL);
}

void tide_get_status(tide_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

bool tide_next(time_t now, tide_event_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_status.count; i++) {
        if (s_events[i].t > now) {
            *out = s_events[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}
