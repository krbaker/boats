#include "vessel_db.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "vessels";

static vessel_t *s_vessels;
static int s_count;
static volatile uint32_t s_generation;
static SemaphoreHandle_t s_lock;

void vessel_db_init(void)
{
    s_vessels = heap_caps_calloc(VESSEL_MAX, sizeof(vessel_t), MALLOC_CAP_SPIRAM);
    assert(s_vessels);
    s_lock = xSemaphoreCreateMutex();
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
    s_generation++;
    xSemaphoreGive(s_lock);
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
