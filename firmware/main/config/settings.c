#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "aisdisp";

#define MAX_SUBSCRIBERS 6

static settings_t s_settings;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_subs[MAX_SUBSCRIBERS];
static int s_nsubs;

static void defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->ais_port = 10110;
    strcpy(s->tide_station, "9447130"); // Seattle
    strcpy(s->tz_name, "America/Los_Angeles");
    strcpy(s->tz_posix, "PST8PDT,M3.2.0,M11.1.0");
    s->center_lat = 47.6005;
    s->center_lon = -122.3550;
    s->zoom_index = 1;
    s->brightness = 100;
    s->filter = (vessel_filter_t){
        .mode = FILTER_HIDE_LABEL, .order = ORDER_SIZE, .include_unknown = true, .label_name = true, .label_speed = true,
    };
}

// Old firmware stored fewer filter fields: only accept a blob of exactly the current size.
static void get_filter(nvs_handle_t h, vessel_filter_t *dst)
{
    vessel_filter_t f;
    size_t len = sizeof(f);
    if (nvs_get_blob(h, "filter", &f, &len) == ESP_OK && len == sizeof(f)) *dst = f;
}

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        // keep default
    }
}

static void get_double(nvs_handle_t h, const char *key, double *dst)
{
    size_t len = sizeof(*dst);
    nvs_get_blob(h, key, dst, &len);
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_lock = xSemaphoreCreateMutex();
    defaults(&s_settings);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ais_host", s_settings.ais_host, sizeof(s_settings.ais_host));
        nvs_get_u16(h, "ais_port", &s_settings.ais_port);
        get_str(h, "tide", s_settings.tide_station, sizeof(s_settings.tide_station));
        get_str(h, "tz_name", s_settings.tz_name, sizeof(s_settings.tz_name));
        get_str(h, "tz_posix", s_settings.tz_posix, sizeof(s_settings.tz_posix));
        get_double(h, "lat", &s_settings.center_lat);
        get_double(h, "lon", &s_settings.center_lon);
        nvs_get_u8(h, "zoom", &s_settings.zoom_index);
        nvs_get_u8(h, "bright", &s_settings.brightness);
        get_filter(h, &s_settings.filter);
        nvs_get_u32(h, "map_created", &s_settings.map_created);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "AIS %s:%u tide %s tz %s centre %.5f,%.5f", s_settings.ais_host, s_settings.ais_port,
             s_settings.tide_station, s_settings.tz_posix, s_settings.center_lat, s_settings.center_lon);
    return ESP_OK;
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

static void notify(EventBits_t bits)
{
    if (!bits) return;
    for (int i = 0; i < s_nsubs; i++) {
        xEventGroupSetBits(s_subs[i], bits);
    }
}

esp_err_t settings_set(const settings_t *in)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    settings_t old = s_settings;
    s_settings = *in;
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ais_host", in->ais_host);
    nvs_set_u16(h, "ais_port", in->ais_port);
    nvs_set_str(h, "tide", in->tide_station);
    nvs_set_str(h, "tz_name", in->tz_name);
    nvs_set_str(h, "tz_posix", in->tz_posix);
    nvs_set_blob(h, "lat", &in->center_lat, sizeof(double));
    nvs_set_blob(h, "lon", &in->center_lon, sizeof(double));
    nvs_set_u8(h, "zoom", in->zoom_index);
    nvs_set_u8(h, "bright", in->brightness);
    nvs_set_blob(h, "filter", &in->filter, sizeof(in->filter));
    nvs_set_u32(h, "map_created", in->map_created);
    err = nvs_commit(h);
    nvs_close(h);

    EventBits_t bits = 0;
    if (strcmp(old.ais_host, in->ais_host) || old.ais_port != in->ais_port) bits |= SETTINGS_EV_AIS;
    if (strcmp(old.tide_station, in->tide_station)) bits |= SETTINGS_EV_TIDE;
    if (strcmp(old.tz_posix, in->tz_posix)) bits |= SETTINGS_EV_TZ;
    if (old.center_lat != in->center_lat || old.center_lon != in->center_lon) bits |= SETTINGS_EV_CENTER;
    if (old.brightness != in->brightness) bits |= SETTINGS_EV_DISPLAY;
    notify(bits);
    return err;
}

void settings_notify_map_changed(void)
{
    notify(SETTINGS_EV_MAP);
}

EventGroupHandle_t settings_subscribe(void)
{
    assert(s_nsubs < MAX_SUBSCRIBERS);
    EventGroupHandle_t g = xEventGroupCreate();
    s_subs[s_nsubs++] = g;
    return g;
}

bool settings_get_wifi(char ssid[33], char pass[65])
{
    nvs_handle_t h;
    ssid[0] = pass[0] = '\0';
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l1 = 33, l2 = 65;
    bool ok = nvs_get_str(h, "wifi_ssid", ssid, &l1) == ESP_OK;
    if (nvs_get_str(h, "wifi_pass", pass, &l2) != ESP_OK) pass[0] = '\0';
    nvs_close(h);
    return ok && ssid[0];
}

esp_err_t settings_set_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "wifi_ssid", ssid);
    nvs_set_str(h, "wifi_pass", pass);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
