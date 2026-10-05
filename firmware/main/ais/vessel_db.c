#include "vessel_db.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "map_store.h"
#include "vessel_cache.h"

static const char *TAG = "vessels";

#define CACHE_DIR       MAP_ROOT "/vessels"
#define CACHE_CAPACITY  20000
#define CACHE_FLUSH_S   60

static vessel_t *s_vessels;
static int s_count;
static volatile uint32_t s_generation;
static SemaphoreHandle_t s_lock;
static vessel_cache_t *s_cache; // NULL until cache_task has loaded it

static vessel_cache_t *cache(void)
{
    return __atomic_load_n(&s_cache, __ATOMIC_ACQUIRE);
}

static uint32_t mono_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

// Fills in the static values v hasn't received itself from the cache.
static void fill_from_cache(vessel_t *v)
{
    vc_vessel_t c;
    if (!cache() || !vc_get(cache(), v->mmsi, &c)) return;
    if (v->cls == AIS_CLASS_UNKNOWN) v->cls = c.cls;
    if ((c.fields & VC_F_NAME) && !(v->fields & AIS_F_NAME)) {
        strcpy(v->name, c.name);
        v->fields |= AIS_F_NAME;
    }
    if ((c.fields & VC_F_CALLSIGN) && !(v->fields & AIS_F_CALLSIGN)) {
        strcpy(v->callsign, c.callsign);
        v->fields |= AIS_F_CALLSIGN;
    }
    if ((c.fields & VC_F_SHIPTYPE) && !(v->fields & AIS_F_SHIPTYPE)) {
        v->ship_type = c.ship_type;
        v->fields |= AIS_F_SHIPTYPE;
    }
    if ((c.fields & VC_F_DIM) && !(v->fields & AIS_F_DIM)) {
        v->dim_a = c.dim_a;
        v->dim_b = c.dim_b;
        v->dim_c = c.dim_c;
        v->dim_d = c.dim_d;
        v->fields |= AIS_F_DIM;
    }
}

// Tells the cache what a message said about a vessel's static data.
static void update_cache(const ais_report_t *r, time_t wall)
{
    if (!cache()) return;
    vc_vessel_t c = {.mmsi = r->mmsi, .cls = r->cls};
    if (r->fields & AIS_F_NAME) {
        strcpy(c.name, r->name);
        c.fields |= VC_F_NAME;
    }
    if (r->fields & AIS_F_CALLSIGN) {
        strcpy(c.callsign, r->callsign);
        c.fields |= VC_F_CALLSIGN;
    }
    if (r->fields & AIS_F_SHIPTYPE) {
        c.ship_type = r->ship_type;
        c.fields |= VC_F_SHIPTYPE;
    }
    if (r->fields & AIS_F_DIM) {
        c.dim_a = r->dim_a;
        c.dim_b = r->dim_b;
        c.dim_c = r->dim_c;
        c.dim_d = r->dim_d;
        c.fields |= VC_F_DIM;
    }
    vc_update(cache(), &c, (uint32_t)wall);
}

// Loads the cache (off the startup path: it can be over a megabyte), then
// writes changes to the card every minute.
static void cache_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    vessel_cache_t *vc = vc_open(map_store_card_ok() ? CACHE_DIR : NULL, CACHE_CAPACITY);
    if (!vc) {
        ESP_LOGE(TAG, "no memory for the vessel cache");
        vTaskDelete(NULL);
    }
    vc_stats_t st;
    vc_get_stats(vc, &st);
    ESP_LOGI(TAG, "vessel cache: %d vessels loaded in %d ms%s%s%s", st.count, (int)((esp_timer_get_time() - t0) / 1000),
             st.ram_only ? " (no card: not saved)" : "", st.last_error[0] ? "; " : "", st.last_error);
    __atomic_store_n(&s_cache, vc, __ATOMIC_RELEASE);

    // Fill in vessels heard while it loaded.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) fill_from_cache(&s_vessels[i]);
    s_generation++;
    xSemaphoreGive(s_lock);

    for (;;) {
        if (vc_flush(vc, mono_s(), false) < 0) {
            vc_get_stats(vc, &st);
            ESP_LOGW(TAG, "vessel cache: %s", st.last_error);
        }
        vTaskDelay(pdMS_TO_TICKS(CACHE_FLUSH_S * 1000));
    }
}

void vessel_db_init(void)
{
    s_vessels = heap_caps_calloc(VESSEL_MAX, sizeof(vessel_t), MALLOC_CAP_SPIRAM);
    assert(s_vessels);
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(cache_task, "vessel_cache", 4096, NULL, 2, NULL);
}

bool vessel_db_cache_stats(vc_stats_t *out)
{
    if (!cache()) return false;
    vc_get_stats(cache(), out);
    return true;
}

