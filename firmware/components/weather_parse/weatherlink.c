// WeatherLink v2 API responses: see weatherlink_parse.h.

#include "weatherlink_parse.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

static void copy_str(cJSON *o, const char *key, char *dst, int cap)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    snprintf(dst, cap, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

int wl_parse_stations(const char *json, wl_station_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *list = cJSON_GetObjectItem(root, "stations");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    cJSON *s;
    cJSON_ArrayForEach(s, list) {
        cJSON *id = cJSON_GetObjectItem(s, "station_id");
        if (n >= max || !cJSON_IsNumber(id)) continue;
        out[n].id = (uint32_t)id->valuedouble;
        copy_str(s, "station_name", out[n].name, sizeof(out[n].name));
        copy_str(s, "product_number", out[n].product, sizeof(out[n].product));
        n++;
    }
    cJSON_Delete(root);
    return n;
}

static bool get_num(cJSON *o, const char *key, double *v)
{
    cJSON *x = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsNumber(x)) return false;
    *v = x->valuedouble;
    return true;
}

int wl_parse_transmitters(const char *json, wl_tx_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *sensors = cJSON_GetObjectItem(root, "sensors");
    if (!cJSON_IsArray(sensors)) {
        cJSON_Delete(root);
        return -1;
    }
    // An outdoor suite is any sensor record with a transmitter id and outdoor
    // temperature or wind. The record layout differs by product (data
    // structure type 10 on the Live, 23 on the Console), so go by the fields.
    int n = 0;
    cJSON *s;
    cJSON_ArrayForEach(s, sensors) {
        cJSON *data = cJSON_GetObjectItem(s, "data");
        cJSON *rec = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
        double tx, v;
        if (!rec || !get_num(rec, "tx_id", &tx) || n >= max) continue;
        wl_tx_t t = {.tx_id = (uint8_t)tx};
        if (get_num(rec, "temp", &v)) {
            t.has_temp = true;
            t.temp_f = (float)v;
        }
        if (get_num(rec, "wind_speed_last", &v)) {
            t.has_wind = true;
            t.wind_mph = (float)v;
            if (get_num(rec, "wind_dir_last", &v)) t.wind_dir = (int)v;
        }
        if (t.has_temp || t.has_wind) out[n++] = t;
    }
    cJSON_Delete(root);
    return n;
}

bool wl_parse_error(const char *json, char *msg, int cap)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *m = cJSON_GetObjectItem(root, "message");
    bool ok = cJSON_IsString(m) && m->valuestring[0];
    if (ok) snprintf(msg, cap, "%s", m->valuestring);
    cJSON_Delete(root);
    return ok;
}

static bool num_f(cJSON *o, const char *key, float *out)
{
    double v;
    if (!get_num(o, key, &v)) return false;
    *out = (float)v;
    return true;
}

bool wl_parse_current(const char *json, int tx, wl_current_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    cJSON *sensors = cJSON_GetObjectItem(root, "sensors");
    bool found = false;
    cJSON *s;
    cJSON_ArrayForEach(s, sensors) {
        cJSON *data = cJSON_GetObjectItem(s, "data");
        cJSON *rec = cJSON_IsArray(data) ? cJSON_GetArrayItem(data, 0) : NULL;
        double v, id;
        if (!rec) continue;
        if (get_num(rec, "bar_sea_level", &v)) {
            out->has_bar = true;
            out->bar_inhg = (float)v;
            num_f(rec, "bar_trend", &out->bar_trend_inhg);
            if (get_num(rec, "ts", &v)) out->bar_when = (time_t)v;
            continue;
        }
        if (found || !get_num(rec, "tx_id", &id) || (tx && (int)id != tx)) continue;
        if (!cJSON_GetObjectItem(rec, "temp") && !cJSON_GetObjectItem(rec, "wind_speed_last")) continue;
        found = true;
        if (get_num(rec, "ts", &v)) out->when = (time_t)v;
        if (num_f(rec, "wind_speed_last", &out->wind_mph)) {
            out->has_wind = true;
            if (get_num(rec, "wind_dir_last", &v)) out->wind_dir = (int)v;
            if (!num_f(rec, "wind_speed_avg_last_10_min", &out->wind_avg10_mph)) out->wind_avg10_mph = out->wind_mph;
            out->wind_dir_avg10 = get_num(rec, "wind_dir_scalar_avg_last_10_min", &v) ? (int)v : out->wind_dir;
            if (!num_f(rec, "wind_speed_hi_last_10_min", &out->gust10_mph)) out->gust10_mph = out->wind_mph;
        }
        out->has_temp = num_f(rec, "temp", &out->temp_f);
        out->has_hum = num_f(rec, "hum", &out->hum_pct);
        out->has_dew = num_f(rec, "dew_point", &out->dew_f);
        // Today's rain is "rainfall_daily_in" on the Live and "rainfall_day_in" on the Console.
        out->has_rain = num_f(rec, "rain_rate_last_in", &out->rain_rate_in_hr);
        if (!num_f(rec, "rainfall_daily_in", &out->rain_today_in)) num_f(rec, "rainfall_day_in", &out->rain_today_in);
    }
    cJSON_Delete(root);
    return found;
}
