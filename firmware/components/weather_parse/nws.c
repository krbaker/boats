// NWS marine forecast text, alerts and observations: see weather_parse.h.

#define _POSIX_C_SOURCE 200809L // strdup, strtok_r under -std=c11

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "weather_parse.h"

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm), so
// there's no need for timegm(), which newlib lacks.
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

static time_t utc_time(int y, int mo, int d, int h, int mi, int s)
{
    return (time_t)days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

time_t wx_parse_iso8601(const char *s)
{
    int y, mo, d, h, mi, sec = 0, n = 0;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) < 6 || !n) return 0;
    const char *z = s + n;
    while (*z == '.' || isdigit((unsigned char)*z)) z++; // fractional seconds
    int off = 0;
    if (*z == '+' || *z == '-') {
        int oh = 0, om = 0;
        if (sscanf(z + 1, "%2d:%2d", &oh, &om) < 1) return 0;
        off = (oh * 60 + om) * 60 * (*z == '-' ? -1 : 1);
    }
    return utc_time(y, mo, d, h, mi, sec) - off;
}

static void copy_str(cJSON *o, const char *key, char *dst, size_t cap)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    snprintf(dst, cap, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

// ---------------------------------------------------------------------------
// Marine forecast text
// ---------------------------------------------------------------------------

// Appends src to dst, collapsing runs of spaces (and dropping leading ones).
static void append_words(char *dst, size_t cap, const char *src)
{
    size_t len = strlen(dst);
    for (; *src && len + 1 < cap; src++) {
        bool space = isspace((unsigned char)*src);
        if (space && (len == 0 || dst[len - 1] == ' ')) continue;
        dst[len++] = space ? ' ' : *src;
    }
    dst[len] = '\0';
}

// Appends a wrapped line, with a space before it.
static void join_line(char *dst, size_t cap, const char *line)
{
    size_t len = strlen(dst);
    if (len && dst[len - 1] != ' ' && len + 1 < cap) {
        dst[len++] = ' ';
        dst[len] = '\0';
    }
    append_words(dst, cap, line);
}

static void trim_end(char *s, const char *chars)
{
    size_t n = strlen(s);
    while (n && (isspace((unsigned char)s[n - 1]) || strchr(chars, s[n - 1]))) s[--n] = '\0';
}

static bool is_zone_line(const char *l)
{
    return isupper((unsigned char)l[0]) && isupper((unsigned char)l[1]) && l[2] == 'Z' && isdigit((unsigned char)l[3]) &&
           isdigit((unsigned char)l[4]) && isdigit((unsigned char)l[5]) && l[6] == '-';
}

bool mf_parse(const char *text, marine_forecast_t *out)
{
    memset(out, 0, sizeof(*out));
    char *buf = strdup(text);
    if (!buf) return false;
    enum { HEADER, NAME, ISSUED, BODY } stage = HEADER;
    enum { NONE, IN_HEADLINE, IN_PERIOD } in = NONE;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        trim_end(line, "\r");
        if (stage == HEADER) {
            int y, mo, d, h, mi;
            if (sscanf(line, "Expires:%4d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi) == 5) out->expires = utc_time(y, mo, d, h, mi, 0);
            if (is_zone_line(line)) {
                snprintf(out->zone, sizeof(out->zone), "%.6s", line);
                stage = NAME;
            }
        } else if (stage == NAME) {
            // The zone's name, possibly over several lines, ends with "-".
            join_line(out->zone_name, sizeof(out->zone_name), line);
            if (line[0] && line[strlen(line) - 1] == '-') {
                trim_end(out->zone_name, "-");
                stage = ISSUED;
            }
        } else if (stage == ISSUED) {
            if (!line[0]) continue;
            snprintf(out->issued, sizeof(out->issued), "%s", line);
            stage = BODY;
        } else {
            if (strncmp(line, "$$", 2) == 0) break;
            if (!line[0]) {
                in = NONE;
                continue;
            }
            if (strncmp(line, "...", 3) == 0 && in != IN_HEADLINE && out->nheadlines < MF_MAX_HEADLINES) {
                in = IN_HEADLINE;
                out->nheadlines++;
                line += 3;
            } else if (line[0] == '.' && line[1] != '.' && out->nperiods < MF_MAX_PERIODS) {
                // ".TUE NIGHT...N wind 5 to 10 kt..."
                char *dots = strstr(line, "...");
                if (!dots) continue;
                in = IN_PERIOD;
                int p = out->nperiods++;
                snprintf(out->period[p].name, sizeof(out->period[p].name), "%.*s", (int)(dots - line - 1), line + 1);
                line = dots + 3;
            }
            if (in == IN_HEADLINE) {
                char *h = out->headline[out->nheadlines - 1];
                join_line(h, sizeof(out->headline[0]), line);
                size_t n = strlen(h);
                if (n >= 3 && strcmp(h + n - 3, "...") == 0) { // the headline's closing dots
                    trim_end(h, ".");
                    in = NONE;
                }
            } else if (in == IN_PERIOD) {
                join_line(out->period[out->nperiods - 1].text, sizeof(out->period[0].text), line);
            }
        }
    }
    free(buf);
    for (int i = 0; i < out->nperiods; i++) trim_end(out->period[i].text, "");
    return out->zone[0] && out->nperiods > 0;
}

// ---------------------------------------------------------------------------
// Alerts
// ---------------------------------------------------------------------------

// NWS wraps alert text at a fixed width. Joins those lines, keeping blank
// lines (paragraphs) and "* WHAT..." style bullets, so the screen can wrap it.
static void unwrap(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p != '\n') continue;
        bool para = p > s && p[-1] == '\n';
        bool next_para = p[1] == '\n' || p[1] == '*' || p[1] == '\0';
        if (!para && !next_para) *p = ' ';
    }
}

