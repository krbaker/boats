#include "vessel_db.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "map_store.h"
#include "met_db.h"
#include "tide.h"
#include "vessel_cache.h"

static const char *TAG = "vessels";

#define CACHE_DIR       MAP_ROOT "/vessels"
#define CACHE_CAPACITY  20000
#define CACHE_FLUSH_S   60

static vessel_t *s_vessels;
static int s_count;
static safety_msg_t s_safety[SAFETY_MSG_MAX];
static int s_nsafety;
static volatile uint32_t s_safety_gen;
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
    met_db_init();
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

static void add_safety_msg(uint32_t mmsi, const char *text, ais_safety_t kind)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    safety_msg_t *m = NULL;
    for (int i = 0; i < s_nsafety && !m; i++) {
        if (s_safety[i].mmsi == mmsi && strcmp(s_safety[i].text, text) == 0) m = &s_safety[i];
    }
    bool is_new = !m;
    if (!m) {
        if (s_nsafety < SAFETY_MSG_MAX) {
            m = &s_safety[s_nsafety++];
        } else { // full: replace the one heard longest ago
            m = &s_safety[0];
            for (int i = 1; i < s_nsafety; i++) {
                if (s_safety[i].last_us < m->last_us) m = &s_safety[i];
            }
        }
        *m = (safety_msg_t){.mmsi = mmsi, .kind = kind, .first_us = now};
        strlcpy(m->text, text, sizeof(m->text));
    }
    m->count++;
    m->last_us = now;
    m->last_wall = wall_now();
    s_safety_gen++;
    xSemaphoreGive(s_lock);
    if (is_new) ESP_LOGW(TAG, "safety message from %lu: %s", (unsigned long)mmsi, text);
}

static int by_newest_safety(const void *a, const void *b)
{
    int64_t x = ((const safety_msg_t *)a)->last_us, y = ((const safety_msg_t *)b)->last_us;
    return (x < y) - (x > y);
}

int vessel_db_safety_msgs(safety_msg_t *out, int max)
{
    static safety_msg_t sorted[SAFETY_MSG_MAX]; // used under s_lock
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_nsafety;
    memcpy(sorted, s_safety, n * sizeof(sorted[0]));
    qsort(sorted, n, sizeof(sorted[0]), by_newest_safety);
    if (n > max) n = max;
    memcpy(out, sorted, n * sizeof(out[0]));
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t vessel_db_safety_generation(void)
{
    return s_safety_gen;
}

void vessel_db_update(const ais_report_t *r, rx_source_t src)
{
    if (r->mmsi == 0) return;
    if (r->nmet) { // weather and water stations (type 8): kept apart, not targets
        for (int i = 0; i < r->nmet; i++) met_db_update(r->mmsi, &r->met[i]);
        return;
    }
    if (r->fields & AIS_F_TEXT) { // a safety broadcast: kept apart, not a target's report
        add_safety_msg(r->mmsi, r->text, ais_safety_kind(r->mmsi, r->text));
        return;
    }
    if (r->cls == AIS_CLASS_BASE) { // a clock (types 4, 11), not a target: for when the internet's isn't there
        if (r->fields & AIS_F_UTC) {
            clock_set_from_ais(r->utc.year, r->utc.month, r->utc.day, r->utc.hour, r->utc.minute, r->utc.second);
        }
        return;
    }
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
    if (r->fields & AIS_F_ATON) { v->aton_type = r->aton_type; v->aton_flags = r->aton_flags; }
    if (r->fields & AIS_F_ALT) v->alt_m = r->alt_m;

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
    if (r->cls != AIS_CLASS_ATON) update_cache(r, wall); // marks send their name every few minutes anyway

    // An AIS-SART (MMSI 970...) is a distress beacon: its position reports alone
    // are a distress message, unless it says it's testing (navigational status
    // 15, which SARTs use for test transmissions).
    if (r->mmsi / 1000000 == 970 && (r->fields & AIS_F_POS)) {
        bool test = (r->fields & AIS_F_NAVSTAT) && r->nav_status == 15;
        add_safety_msg(r->mmsi, test ? "AIS-SART in test mode" : "AIS-SART detected",
                       test ? AIS_SAFETY_TEST : AIS_SAFETY_DISTRESS);
    }
}

void vessel_db_expire(void)
{
    int64_t now = esp_timer_get_time();
    int64_t cutoff = now - (int64_t)VESSEL_EXPIRY_S * 1000000, aton_cutoff = now - (int64_t)ATON_EXPIRY_S * 1000000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int removed = 0;
    for (int i = 0; i < s_count;) {
        if (s_vessels[i].last_heard_us < (s_vessels[i].cls == AIS_CLASS_ATON ? aton_cutoff : cutoff)) {
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
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) n += s_vessels[i].cls != AIS_CLASS_ATON;
    xSemaphoreGive(s_lock);
    return n;
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
