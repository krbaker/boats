#include "settings_json.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *const MODE_NAMES[] = {"label", "vessel"}; // filter_mode_t
static const char *const ORDER_NAMES[] = {"speed", "size"};  // draw_order_t

bool settings_json_str(const cJSON *o, const char *key, char *dst, size_t cap, const char **bad)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v) return true;
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= cap) {
        *bad = key;
        return false;
    }
    strcpy(dst, v->valuestring);
    return true;
}

bool settings_alnum(const char *s, size_t cap)
{
    if (strlen(s) >= cap) return false;
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s)) return false;
    }
    return true;
}

static bool valid_host(const char *h)
{
    for (; *h; h++) {
        if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-') return false;
    }
    return true;
}

// Empty, or a plain-HTTP URL (dump1090 doesn't do HTTPS).
static bool valid_http_url(const char *u)
{
    if (!u[0]) return true;
    if (strncmp(u, "http://", 7) != 0 || !u[7] || u[7] == '/') return false;
    for (; *u; u++) {
        if (*u <= ' ' || *u > '~') return false;
    }
    return true;
}

static bool valid_zone(const char *z)
{
    if (!z[0]) return true;
    return strlen(z) == 6 && isupper((unsigned char)z[0]) && isupper((unsigned char)z[1]) && z[2] == 'Z' &&
           isdigit((unsigned char)z[3]) && isdigit((unsigned char)z[4]) && isdigit((unsigned char)z[5]);
}

static bool parse_fwd(const cJSON *o, fwd_dest_t *d, int n, char *msg, size_t cap)
{
    if (!cJSON_IsObject(o)) {
        snprintf(msg, cap, "invalid retransmit destination %d", n);
        return false;
    }
    cJSON *v;
    if ((v = cJSON_GetObjectItem(o, "enabled"))) {
        if (!cJSON_IsBool(v)) goto bad;
        d->enabled = cJSON_IsTrue(v);
    }
    if ((v = cJSON_GetObjectItem(o, "proto"))) {
        if (cJSON_IsString(v) && strcmp(v->valuestring, "udp") == 0) d->proto = FWD_UDP;
        else if (cJSON_IsString(v) && strcmp(v->valuestring, "tcp") == 0) d->proto = FWD_TCP;
        else goto bad;
    }
    if ((v = cJSON_GetObjectItem(o, "host"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(d->host) || !valid_host(v->valuestring)) {
            snprintf(msg, cap, "destination %d: enter an IP address or host name", n);
            return false;
        }
        strcpy(d->host, v->valuestring);
    }
    if ((v = cJSON_GetObjectItem(o, "port"))) {
        if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 65535) {
            snprintf(msg, cap, "destination %d: the port must be 1-65535", n);
            return false;
        }
        d->port = v->valueint;
    }
    if (d->enabled && (!d->host[0] || !d->port)) {
        snprintf(msg, cap, "destination %d: enter an address and port before turning it on", n);
        return false;
    }
    return true;
bad:
    snprintf(msg, cap, "invalid retransmit destination %d", n);
    return false;
}

// One of names[] (n of them) by name, if o has key. False if it isn't one.
static bool get_enum(const cJSON *o, const char *key, const char *const *names, int n, uint8_t *dst)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v) return true;
    for (int i = 0; i < n; i++) {
        if (cJSON_IsString(v) && strcmp(v->valuestring, names[i]) == 0) {
            *dst = i;
            return true;
        }
    }
    return false;
}

static bool get_bool(const cJSON *o, const char *key, bool *dst, const char **bad)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v) return true;
    if (!cJSON_IsBool(v)) {
        *bad = key;
        return false;
    }
    *dst = cJSON_IsTrue(v);
    return true;
}

// A number in [lo, hi], if o has key.
static bool get_num(const cJSON *o, const char *key, double lo, double hi, double *dst, const char **bad)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    if (!v) return false;
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        *bad = key;
        return false;
    }
    *dst = v->valuedouble;
    return true;
}

