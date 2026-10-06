// NDBC buoy standard meteorological data: see weather_parse.h.
//
//   #YY  MM DD hh mm WDIR WSPD GST  WVHT   DPD   APD MWD   PRES  ATMP  WTMP  DEWP  VIS PTDY  TIDE
//   #yr  mo dy hr mn degT m/s  m/s     m   sec   sec degT   hPa  degC  degC  degC  nmi  hPa    ft
//   2026 10 06 00 50 220  7.0  8.0   0.3     3   2.9 227 1019.9  12.5    MM  12.1   MM   MM    MM
//
// Newest row first, "MM" for missing. Columns are found by name, since they
// differ a little between buoys and over the years.

#define _POSIX_C_SOURCE 200809L // strdup, strtok_r under -std=c11

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weather_parse.h"

#define MAX_COLS 24

static time_t row_time(int y, int mo, int d, int h, int mi)
{
    // Days since 1970 (Howard Hinnant's algorithm), avoiding timegm().
    int yy = y - (mo <= 2);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + h * 3600 + mi * 60;
}

static float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

bool buoy_parse(const char *text, buoy_t *out)
{
    memset(out, 0, sizeof(*out));
    // Which output each column fills, and how to convert it.
    static const struct { const char *name; size_t off; float scale; bool temp; } fields[] = {
        {"WVHT", offsetof(buoy_t, wave_ft), 3.28084f, false},
        {"DPD", offsetof(buoy_t, dom_period_s), 1, false},
        {"APD", offsetof(buoy_t, avg_period_s), 1, false},
        {"MWD", offsetof(buoy_t, wave_dir), 1, false},
        {"WDIR", offsetof(buoy_t, wind_dir), 1, false},
        {"WSPD", offsetof(buoy_t, wind_mph), 2.23694f, false},
        {"GST", offsetof(buoy_t, gust_mph), 2.23694f, false},
        {"PRES", offsetof(buoy_t, pressure_inhg), 0.0295300f, false},
        {"PTDY", offsetof(buoy_t, pressure_trend_inhg), 0.0295300f, false},
        {"ATMP", offsetof(buoy_t, air_f), 1, true},
        {"WTMP", offsetof(buoy_t, water_f), 1, true},
        {"DEWP", offsetof(buoy_t, dew_f), 1, true},
    };
    const int nfields = sizeof(fields) / sizeof(fields[0]);
    int col_field[MAX_COLS]; // field index per column, -1 = unused
    int ncols = 0, rows = 0;
    bool have_header = false;

    char *buf = strdup(text);
    if (!buf) return false;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tok[MAX_COLS], *s2 = NULL;
        int n = 0;
        for (char *t = strtok_r(line, " \t\r", &s2); t && n < MAX_COLS; t = strtok_r(NULL, " \t\r", &s2)) tok[n++] = t;
        if (!n) continue;
        if (!have_header) {
            if (strcmp(tok[0], "#YY") != 0) continue;
            ncols = n;
            for (int c = 0; c < n; c++) {
                col_field[c] = -1;
                for (int f = 0; f < nfields; f++) {
                    if (!strcmp(tok[c], fields[f].name)) col_field[c] = f;
                }
            }
            have_header = true;
            continue;
        }
        if (tok[0][0] == '#' || n < 5 || n > ncols) continue; // the units line
        time_t when = row_time(atoi(tok[0]), atoi(tok[1]), atoi(tok[2]), atoi(tok[3]), atoi(tok[4]));
        if (!rows++) out->latest = when;
        for (int c = 5; c < n; c++) {
            if (col_field[c] < 0 || !strcmp(tok[c], "MM")) continue;
            buoy_val_t *v = (buoy_val_t *)((char *)out + fields[col_field[c]].off);
            if (v->ok) continue; // a newer row had it
            float x = strtof(tok[c], NULL);
            v->v = fields[col_field[c]].temp ? c_to_f(x) : x * fields[col_field[c]].scale;
            v->when = when;
            v->ok = true;
        }
    }
    free(buf);
    return have_header && rows > 0;
}
