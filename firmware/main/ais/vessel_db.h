// Table of AIS targets heard recently.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "ais.h"
#include "rx_stats.h"
#include "vessel_cache.h"

// Vessels, SAR aircraft and aids to navigation (base stations only set the
// clock: see clock_set_from_ais).
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
    uint8_t aton_type;      // aids to navigation (cls AIS_CLASS_ATON): AIS_ATON_*
    uint8_t aton_flags;
    uint16_t alt_m;         // SAR aircraft (cls AIS_CLASS_SAR): altitude, AIS_F_ALT
    int64_t last_heard_us;  // esp_timer_get_time() of the last message
    time_t last_heard_wall; // wall clock of the last message (0 if clock not set)
    uint32_t msgs[RX_NSRC]; // messages from each source (network, SDR)
} vessel_t;

// Also starts the vessel cache: names, call signs, types and sizes heard
// before are remembered on the microSD card (VESSEL_CACHE_PLAN.md), and fill
// in a vessel as soon as it's heard again.
void vessel_db_init(void);
void vessel_db_update(const ais_report_t *r, rx_source_t src);
void vessel_db_expire(void);

// Copy all vessels into out (up to max). Returns the number copied.
int vessel_db_snapshot(vessel_t *out, int max);

// Copy a single vessel. Returns false if it isn't (or is no longer) known.
bool vessel_db_get(uint32_t mmsi, vessel_t *out);

// Vessels and aircraft (not aids to navigation).
int vessel_db_count(void);

// How many vessels have been heard only on the network feed, only by the
// local SDR, and by both.
void vessel_db_source_counts(int *net_only, int *sdr_only, int *both);

// Write any unsaved changes to the vessel cache now. Call before a planned restart.
void vessel_db_save(void);

// The vessel cache's state. False while it's still loading at startup.
bool vessel_db_cache_stats(vc_stats_t *out);

// Forget every remembered vessel, on the card too. Vessels in the table keep
// what they've sent.
void vessel_db_forget_all(void);

// Increments whenever a vessel is added, updated or expired.
uint32_t vessel_db_generation(void);
