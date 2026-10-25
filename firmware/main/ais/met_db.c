#include "met_db.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "met_db";

static met_station_t s_st[MET_MAX];
static int s_n;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_gen;

void met_db_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

// The same station: its MMSI and site, and for the international message
// (no site number) the same place, within about 100 m.
static bool same(const met_station_t *s, uint32_t mmsi, const ais_met_t *m)
{
    if (s->mmsi != mmsi || s->m.site != m->site) return false;
    if (m->site != AIS_MET_SITE_IMO || !(m->fields & AIS_MET_POS) || !(s->m.fields & AIS_MET_POS)) return true;
    return fabs(s->m.lat - m->lat) < 0.001 && fabs(s->m.lon - m->lon) < 0.0015;
}

// Copy into d each reading s has.
static void merge(ais_met_t *d, const ais_met_t *s)
{
    uint32_t f = s->fields;
    if (f & AIS_MET_POS) d->lat = s->lat, d->lon = s->lon;
    if (f & AIS_MET_NAME) strcpy(d->name, s->name);
    if (f & AIS_MET_TIME) d->day = s->day, d->hour = s->hour, d->minute = s->minute;
    if (f & AIS_MET_WIND) d->wind_kn = s->wind_kn, d->wind_dir = s->wind_dir;
    if (f & AIS_MET_GUST) d->gust_kn = s->gust_kn, d->gust_dir = s->gust_dir;
    if (f & AIS_MET_AIR_TEMP) d->air_c = s->air_c;
    if (f & AIS_MET_DEW) d->dew_c = s->dew_c;
    if (f & AIS_MET_HUMIDITY) d->humidity = s->humidity;
    if (f & AIS_MET_PRESSURE) d->pressure_hpa = s->pressure_hpa, d->pressure_trend = s->pressure_trend;
    if (f & AIS_MET_VIS) d->vis_nm = s->vis_nm;
    if (f & AIS_MET_WATER_LEVEL) d->water_m = s->water_m, d->water_trend = s->water_trend;
    if (f & AIS_MET_CURRENT) d->current_kn = s->current_kn, d->current_dir = s->current_dir;
    if (f & AIS_MET_WAVES) d->wave_m = s->wave_m, d->wave_s = s->wave_s, d->wave_dir = s->wave_dir;
    if (f & AIS_MET_SWELL) d->swell_m = s->swell_m, d->swell_s = s->swell_s, d->swell_dir = s->swell_dir;
    if (f & AIS_MET_WATER_TEMP) d->water_c = s->water_c;
    if (f & AIS_MET_SALINITY) d->salinity = s->salinity;
    if (f & AIS_MET_SEA_STATE) d->sea_state = s->sea_state;
    d->fields |= f;
}

void met_db_update(uint32_t mmsi, const ais_met_t *m)
{
    if (!m->fields) return;
    int64_t now = esp_timer_get_time();
    time_t wall = time(NULL);
    lock();
    met_station_t *s = NULL;
    for (int i = 0; i < s_n && !s; i++) {
        if (same(&s_st[i], mmsi, m)) s = &s_st[i];
    }
    bool is_new = !s;
    if (!s) {
        if (s_n < MET_MAX) {
            s = &s_st[s_n++];
        } else { // full: replace the one heard from longest ago
            s = &s_st[0];
            for (int i = 1; i < s_n; i++) {
                if (s_st[i].last_us < s->last_us) s = &s_st[i];
            }
        }
        memset(s, 0, sizeof(*s));
        s->mmsi = mmsi;
        s->m.site = m->site;
    }
    merge(&s->m, m);
    s->last_us = now;
    s->last_wall = wall > 1700000000 ? wall : 0;
    s_gen++;
    xSemaphoreGive(s_lock);
    if (is_new) ESP_LOGI(TAG, "weather station %lu site %u", (unsigned long)mmsi, m->site);
}

int met_db_snapshot(met_station_t *out, int max)
{
    int64_t cutoff = esp_timer_get_time() - (int64_t)MET_EXPIRY_S * 1000000;
    lock();
    int n = 0;
    for (int i = 0; i < s_n && n < max; i++) {
        if (s_st[i].last_us >= cutoff) out[n++] = s_st[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool met_db_get(uint32_t mmsi, uint8_t site, double lat, double lon, met_station_t *out)
{
    ais_met_t probe = {.site = site, .fields = AIS_MET_POS, .lat = lat, .lon = lon};
    lock();
    bool found = false;
    for (int i = 0; i < s_n && !found; i++) {
        if (same(&s_st[i], mmsi, &probe)) {
            *out = s_st[i];
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

uint32_t met_db_generation(void)
{
    return s_gen;
}
