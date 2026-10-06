// Sun and moon for the weather icon and the Weather tab (WEATHER_PLAN.md).
//
// Low-precision formulas from Meeus, "Astronomical Algorithms" (2nd ed.) and
// NOAA's solar calculator: sunrise and sunset to about a minute, the moon's
// illuminated fraction to about 1%, and new / full moon times to a few
// minutes. Tested against USNO in test/host/test_weather_parse.c.

#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// The sun's altitude above the horizon in degrees (no refraction).
double sun_elevation(double lat, double lon, time_t t);

// The sun is up: its upper edge is above the horizon, allowing for refraction.
bool sun_is_up(double lat, double lon, time_t t);

// The first sunrise (rise = true) or sunset in [t0, t1). False if there's none
// (polar day or night, or the window is too short).
bool sun_event(double lat, double lon, time_t t0, time_t t1, bool rise, time_t *out);

typedef enum {
    MOON_NEW,
    MOON_WAXING_CRESCENT,
    MOON_FIRST_QUARTER,
    MOON_WAXING_GIBBOUS,
    MOON_FULL,
    MOON_WANING_GIBBOUS,
    MOON_LAST_QUARTER,
    MOON_WANING_CRESCENT,
} moon_phase_t;

typedef struct {
    double illum;           // fraction of the disc lit, 0-1
    bool waxing;            // lit on the right, seen from the northern hemisphere
    double elongation;      // the moon's longitude minus the sun's, 0-360 degrees
    moon_phase_t phase;
} moon_t;

void moon_at(time_t t, moon_t *out);
const char *moon_phase_name(moon_phase_t p);

// The first new moon (full = false) or full moon after t.
time_t moon_next(time_t t, bool full);

#ifdef __cplusplus
}
#endif
