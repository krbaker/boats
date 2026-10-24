#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "map_store.h"
#include "settings_json.h"

static const char *TAG = "settings";
static const char *NS = "aisdisp";

#define MAX_SUBSCRIBERS 8

// The settings live on the microSD card, as JSON that can be edited on a
// computer. Flash keeps a copy for when there's no card. The secrets (Wi-Fi
// and access-point passwords, the WeatherLink secret) only live in flash; the
// file can set them, and they're taken out of it as soon as they're read.
#define CARD_PATH       MAP_ROOT "/config.json"
#define CARD_TMP_PATH   MAP_ROOT "/config.json.tmp"
#define CARD_BAD_PATH   MAP_ROOT "/config.bad.json"     // a file that couldn't be used, kept for looking at
#define CARD_MAX        16384

static settings_t s_settings;
static SemaphoreHandle_t s_lock, s_card_lock;
static char s_card_ssid[33], s_card_pass[65]; // Wi-Fi credentials from the card, for wifi_mgr to test
static bool s_card_wifi;
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
    s->net_enabled = true;
    s->sdr_enabled = true;
    s->sdr_gain = SDR_GAIN_BEST;
}

static void fwd_key(char *key, size_t cap, int i, const char *name)
{
    snprintf(key, cap, "fwd%d_%s", i + 1, name);
}

static void get_fwd(nvs_handle_t h, fwd_dest_t fwd[FWD_DEST_MAX])
{
    char key[16];
    uint8_t u8;
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        fwd_key(key, sizeof(key), i, "on");
        if (nvs_get_u8(h, key, &u8) == ESP_OK) fwd[i].enabled = u8;
        fwd_key(key, sizeof(key), i, "proto");
        if (nvs_get_u8(h, key, &u8) == ESP_OK && u8 <= FWD_TCP) fwd[i].proto = u8;
        fwd_key(key, sizeof(key), i, "port");
        nvs_get_u16(h, key, &fwd[i].port);
        fwd_key(key, sizeof(key), i, "host");
        size_t len = sizeof(fwd[i].host);
        if (nvs_get_str(h, key, fwd[i].host, &len) != ESP_OK) fwd[i].host[0] = '\0';
    }
}

static void set_fwd(nvs_handle_t h, const fwd_dest_t fwd[FWD_DEST_MAX])
{
    char key[16];
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        fwd_key(key, sizeof(key), i, "on");
        nvs_set_u8(h, key, fwd[i].enabled);
        fwd_key(key, sizeof(key), i, "proto");
        nvs_set_u8(h, key, fwd[i].proto);
        fwd_key(key, sizeof(key), i, "port");
        nvs_set_u16(h, key, fwd[i].port);
        fwd_key(key, sizeof(key), i, "host");
        nvs_set_str(h, key, fwd[i].host);
    }
}

