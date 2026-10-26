#include "adsb.h"

#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "geo.h"
#include "settings.h"

static const char *TAG = "adsb";

#define POLL_MS         2000
#define BACKOFF_MAX_MS  30000
#define TIMEOUT_MS      5000
#define RESP_MAX        (512 * 1024)    // PSRAM: a busy readsb's file is ~150 KB

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static adsb_target_t *s_targets, *s_work;   // published list, and the one being filled (PSRAM)
static int s_n;
static uint32_t s_gen;
static adsb_status_t s_status;
static char *s_buf;

const char *adsb_state_str(adsb_state_t st)
{
    static const char *const names[] = {"off", "no address set", "connecting", "connected", "error"};
    return st < sizeof(names) / sizeof(names[0]) ? names[st] : "?";
}

static void set_state(adsb_state_t st, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_status.state != st || strcmp(s_status.error, err) != 0;
    s_status.state = st;
    strlcpy(s_status.error, err, sizeof(s_status.error));
    xSemaphoreGive(s_lock);
    if (changed && st == ADSB_ERROR) ESP_LOGW(TAG, "%s", err);
}

static void clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_n) s_gen++;
    s_n = 0;
    s_status.heard = s_status.shown = 0;
    xSemaphoreGive(s_lock);
}

// GET the file into s_buf. Returns its length, or -1 with err set.
static int fetch(const char *url, char *err, size_t errlen)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = TIMEOUT_MS,
        .user_agent = "ais-display/1.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    int total = -1;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        snprintf(err, errlen, "can't connect");
        goto out;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        snprintf(err, errlen, "HTTP %d (check the address)", status);
        goto out;
    }
    total = 0;
    for (;;) {
        int n = esp_http_client_read(c, s_buf + total, RESP_MAX - total);
        if (n <= 0) break;
        total += n;
        if (total >= RESP_MAX) {
            static bool warned;
            if (!warned) ESP_LOGW(TAG, "aircraft.json is over %d KB: reading only the start", RESP_MAX / 1024);
            warned = true;
            break;
        }
    }
out:
    esp_http_client_cleanup(c);
    return total;
}

typedef struct {
    double lat, lon;
    float range_nm;
    int64_t now_us;
    int n, heard;
} collect_t;

// Each aircraft in the file: kept if it has a recent position within range.
// When there are more than ADSB_MAX, the farthest go.
static bool collect_cb(void *ctx, const adsb_aircraft_t *a)
{
    collect_t *c = ctx;
    if (!(a->fields & ADSB_F_POS) || a->seen_pos_s > ADSB_EXPIRY_S || adsb_kind(a) == ADSB_KIND_SURFACE) return true;
    c->heard++;
    float d = (float)geo_distance_nm(c->lat, c->lon, a->lat, a->lon);
    if (d > c->range_nm) return true;
    adsb_target_t t = {.a = *a, .id = adsb_id(a), .dist_nm = d, .pos_us = c->now_us - (int64_t)(a->seen_pos_s * 1e6f)};
    if (c->n < ADSB_MAX) {
        s_work[c->n++] = t;
        return true;
    }
    int far = 0;
    for (int i = 1; i < c->n; i++) {
        if (s_work[i].dist_nm > s_work[far].dist_nm) far = i;
    }
    if (d < s_work[far].dist_nm) s_work[far] = t;
    return true;
}

static int dist_cmp(const void *a, const void *b)
{
    float da = ((const adsb_target_t *)a)->dist_nm, db = ((const adsb_target_t *)b)->dist_nm;
    return (da > db) - (da < db);
}

// One fetch. False if it failed (the state says why).
static bool fetch_once(const settings_t *cfg)
{
    char err[64];
    int64_t t0 = esp_timer_get_time();
    int len = fetch(cfg->adsb_url, err, sizeof(err));
    if (len < 0) {
        set_state(ADSB_ERROR, err);
        return false;
    }
    collect_t c = {.lat = cfg->center_lat, .lon = cfg->center_lon, .range_nm = cfg->adsb_range_nm, .now_us = t0};
    double now;
    if (adsb_parse(s_buf, len, collect_cb, &c, &now) < 0) {
        set_state(ADSB_ERROR, "that address isn't a dump1090 aircraft.json");
        return false;
    }
    qsort(s_work, c.n, sizeof(s_work[0]), dist_cmp);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool first = s_status.state != ADSB_OK;
    memcpy(s_targets, s_work, c.n * sizeof(s_work[0]));
    s_n = c.n;
    s_gen++;
    s_status.heard = c.heard;
    s_status.shown = c.n;
    s_status.last_ok_us = esp_timer_get_time();
    s_status.state = ADSB_OK;
    s_status.error[0] = '\0';
    xSemaphoreGive(s_lock);
    if (first) ESP_LOGI(TAG, "%d aircraft heard, %d within %u nm", c.heard, c.n, cfg->adsb_range_nm);
    return true;
}

static void adsb_task(void *arg)
{
    int backoff_ms = 0;
    for (;;) {
        settings_t cfg;
        settings_get(&cfg);
        TickType_t wait = pdMS_TO_TICKS(POLL_MS);
        if (!cfg.adsb_enabled || !cfg.adsb_url[0]) {
            clear();
            set_state(cfg.adsb_enabled ? ADSB_NOT_SET : ADSB_OFF, "");
            backoff_ms = 0;
            wait = portMAX_DELAY;
        } else {
            if (s_status.state == ADSB_OFF || s_status.state == ADSB_NOT_SET) set_state(ADSB_CONNECTING, "");
            if (fetch_once(&cfg)) {
                backoff_ms = 0;
            } else {
                // Keep what we had (it ages out by itself), and try less often.
                backoff_ms = backoff_ms ? MIN(backoff_ms * 2, BACKOFF_MAX_MS) : 5000;
                wait = pdMS_TO_TICKS(backoff_ms);
            }
        }
        // Settings changes (on, off, a new address or range) take effect at once.
        if (xEventGroupWaitBits(s_events, SETTINGS_EV_ADSB, pdTRUE, pdFALSE, wait)) backoff_ms = 0;
    }
}

void adsb_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_targets = heap_caps_calloc(ADSB_MAX, sizeof(adsb_target_t), MALLOC_CAP_SPIRAM);
    s_work = heap_caps_calloc(ADSB_MAX, sizeof(adsb_target_t), MALLOC_CAP_SPIRAM);
    s_buf = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    if (!s_targets || !s_work || !s_buf) {
        ESP_LOGE(TAG, "out of memory for aircraft");
        return;
    }
    s_events = settings_subscribe();
    // Core 0: off the SDR decoder's core.
    xTaskCreatePinnedToCore(adsb_task, "adsb", 6144, NULL, 2, NULL, 0);
}

int adsb_snapshot(adsb_target_t *out, int max)
{
    if (!s_lock) return 0;
    int64_t cutoff = esp_timer_get_time() - (int64_t)ADSB_EXPIRY_S * 1000000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < s_n && n < max; i++) {
        if (s_targets[i].pos_us >= cutoff) out[n++] = s_targets[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool adsb_get(uint32_t id, adsb_target_t *out)
{
    if (!s_lock) return false;
    int64_t cutoff = esp_timer_get_time() - (int64_t)ADSB_EXPIRY_S * 1000000;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n && !found; i++) {
        if (s_targets[i].id == id && s_targets[i].pos_us >= cutoff) {
            *out = s_targets[i];
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

uint32_t adsb_generation(void)
{
    return s_gen;
}

void adsb_get_status(adsb_status_t *out)
{
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
