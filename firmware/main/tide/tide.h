// NOAA CO-OPS tide predictions, plus SNTP / timezone handling.

#pragma once

#include <stdbool.h>
#include <time.h>

typedef struct {
    time_t t;
    float height_ft;
    bool high;
} tide_event_t;

// Predicted water level at one moment (the tide curve).
typedef struct {
    time_t t;
    float height_ft;
} tide_sample_t;

typedef enum {
    TIDE_NO_STATION,
    TIDE_WAITING,       // for network / clock
    TIDE_OK,
    TIDE_ERROR,
} tide_state_t;

typedef struct {
    tide_state_t state;
    char station[16];
    char station_name[48];
    char error[64];
    time_t fetched;
    int count;
} tide_status_t;

// Starts SNTP, applies the configured timezone and fetches tides in the background.
void tide_start(void);

void tide_get_status(tide_status_t *out);

// Next tide event after `now`. Returns false if none is known.
bool tide_next(time_t now, tide_event_t *out);

// Copy the known high/low events, or the half-hourly tide curve (covering at
// least the last 6 h and the next 24 h), oldest first. Return the number copied.
int tide_events(tide_event_t *out, int max);
int tide_curve(tide_sample_t *out, int max);

// Predicted water level at t, interpolated from the tide curve. False if unknown.
bool tide_level(time_t t, float *height_ft);

bool clock_is_set(void);
