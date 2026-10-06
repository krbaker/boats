// Parsing WeatherLink v2 API responses (WEATHER_PLAN.md).
//
// Pure C plus cJSON, no ESP-IDF, so it's tested on the host against real
// responses in test/host/fixtures/weather/.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    char name[48];
    char product[8];        // product_number: "6100" WeatherLink Live, "6313" Console
} wl_station_t;

// One outdoor sensor suite a station hears.
typedef struct {
    uint8_t tx_id;
    bool has_temp, has_wind;
    float temp_f;
    float wind_mph;         // latest
    int wind_dir;           // latest, degrees the wind comes from
} wl_tx_t;

// GET /v2/stations. Returns the number of stations (up to max), or -1 if the
// response isn't a station list.
int wl_parse_stations(const char *json, wl_station_t *out, int max);

// GET /v2/current/{station-id}: the outdoor sensor suites in it, by
// transmitter. Returns how many (up to max), or -1 if it isn't a current
// conditions response.
int wl_parse_transmitters(const char *json, wl_tx_t *out, int max);

// Current conditions from one outdoor sensor suite and the barometer.
typedef struct {
    time_t when;                // the sensor suite's reading
    bool has_wind;
    float wind_mph;             // latest
    int wind_dir;
    float wind_avg10_mph;       // 10-minute average, and its direction
    int wind_dir_avg10;
    float gust10_mph;           // highest in the last 10 minutes
    bool has_temp, has_hum, has_dew;
    float temp_f, hum_pct, dew_f;
    bool has_rain;
    float rain_rate_in_hr, rain_today_in;
    bool has_bar;
    time_t bar_when;
    float bar_inhg;             // sea-level pressure
    float bar_trend_inhg;       // change over the last 3 hours
} wl_current_t;

// GET /v2/current/{station-id}: transmitter tx's readings (0 = the first
// outdoor suite), and the barometer. False if that suite isn't there.
bool wl_parse_current(const char *json, int tx, wl_current_t *out);

// A WeatherLink error response's message ("Invalid API key"...), or false.
bool wl_parse_error(const char *json, char *msg, int cap);

#ifdef __cplusplus
}
#endif
