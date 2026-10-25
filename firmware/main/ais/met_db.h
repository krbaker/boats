// Weather and water stations heard over AIS (type 8: see ais_met_t). Each
// station (sending MMSI and site) is the latest of each reading it has sent:
// a US site's location, name, wind and water level can come in separate
// messages.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "ais.h"

#define MET_MAX         32
#define MET_STALE_S     (30 * 60)   // drawn dimmed after this without a reading
#define MET_EXPIRY_S    (2 * 3600)  // and forgotten after this

typedef struct {
    uint32_t mmsi;
    ais_met_t m;            // fields: every reading it has sent
    int64_t last_us;        // esp_timer_get_time() of the latest
    time_t last_wall;       // 0 if the clock wasn't set
} met_station_t;

void met_db_init(void);    // (vessel_db_init calls it)
void met_db_update(uint32_t mmsi, const ais_met_t *m);

// The stations heard in the last MET_EXPIRY_S. Returns how many.
int met_db_snapshot(met_station_t *out, int max);
// One station: by MMSI and site, and for the international message (no site
// number) its position.
bool met_db_get(uint32_t mmsi, uint8_t site, double lat, double lon, met_station_t *out);
uint32_t met_db_generation(void);