void vessel_db_forget_all(void)
{
    if (!cache()) return;
    vc_clear(cache());
    ESP_LOGI(TAG, "vessel cache cleared");
}

void vessel_db_save(void)
{
    if (!cache()) return;
    int n = vc_flush(cache(), mono_s(), true);
    if (n > 0) ESP_LOGI(TAG, "vessel cache: saved %d vessel(s)", n);
}

static vessel_t *find(uint32_t mmsi)
{
    for (int i = 0; i < s_count; i++) {
        if (s_vessels[i].mmsi == mmsi) return &s_vessels[i];
    }
    return NULL;
}

static time_t wall_now(void)
{
    time_t now = time(NULL);
    return now > 1600000000 ? now : 0; // treat an unset clock as unknown
}

void vessel_db_update(const ais_report_t *r, rx_source_t src)
{
    if (r->mmsi == 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vessel_t *v = find(r->mmsi);
    if (!v) {
        if (s_count >= VESSEL_MAX) {
            // Table full: replace the vessel heard least recently.
            v = &s_vessels[0];
            for (int i = 1; i < s_count; i++) {
                if (s_vessels[i].last_heard_us < v->last_heard_us) v = &s_vessels[i];
            }
        } else {
            v = &s_vessels[s_count++];
        }
        memset(v, 0, sizeof(*v));
        v->mmsi = r->mmsi;
        fill_from_cache(v);
    }

    if (r->cls != AIS_CLASS_UNKNOWN) v->cls = r->cls;
    if (r->fields & AIS_F_POS) { v->lat = r->lat; v->lon = r->lon; }
    if (r->fields & AIS_F_SOG) v->sog_kn = r->sog_kn;
    if (r->fields & AIS_F_COG) v->cog_deg = r->cog_deg;
    if (r->fields & AIS_F_HEADING) v->heading = r->heading;
    if (r->fields & AIS_F_NAVSTAT) v->nav_status = r->nav_status;
    if (r->fields & AIS_F_SHIPTYPE) v->ship_type = r->ship_type;
    if (r->fields & AIS_F_DIM) { v->dim_a = r->dim_a; v->dim_b = r->dim_b; v->dim_c = r->dim_c; v->dim_d = r->dim_d; }
    if (r->fields & AIS_F_NAME) strcpy(v->name, r->name);
    if (r->fields & AIS_F_CALLSIGN) strcpy(v->callsign, r->callsign);
    if (r->fields & AIS_F_DEST) strcpy(v->destination, r->destination);

    // Position reports that omit heading/COG mean "not available now".
    if (r->fields & AIS_F_POS) {
        uint32_t dynamic = AIS_F_SOG | AIS_F_COG | AIS_F_HEADING;
        v->fields = (v->fields & ~dynamic) | (r->fields & dynamic);
    }
    v->fields |= r->fields;
    if (src < RX_NSRC) v->msgs[src]++;
    v->last_heard_us = esp_timer_get_time();
    v->last_heard_wall = wall_now();
    time_t wall = v->last_heard_wall;
    s_generation++;
    xSemaphoreGive(s_lock);
    update_cache(r, wall);
}

void vessel_db_expire(void)
{
    int64_t cutoff = esp_timer_get_time() - (int64_t)VESSEL_EXPIRY_S * 1000000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int removed = 0;
    for (int i = 0; i < s_count;) {
        if (s_vessels[i].last_heard_us < cutoff) {
            s_vessels[i] = s_vessels[--s_count];
            removed++;
        } else {
            i++;
        }
    }
    if (removed) s_generation++;
    xSemaphoreGive(s_lock);
    if (removed) ESP_LOGI(TAG, "expired %d vessel(s), %d remain", removed, s_count);
}

int vessel_db_snapshot(vessel_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count < max ? s_count : max;
    memcpy(out, s_vessels, n * sizeof(vessel_t));
    xSemaphoreGive(s_lock);
    return n;
}

bool vessel_db_get(uint32_t mmsi, vessel_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vessel_t *v = find(mmsi);
    if (v) *out = *v;
    xSemaphoreGive(s_lock);
    return v != NULL;
}

int vessel_db_count(void)
{
    return s_count;
}

void vessel_db_source_counts(int *net_only, int *sdr_only, int *both)
{
    *net_only = *sdr_only = *both = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        bool net = s_vessels[i].msgs[RX_NET], sdr = s_vessels[i].msgs[RX_SDR];
        if (net && sdr) (*both)++;
        else if (net) (*net_only)++;
        else if (sdr) (*sdr_only)++;
    }
    xSemaphoreGive(s_lock);
}

uint32_t vessel_db_generation(void)
{
    return s_generation;
}
