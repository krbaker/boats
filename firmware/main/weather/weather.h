// Weather: fetches the marine forecast, alerts, buoy, WeatherLink station and
// the nearest NWS observation in the background (WEATHER_PLAN.md).

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "weather_parse.h"
#include "weatherlink_parse.h"

typedef enum {
    WX_OFF,         // not set up
    WX_WAITING,     // for Wi-Fi / the clock / the first fetch
    WX_OK,
    WX_ERROR,       // the last fetch failed; older data may still be there
} wx_state_t;

typedef struct {
    wx_state_t state;
    time_t fetched;     // last successful fetch, 0 = never
    char error[64];
} wx_source_t;

// Everything the screens show, copied out in one go (about 20 KB: put it in PSRAM).
typedef struct {
    wx_source_t forecast_src, alerts_src, buoy_src, station_src, obs_src;

    bool have_forecast;
    marine_forecast_t forecast;

    int nalerts;
    wx_alert_t alerts[WX_MAX_ALERTS];
    bool alert_flag;            // one of them is a watch, warning or advisory

    bool have_buoy;
    char buoy_id[8];
    buoy_t buoy;

    bool have_station;
    char station_name[48];
    wl_current_t station;

    bool have_obs;
    char obs_station[16];       // "KBFI"
    wx_obs_t obs;
} weather_data_t;

void weather_start(void);
void weather_get(weather_data_t *out);

// Changes whenever any weather data or status changes.
uint32_t weather_generation(void);

const char *wx_state_str(wx_state_t s);