static void parse_filter(const cJSON *o, vessel_filter_t *f, const char **bad)
{
    if (!cJSON_IsObject(o)) {
        *bad = "filter";
        return;
    }
    double d;
    if (get_num(o, "min_len_ft", 0, 1000, &d, bad)) f->min_len_ft = (uint16_t)d;
    if (get_num(o, "min_speed_kn", 0, 100, &d, bad)) f->min_speed_kn = (uint8_t)d;
    if (!get_enum(o, "mode", MODE_NAMES, 2, &f->mode)) *bad = "filter mode";
    if (!get_enum(o, "order", ORDER_NAMES, 2, &f->order)) *bad = "filter order";
    get_bool(o, "include_unknown", &f->include_unknown, bad);
    get_bool(o, "label_name", &f->label_name, bad);
    get_bool(o, "label_speed", &f->label_speed, bad);
}

cJSON *settings_to_json(const settings_t *s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ais_host", s->ais_host);
    cJSON_AddNumberToObject(o, "ais_port", s->ais_port);
    cJSON_AddStringToObject(o, "tide_station", s->tide_station);
    cJSON_AddStringToObject(o, "tz_name", s->tz_name);
    cJSON_AddStringToObject(o, "tz_posix", s->tz_posix);
    cJSON_AddNumberToObject(o, "center_lat", s->center_lat);
    cJSON_AddNumberToObject(o, "center_lon", s->center_lon);
    cJSON_AddNumberToObject(o, "zoom_index", s->zoom_index);
    cJSON_AddNumberToObject(o, "brightness", s->brightness);
    cJSON_AddBoolToObject(o, "net_enabled", s->net_enabled);
    cJSON_AddBoolToObject(o, "sdr_enabled", s->sdr_enabled);
    cJSON_AddNumberToObject(o, "sdr_gain", s->sdr_gain);  // tenths of a dB, -1 tuner AGC, -2 best fixed
    cJSON_AddNumberToObject(o, "sdr_ppm", s->sdr_ppm_tenths / 10.0);
    cJSON_AddBoolToObject(o, "sdr_bias_tee", s->sdr_bias_tee);
    cJSON_AddStringToObject(o, "wx_zone", s->wx_zone);
    cJSON_AddStringToObject(o, "wx_buoy", s->wx_buoy);
    cJSON_AddStringToObject(o, "wx_obs", s->wx_obs);
    cJSON_AddStringToObject(o, "wl_key", s->wl_key);
    cJSON_AddNumberToObject(o, "wl_station", s->wl_station);
    cJSON_AddNumberToObject(o, "wl_tx", s->wl_tx);
    cJSON_AddBoolToObject(o, "adsb_enabled", s->adsb_enabled);
    cJSON_AddStringToObject(o, "adsb_url", s->adsb_url);
    cJSON_AddNumberToObject(o, "adsb_range_nm", s->adsb_range_nm);
    cJSON *fa = cJSON_AddArrayToObject(o, "retransmit");
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "enabled", s->fwd[i].enabled);
        cJSON_AddStringToObject(d, "proto", s->fwd[i].proto == FWD_TCP ? "tcp" : "udp");
        cJSON_AddStringToObject(d, "host", s->fwd[i].host);
        cJSON_AddNumberToObject(d, "port", s->fwd[i].port);
        cJSON_AddItemToArray(fa, d);
    }
    cJSON *f = cJSON_AddObjectToObject(o, "filter");
    cJSON_AddNumberToObject(f, "min_len_ft", s->filter.min_len_ft);
    cJSON_AddNumberToObject(f, "min_speed_kn", s->filter.min_speed_kn);
    cJSON_AddStringToObject(f, "mode", MODE_NAMES[s->filter.mode & 1]);
    cJSON_AddStringToObject(f, "order", ORDER_NAMES[s->filter.order & 1]);
    cJSON_AddBoolToObject(f, "include_unknown", s->filter.include_unknown);
    cJSON_AddBoolToObject(f, "label_name", s->filter.label_name);
    cJSON_AddBoolToObject(f, "label_speed", s->filter.label_speed);
    cJSON_AddNumberToObject(o, "map_created", s->map_created); // the chart whose centre was last applied
    return o;
}

