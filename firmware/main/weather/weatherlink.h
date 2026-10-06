// WeatherLink v2 API client (WEATHER_PLAN.md): the HTTPS side. Parsing is in
// components/weather_parse.

#pragma once

#include <stddef.h>

#include "weatherlink_parse.h"

#define WL_MAX_STATIONS 8
#define WL_MAX_TX       8

typedef struct {
    wl_station_t station;
    wl_tx_t tx[WL_MAX_TX];
    int ntx;                // -1 if its current conditions couldn't be read
} wl_station_info_t;

// The stations an API key and secret can see, each with the outdoor sensor
// suites it hears. Several HTTPS requests: takes a few seconds. Returns the
// number of stations, or -1 with err set.
int weatherlink_list(const char *key, const char *secret, wl_station_info_t *out, int max, char *err, size_t errlen);

// GET an api.weatherlink.com/v2 path ("stations", "current/192412"). Returns
// the body (caller frees), or NULL with err set.
char *weatherlink_get(const char *path, const char *key, const char *secret, char *err, size_t errlen);