static bool fwd_equal(const fwd_dest_t *a, const fwd_dest_t *b)
{
    return a->enabled == b->enabled && a->proto == b->proto && a->port == b->port && strcmp(a->host, b->host) == 0;
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

static esp_err_t nvs_save(const settings_t *in)
{
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
    nvs_set_u8(h, "net_on", in->net_enabled);
    nvs_set_u8(h, "sdr_on", in->sdr_enabled);
    nvs_set_u8(h, "sdr_bias", in->sdr_bias_tee);
    nvs_set_i16(h, "sdr_gain", in->sdr_gain);
    nvs_set_i16(h, "sdr_ppm", in->sdr_ppm_tenths);
    set_fwd(h, in->fwd);
    nvs_set_str(h, "wx_zone", in->wx_zone);
    nvs_set_str(h, "wx_buoy", in->wx_buoy);
    nvs_set_str(h, "wx_obs", in->wx_obs);
    nvs_set_str(h, "wl_key", in->wl_key);
    nvs_set_u32(h, "wl_station", in->wl_station);
    nvs_set_u8(h, "wl_tx", in->wl_tx);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Write the settings (and the saved network's name) to the card: to a new file
// first, so a card pulled mid-write doesn't lose the old one.
static void card_save(const settings_t *in)
{
    if (!map_store_card_ok()) return;
    char ssid[33], pass[65];
    settings_get_wifi(ssid, pass);
    cJSON *o = settings_to_json(in);
    cJSON_AddStringToObject(o, "wifi_ssid", ssid);
    char *text = cJSON_Print(o);
    cJSON_Delete(o);
    if (!text) return;

    xSemaphoreTake(s_card_lock, portMAX_DELAY);
    FILE *f = fopen(CARD_TMP_PATH, "w");
    bool ok = f && fputs(text, f) >= 0 && fputc('\n', f) != EOF;
    if (f && fclose(f) != 0) ok = false;
    if (ok) {
        unlink(CARD_PATH); // FAT won't rename over a file
        ok = rename(CARD_TMP_PATH, CARD_PATH) == 0;
    }
    xSemaphoreGive(s_card_lock);
    cJSON_free(text);
    if (!ok) ESP_LOGW(TAG, "couldn't write %s", CARD_PATH);
}

// A secret from the card: taken out of o, and true if it was there.
static bool take_secret(cJSON *o, const char *key, char *dst, size_t cap)
{
    cJSON *v = cJSON_DetachItemFromObject(o, key);
    if (!v) return false;
    bool ok = cJSON_IsString(v) && strlen(v->valuestring) < cap;
    if (ok) strcpy(dst, v->valuestring);
    else ESP_LOGW(TAG, "config.json: %s isn't a usable value, ignored", key);
    cJSON_Delete(v);
    return ok;
}

// Printable ASCII, as WPA2 wants, and min..max long.
static bool valid_pass(const char *p, size_t min, size_t max)
{
    size_t n = strlen(p);
    if (n < min || n > max) return false;
    for (; *p; p++) {
        if (*p < 0x20 || *p > 0x7e) return false;
    }
    return true;
}

static char *read_card_file(void)
{
    FILE *f = fopen(CARD_PATH, "r");
    if (!f) return NULL;
    char *buf = heap_caps_malloc(CARD_MAX + 1, MALLOC_CAP_SPIRAM);
    size_t n = buf ? fread(buf, 1, CARD_MAX + 1, f) : 0;
    fclose(f);
    if (!buf || n > CARD_MAX) {
        ESP_LOGW(TAG, "config.json is too big");
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
}

// Settings from the card's file over the ones from flash. Its secrets go to
// flash; then the file is rewritten without them (or written, if there's none).
static void card_load(void)
{
    if (!map_store_card_ok()) {
        ESP_LOGW(TAG, "no microSD card: settings from flash");
        return;
    }
    char *text = read_card_file();
    if (!text) {
        ESP_LOGI(TAG, "no usable config.json: writing one");
        card_save(&s_settings);
        return;
    }
    cJSON *o = cJSON_Parse(text);
    free(text);
    if (!o) {
        // Its secrets can't be taken out, so they stay in config.bad.json.
        ESP_LOGW(TAG, "config.json isn't valid JSON: kept as config.bad.json, settings from flash");
        unlink(CARD_BAD_PATH);
        rename(CARD_PATH, CARD_BAD_PATH);
        card_save(&s_settings);
        return;
    }

    // The secrets first, so they're gone from the file whatever else is in it.
    char ssid[33], pass[65], saved_ssid[33], saved_pass[65], v[65];
    settings_get_wifi(saved_ssid, saved_pass);
    strcpy(ssid, saved_ssid);
    bool new_ssid = take_secret(o, "wifi_ssid", ssid, sizeof(ssid)) && strcmp(ssid, saved_ssid) != 0;
    bool new_pass = take_secret(o, "wifi_password", pass, sizeof(pass));
    if (!new_pass) pass[0] = '\0'; // a new network with no password given: an open one
    if ((new_ssid || new_pass) && ssid[0] && (!pass[0] || valid_pass(pass, 8, 64))) {
        ESP_LOGI(TAG, "config.json: Wi-Fi network \"%s\", to be tested", ssid);
        strcpy(s_card_ssid, ssid);
        strcpy(s_card_pass, pass);
        s_card_wifi = true;
    } else if (new_ssid || new_pass) {
        ESP_LOGW(TAG, "config.json: Wi-Fi network or password not usable, ignored");
    }
    if (take_secret(o, "ap_password", v, 16)) {
        if (valid_pass(v, 8, 15)) {
            ESP_LOGI(TAG, "config.json: new access point password");
            settings_set_ap_pass(v);
        } else {
            ESP_LOGW(TAG, "config.json: ap_password must be 8-15 characters, ignored");
        }
    }
    if (take_secret(o, "wl_secret", v, 48)) {
        if (settings_alnum(v, 48)) {
            ESP_LOGI(TAG, "config.json: new WeatherLink secret");
            settings_set_wl_secret(v);
        } else {
            ESP_LOGW(TAG, "config.json: wl_secret must be letters and digits, ignored");
        }
    }
    memset(pass, 0, sizeof(pass));
    memset(v, 0, sizeof(v));

    settings_t s = s_settings;
    char msg[80];
    if (settings_from_json(o, &s, msg, sizeof(msg))) {
        s_settings = s;
        nvs_save(&s_settings);
    } else {
        // Kept, without its secrets, for looking at; the rest from flash.
        ESP_LOGW(TAG, "config.json: %s; kept as config.bad.json, settings from flash", msg);
        char *bad = cJSON_Print(o);
        FILE *f = bad ? fopen(CARD_BAD_PATH, "w") : NULL;
        if (f) {
            fputs(bad, f);
            fclose(f);
        }
        cJSON_free(bad);
    }
    cJSON_Delete(o);
    card_save(&s_settings);
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
    s_card_lock = xSemaphoreCreateMutex();
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
        uint8_t u8;
        if (nvs_get_u8(h, "net_on", &u8) == ESP_OK) s_settings.net_enabled = u8;
        if (nvs_get_u8(h, "sdr_on", &u8) == ESP_OK) s_settings.sdr_enabled = u8;
        if (nvs_get_u8(h, "sdr_bias", &u8) == ESP_OK) s_settings.sdr_bias_tee = u8;
        nvs_get_i16(h, "sdr_gain", &s_settings.sdr_gain);
        nvs_get_i16(h, "sdr_ppm", &s_settings.sdr_ppm_tenths);
        get_fwd(h, s_settings.fwd);
        get_str(h, "wx_zone", s_settings.wx_zone, sizeof(s_settings.wx_zone));
        get_str(h, "wx_buoy", s_settings.wx_buoy, sizeof(s_settings.wx_buoy));
        get_str(h, "wx_obs", s_settings.wx_obs, sizeof(s_settings.wx_obs));
        get_str(h, "wl_key", s_settings.wl_key, sizeof(s_settings.wl_key));
        nvs_get_u32(h, "wl_station", &s_settings.wl_station);
        nvs_get_u8(h, "wl_tx", &s_settings.wl_tx);
        nvs_close(h);
    }
    card_load();
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

    esp_err_t err = nvs_save(in);
    card_save(in);

    EventBits_t bits = 0;
    if (strcmp(old.ais_host, in->ais_host) || old.ais_port != in->ais_port || old.net_enabled != in->net_enabled) {
        bits |= SETTINGS_EV_AIS;
    }
    if (old.sdr_enabled != in->sdr_enabled || old.sdr_gain != in->sdr_gain || old.sdr_ppm_tenths != in->sdr_ppm_tenths ||
        old.sdr_bias_tee != in->sdr_bias_tee) {
        bits |= SETTINGS_EV_SDR;
    }
    if (strcmp(old.tide_station, in->tide_station)) bits |= SETTINGS_EV_TIDE;
    if (strcmp(old.tz_posix, in->tz_posix)) bits |= SETTINGS_EV_TZ;
    if (old.center_lat != in->center_lat || old.center_lon != in->center_lon) bits |= SETTINGS_EV_CENTER;
    if (old.brightness != in->brightness) bits |= SETTINGS_EV_DISPLAY;
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        if (!fwd_equal(&old.fwd[i], &in->fwd[i])) bits |= SETTINGS_EV_FWD;
    }
    if (strcmp(old.wx_zone, in->wx_zone) || strcmp(old.wx_buoy, in->wx_buoy) || strcmp(old.wx_obs, in->wx_obs) ||
        strcmp(old.wl_key, in->wl_key) ||
        old.wl_station != in->wl_station || old.wl_tx != in->wl_tx) {
        bits |= SETTINGS_EV_WEATHER;
    }
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
    settings_t s;
    settings_get(&s);
    card_save(&s); // the network's name is in the card's file
    return err;
}

bool settings_take_card_wifi(char ssid[33], char pass[65])
{
    if (!s_card_wifi) return false;
    s_card_wifi = false;
    strcpy(ssid, s_card_ssid);
    strcpy(pass, s_card_pass);
    memset(s_card_pass, 0, sizeof(s_card_pass));
    return true;
}

void settings_get_wl_secret(char secret[48])
{
    secret[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 48;
    if (nvs_get_str(h, "wl_secret", secret, &len) != ESP_OK) secret[0] = '\0';
    nvs_close(h);
}

esp_err_t settings_set_wl_secret(const char *secret)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "wl_secret", secret);
    err = nvs_commit(h);
    nvs_close(h);
    notify(SETTINGS_EV_WEATHER);
    return err;
}

void settings_get_ap_pass(char pass[16])
{
    pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 16;
    if (nvs_get_str(h, "ap_pass", pass, &len) != ESP_OK) pass[0] = '\0';
    nvs_close(h);
}

esp_err_t settings_set_ap_pass(const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "ap_pass", pass);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
