// Aircraft from a dump1090 receiver (ADS-B): its aircraft.json, fetched over
// the network every couple of seconds, keeping the aircraft within the set
// range of the position.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "adsb_parse.h"

#define ADSB_MAX        64      // the nearest this many are kept
#define ADSB_STALE_S    20      // a position this old is drawn dimmed...
#define ADSB_EXPIRY_S   60      // ...and dropped at this age

typedef struct {
    adsb_aircraft_t a;
    uint32_t id;            // adsb_id(&a)
    float dist_nm;          // from the position
    int64_t pos_us;         // esp_timer_get_time() when the position was heard
} adsb_target_t;

typedef enum { ADSB_OFF, ADSB_NOT_SET, ADSB_CONNECTING, ADSB_OK, ADSB_ERROR } adsb_state_t;

typedef struct {
    adsb_state_t state;
    char error[64];         // why the last fetch failed (ADSB_ERROR)
    int heard;              // aircraft with a position in the last file
    int shown;              // of those, how many are within range
    int64_t last_ok_us;     // esp_timer_get_time() of the last good fetch, 0 = never
} adsb_status_t;

// An aircraft's key: its address, with bit 24 set for a non-ICAO one.
static inline uint32_t adsb_id(const adsb_aircraft_t *a)
{
    return a->addr | (a->non_icao ? 1u << 24 : 0);
}

void adsb_start(void);

// The aircraft within range, nearest first (positions older than
// ADSB_EXPIRY_S left out). Returns how many.
int adsb_snapshot(adsb_target_t *out, int max);
bool adsb_get(uint32_t id, adsb_target_t *out);
uint32_t adsb_generation(void);     // changes whenever the list does

void adsb_get_status(adsb_status_t *out);
const char *adsb_state_str(adsb_state_t st);