int wx_parse_alerts(const char *json, wx_alert_t *out, int max)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *features = cJSON_GetObjectItem(root, "features");
    if (!cJSON_IsArray(features)) {
        cJSON_Delete(root);
        return -1;
    }
    int n = 0;
    cJSON *f;
    cJSON_ArrayForEach(f, features) {
        cJSON *p = cJSON_GetObjectItem(f, "properties");
        cJSON *status = cJSON_GetObjectItem(p, "status"), *type = cJSON_GetObjectItem(p, "messageType");
        if (n >= max || !cJSON_IsString(status) || strcmp(status->valuestring, "Actual") != 0) continue;
        if (cJSON_IsString(type) && strcmp(type->valuestring, "Cancel") == 0) continue;
        wx_alert_t *a = &out[n++];
        memset(a, 0, sizeof(*a));
        copy_str(p, "event", a->event, sizeof(a->event));
        copy_str(p, "headline", a->headline, sizeof(a->headline));
        copy_str(p, "description", a->description, sizeof(a->description));
        cJSON *instr = cJSON_GetObjectItem(p, "instruction");
        if (cJSON_IsString(instr) && instr->valuestring[0]) {
            size_t len = strlen(a->description);
            snprintf(a->description + len, sizeof(a->description) - len, "%s%s", len ? "\n\n" : "", instr->valuestring);
        }
        unwrap(a->description);
        cJSON *ends = cJSON_GetObjectItem(p, "ends");
        a->ends = wx_parse_iso8601(cJSON_IsString(ends) ? ends->valuestring : NULL);
        if (!a->ends) {
            cJSON *exp = cJSON_GetObjectItem(p, "expires");
            a->ends = wx_parse_iso8601(cJSON_IsString(exp) ? exp->valuestring : NULL);
        }
        a->flag = strstr(a->event, "Warning") || strstr(a->event, "Watch") || strstr(a->event, "Advisory");
    }
    cJSON_Delete(root);
    return n;
}

// ---------------------------------------------------------------------------
// Observations and stations
// ---------------------------------------------------------------------------

static cloud_t cloud_amount(const char *a)
{
    if (!strcmp(a, "CLR") || !strcmp(a, "SKC")) return CLOUD_CLEAR;
    if (!strcmp(a, "FEW")) return CLOUD_FEW;
    if (!strcmp(a, "SCT")) return CLOUD_SCATTERED;
    if (!strcmp(a, "BKN")) return CLOUD_BROKEN;
    if (!strcmp(a, "OVC") || !strcmp(a, "VV")) return CLOUD_OVERCAST; // VV: sky obscured (fog)
    return CLOUD_UNKNOWN;
}

static precip_t precip_word(const char *w)
{
    if (strstr(w, "snow") || strstr(w, "ice_pellets") || strstr(w, "ice_crystals") || strstr(w, "Snow") ||
        strstr(w, "Sleet") || strstr(w, "Flurries")) {
        return PRECIP_SNOW;
    }
    if (strstr(w, "rain") || strstr(w, "drizzle") || strstr(w, "thunderstorm") || strstr(w, "Rain") ||
        strstr(w, "Drizzle") || strstr(w, "Showers") || strstr(w, "Thunderstorm")) {
        return PRECIP_RAIN;
    }
    return PRECIP_NONE;
}

bool wx_parse_observation(const char *json, wx_obs_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(json);
    cJSON *p = cJSON_GetObjectItem(root, "properties");
    cJSON *ts = cJSON_GetObjectItem(p, "timestamp");
    if (!cJSON_IsString(ts)) {
        cJSON_Delete(root);
        return false;
    }
    out->when = wx_parse_iso8601(ts->valuestring);
    copy_str(p, "textDescription", out->text, sizeof(out->text));
    cJSON *layer;
    cJSON_ArrayForEach(layer, cJSON_GetObjectItem(p, "cloudLayers")) {
        cJSON *amount = cJSON_GetObjectItem(layer, "amount");
        if (!cJSON_IsString(amount)) continue;
        cloud_t c = cloud_amount(amount->valuestring);
        if (c > out->cloud) out->cloud = c;
    }
    cJSON *w;
    cJSON_ArrayForEach(w, cJSON_GetObjectItem(p, "presentWeather")) {
        cJSON *kind = cJSON_GetObjectItem(w, "weather");
        if (!cJSON_IsString(kind)) continue;
        precip_t pr = precip_word(kind->valuestring);
        if (pr > out->precip) out->precip = pr;
    }
    precip_t from_text = precip_word(out->text); // "Light Snow" with an empty presentWeather
    if (from_text > out->precip) out->precip = from_text;
    cJSON_Delete(root);
    return out->when != 0;
}

bool wx_parse_points(const char *json, char *stations_url, int cap)
{
    cJSON *root = cJSON_Parse(json);
    copy_str(cJSON_GetObjectItem(root, "properties"), "observationStations", stations_url, cap);
    cJSON_Delete(root);
    return stations_url[0] != '\0';
}

bool wx_parse_first_station(const char *json, char *id, int cap)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "features"), 0);
    copy_str(cJSON_GetObjectItem(first, "properties"), "stationIdentifier", id, cap);
    cJSON_Delete(root);
    return id[0] != '\0';
}