bool settings_from_json(const cJSON *o, settings_t *s, char *msg, size_t cap)
{
    const char *bad = NULL;
    double d;
    msg[0] = '\0';
    settings_json_str(o, "ais_host", s->ais_host, sizeof(s->ais_host), &bad);
    settings_json_str(o, "tide_station", s->tide_station, sizeof(s->tide_station), &bad);
    settings_json_str(o, "tz_name", s->tz_name, sizeof(s->tz_name), &bad);
    settings_json_str(o, "tz_posix", s->tz_posix, sizeof(s->tz_posix), &bad);
    if (get_num(o, "ais_port", 0, 65535, &d, &bad)) s->ais_port = (uint16_t)d;
    if (get_num(o, "center_lat", -90, 90, &d, &bad)) s->center_lat = d;
    if (get_num(o, "center_lon", -180, 180, &d, &bad)) s->center_lon = d;
    if (get_num(o, "zoom_index", 0, 255, &d, &bad)) s->zoom_index = (uint8_t)d;
    if (get_num(o, "brightness", 5, 100, &d, &bad)) s->brightness = (uint8_t)d;
    get_bool(o, "net_enabled", &s->net_enabled, &bad);
    get_bool(o, "sdr_enabled", &s->sdr_enabled, &bad);
    get_bool(o, "sdr_bias_tee", &s->sdr_bias_tee, &bad);
    if (get_num(o, "sdr_gain", SDR_GAIN_BEST, 500, &d, &bad)) s->sdr_gain = (int16_t)d;
    if (get_num(o, "sdr_ppm", -200, 200, &d, &bad)) s->sdr_ppm_tenths = (int16_t)lround(d * 10);
    for (const char *c = s->tide_station; *c; c++) {
        if (*c < '0' || *c > '9') bad = "tide_station";
    }
    settings_json_str(o, "wx_zone", s->wx_zone, sizeof(s->wx_zone), &bad);
    if (!valid_zone(s->wx_zone)) bad = "wx_zone";
    settings_json_str(o, "wx_buoy", s->wx_buoy, sizeof(s->wx_buoy), &bad);
    if (!settings_alnum(s->wx_buoy, sizeof(s->wx_buoy))) bad = "wx_buoy";
    settings_json_str(o, "wx_obs", s->wx_obs, sizeof(s->wx_obs), &bad);
    if (!settings_alnum(s->wx_obs, sizeof(s->wx_obs))) bad = "wx_obs";
    settings_json_str(o, "wl_key", s->wl_key, sizeof(s->wl_key), &bad);
    if (!settings_alnum(s->wl_key, sizeof(s->wl_key))) bad = "wl_key";
    if (get_num(o, "wl_station", 0, 4294967295.0, &d, &bad)) s->wl_station = (uint32_t)d;
    if (get_num(o, "wl_tx", 0, 255, &d, &bad)) s->wl_tx = (uint8_t)d;
    if (get_num(o, "map_created", 0, 4294967295.0, &d, &bad)) s->map_created = (uint32_t)d;
    get_bool(o, "adsb_enabled", &s->adsb_enabled, &bad);
    settings_json_str(o, "adsb_url", s->adsb_url, sizeof(s->adsb_url), &bad);
    if (get_num(o, "adsb_range_nm", 1, ADSB_RANGE_MAX_NM, &d, &bad)) s->adsb_range_nm = (uint8_t)d;

    cJSON *v;
    if ((v = cJSON_GetObjectItem(o, "filter"))) parse_filter(v, &s->filter, &bad);
    if ((v = cJSON_GetObjectItem(o, "retransmit"))) {
        if (!cJSON_IsArray(v) || cJSON_GetArraySize(v) > FWD_DEST_MAX) {
            bad = "retransmit";
        } else {
            for (int i = 0; i < cJSON_GetArraySize(v) && !msg[0]; i++) {
                parse_fwd(cJSON_GetArrayItem(v, i), &s->fwd[i], i + 1, msg, cap);
            }
        }
    }
    if (!msg[0] && !valid_http_url(s->adsb_url)) {
        snprintf(msg, cap, "the aircraft.json address must start with http://");
    } else if (!msg[0] && s->adsb_enabled && !s->adsb_url[0]) {
        snprintf(msg, cap, "enter the dump1090 aircraft.json address before turning aircraft on");
    }
    if (bad) snprintf(msg, cap, "invalid value for %s", bad);
    return !msg[0];
}
