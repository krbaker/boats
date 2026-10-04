// Table of AIS targets heard recently.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "ais.h"

#define VESSEL_MAX          500
#define VESSEL_EXPIRY_S     (10 * 60)

typedef struct {
    uint32_t mmsi;
    ais_class_t cls;
    uint32_t fields;        // AIS_F_* bits ever received for this vessel
    double lat, lon;
    float sog_kn, cog_deg;
    uint16_t heading;
    uint8_t nav_status;
    uint8_t ship_type;
    uint16_t dim_a, dim_b;
    uint8_t dim_c, dim_d;
    char name[21];
    char callsign[8];
    char destination[21];
    int64_t last_heard_us;  // esp_timer_get_time() of the last message
    time_t last_heard_wall; // wall clock of the last message (0 if clock not set)
} vessel_t;

void vessel_db_init(void);
void vessel_db_update(const ais_report_t *r);
void vessel_db_expire(void);

// Copy all vessels into out (up to max). Returns the number copied.
int vessel_db_snapshot(vessel_t *out, int max);

// Copy a single vessel. Returns false if it isn't (or is no longer) known.
bool vessel_db_get(uint32_t mmsi, vessel_t *out);

int vessel_db_count(void);

// Increments whenever a vessel is added, updated or expired.
uint32_t vessel_db_generation(void);
