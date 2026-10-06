// Parsing NWS and NDBC data, and deciding the weather icon (WEATHER_PLAN.md).
//
// Pure C plus cJSON, no ESP-IDF, so it's tested on the host against real
// responses in test/host/fixtures/weather/. Units are converted to the
// display's: mph, feet, degrees F, inHg.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "astro.h"

#ifdef __cplusplus
extern "C" {
#endif

// "2026-10-05T20:54:00-04:00" (or Z) to Unix time. 0 if it isn't one.
time_t wx_parse_iso8601(const char *s);

// ---------------------------------------------------------------------------
// NWS marine forecast: the plain-text product for one zone, from
// tgftp.nws.noaa.gov/data/forecasts/marine/coastal/<pz>/<pzz135>.txt
// ---------------------------------------------------------------------------

#define MF_MAX_HEADLINES 4
#define MF_MAX_PERIODS   16

typedef struct {
    char zone[8];               // "PZZ135"
    char zone_name[96];         // "Puget Sound and Hood Canal"
    char issued[48];            // as written: "133 PM PDT Mon Oct 5 2026"
    time_t expires;             // from the "Expires:" line, 0 if missing
    int nheadlines;
    char headline[MF_MAX_HEADLINES][160];   // "SMALL CRAFT ADVISORY IN EFFECT UNTIL 10 PM EDT THIS EVENING"
    int nperiods;
    struct {
        char name[24];          // "TONIGHT", "TUE NIGHT"
        char text[400];         // wrapped lines joined with spaces
    } period[MF_MAX_PERIODS];
} marine_forecast_t;

// False if it isn't a marine zone forecast.
bool mf_parse(const char *text, marine_forecast_t *out);

// ---------------------------------------------------------------------------
// NWS alerts: api.weather.gov/alerts/active/zone/<zone>
// ---------------------------------------------------------------------------

#define WX_MAX_ALERTS 6

typedef struct {
    char event[48];             // "Small Craft Advisory"
    char headline[200];
    char description[1200];     // trimmed to fit
    time_t ends;                // "ends", or "expires" if there's no end; 0 if neither
    bool flag;                  // a watch, warning or advisory: shows the red "!"
} wx_alert_t;

// Returns the number of alerts (up to max), or -1 if it isn't an alert list.
int wx_parse_alerts(const char *json, wx_alert_t *out, int max);

// ---------------------------------------------------------------------------
// NWS observations
// ---------------------------------------------------------------------------

typedef enum { CLOUD_UNKNOWN, CLOUD_CLEAR, CLOUD_FEW, CLOUD_SCATTERED, CLOUD_BROKEN, CLOUD_OVERCAST } cloud_t;
typedef enum { PRECIP_NONE, PRECIP_RAIN, PRECIP_SNOW } precip_t;

typedef struct {
    time_t when;
    char text[64];              // textDescription: "Light Rain", "Clear"
    cloud_t cloud;              // the most cover of any layer
    precip_t precip;
} wx_obs_t;

// /stations/<id>/observations/latest. False if it isn't one.
bool wx_parse_observation(const char *json, wx_obs_t *out);

// /points/<lat>,<lon>: the URL of the nearby observation stations list.
bool wx_parse_points(const char *json, char *stations_url, int cap);

// That list (sorted nearest first): the first station's ID ("KBFI").
bool wx_parse_first_station(const char *json, char *id, int cap);

// ---------------------------------------------------------------------------
// NDBC buoy: www.ndbc.noaa.gov/data/realtime2/<buoy>.txt
// ---------------------------------------------------------------------------

typedef struct {
    bool ok;
    float v;
    time_t when;                // the row it came from
} buoy_val_t;

typedef struct {
    buoy_val_t wave_ft;         // significant wave height (WVHT)
    buoy_val_t dom_period_s;    // dominant wave period (DPD)
    buoy_val_t avg_period_s;    // average wave period (APD)
    buoy_val_t wave_dir;        // mean wave direction, degrees it comes from (MWD)
    buoy_val_t wind_dir;        // WDIR
    buoy_val_t wind_mph;        // WSPD
    buoy_val_t gust_mph;        // GST
    buoy_val_t pressure_inhg;   // PRES
    buoy_val_t pressure_trend_inhg; // PTDY: change over 3 hours
    buoy_val_t air_f;           // ATMP
    buoy_val_t water_f;         // WTMP
    buoy_val_t dew_f;           // DEWP
    time_t latest;              // the newest row
} buoy_t;

// Each value from the newest row that has it ("MM" = missing). False if it
// isn't an NDBC standard meteorological file.
bool buoy_parse(const char *text, buoy_t *out);

// ---------------------------------------------------------------------------
// The weather icon and the pressure trend
// ---------------------------------------------------------------------------

typedef struct {
    bool night;                 // the moon instead of the sun
    double moon_illum;          // at night: 0-1
    bool moon_waxing;
    int cloud;                  // 0 none, 1 small cloud, 2 large cloud, 3 overcast
    bool rain, snow;            // snow wins: a snowflake
    bool alert;                 // the red "!"
} wx_icon_t;

// obs may be NULL (no NWS station). rain_rate_in_hr < 0 if unknown.
void wx_icon_decide(double lat, double lon, time_t now, const wx_obs_t *obs, float rain_rate_in_hr, bool alert,
                    wx_icon_t *out);

typedef enum { TREND_STEADY, TREND_RISING, TREND_FALLING, TREND_RISING_FAST, TREND_FALLING_FAST } trend_t;

// From the change over 3 hours in inHg: steady under 0.02, fast from 0.06.
trend_t wx_pressure_trend(float change_inhg_3h);
const char *wx_trend_name(trend_t t);

const char *wx_compass(int deg);    // "NNW"

#ifdef __cplusplus
}
#endif
