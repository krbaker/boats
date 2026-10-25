#include "adsb_parse.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// A cursor over the text. Every step checks for the end, so a file cut short
// (or not JSON at all) just stops the reading.
typedef struct {
    const char *p, *end;
} scan_t;

static void ws(scan_t *s)
{
    while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++;
}

static bool peek(scan_t *s, char c)
{
    ws(s);
    return s->p < s->end && *s->p == c;
}

static bool eat(scan_t *s, char c)
{
    if (!peek(s, c)) return false;
    s->p++;
    return true;
}

// A string into buf (truncated to fit; NULL to skip it). Escapes come out as
// the character escaped, \uXXXX as '?': nothing we read needs more.
static bool str(scan_t *s, char *buf, size_t cap)
{
    if (!eat(s, '"')) return false;
    size_t n = 0;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\') {
            if (s->p >= s->end) return false;
            c = *s->p++;
            if (c == 'u') {
                if (s->end - s->p < 4) return false;
                s->p += 4;
                c = '?';
            }
        }
        if (buf && n + 1 < cap) buf[n++] = c;
    }
    if (!eat(s, '"')) return false;
    if (buf && cap) buf[n] = '\0';
    return true;
}

static bool num(scan_t *s, double *v)
{
    ws(s);
    char tmp[32];
    size_t n = 0;
    while (s->p + n < s->end && n < sizeof(tmp) - 1 && s->p[n] && strchr("+-0123456789.eE", s->p[n])) n++;
    if (!n) return false;
    memcpy(tmp, s->p, n);
    tmp[n] = '\0';
    char *e;
    *v = strtod(tmp, &e);
    if (e != tmp + n) return false;
    s->p += n;
    return true;
}

// Any value, unread.
static bool skip(scan_t *s)
{
    ws(s);
    if (s->p >= s->end) return false;
    if (*s->p == '"') return str(s, NULL, 0);
    if (*s->p == '{' || *s->p == '[') {
        int depth = 0;
        while (s->p < s->end) {
            char c = *s->p;
            if (c == '"') {
                if (!str(s, NULL, 0)) return false;
                continue;
            }
            s->p++;
            if (c == '{' || c == '[') depth++;
            else if ((c == '}' || c == ']') && --depth == 0) return true;
        }
        return false;
    }
    const char *start = s->p; // a number, true, false or null
    while (s->p < s->end && *s->p && !strchr(",}] \t\r\n", *s->p)) s->p++;
    return s->p > start && s->p < s->end;
}

// A number if the value is one (true), else the value skipped (false). *ok
// goes false if the text ends.
static bool num_or_skip(scan_t *s, double *v, bool *ok)
{
    ws(s);
    if (s->p < s->end && (*s->p == '-' || isdigit((unsigned char)*s->p))) {
        if (num(s, v)) return true;
    }
    *ok = skip(s);
    return false;
}

static uint8_t emergency_of(const char *e)
{
    static const char *const names[] = {"none", "general", "lifeguard", "minfuel", "nordo", "unlawful", "downed", "reserved"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(e, names[i]) == 0) return (uint8_t)i;
    }
    return ADSB_EMERG_NONE;
}

// Rates: barometric first, else geometric; altitude likewise.
enum { RATE_NONE, RATE_GEOM, RATE_BARO };

typedef struct {
    bool have_lat, have_lon, alt_baro;
    int rate_src;
} seen_t;

// One field of an aircraft. False if the text ran out.
static bool field(scan_t *s, const char *key, adsb_aircraft_t *a, seen_t *seen)
{
    bool ok = true;
    double d;
    char buf[16];
    if (strcmp(key, "hex") == 0) {
        if (!peek(s, '"')) return skip(s);
        if (!str(s, buf, sizeof(buf))) return false;
        const char *h = buf;
        if (*h == '~') {
            a->non_icao = true;
            h++;
        }
        a->addr = (uint32_t)strtoul(h, NULL, 16) & 0xFFFFFF;
    } else if (strcmp(key, "flight") == 0 || strcmp(key, "t") == 0 || strcmp(key, "r") == 0) {
        char *dst = key[0] == 'f' ? a->flight : key[0] == 't' ? a->type : a->reg;
        size_t cap = key[0] == 'f' ? sizeof(a->flight) : key[0] == 't' ? sizeof(a->type) : sizeof(a->reg);
        if (!peek(s, '"')) return skip(s);
        if (!str(s, dst, cap)) return false;
        for (size_t n = strlen(dst); n && dst[n - 1] == ' '; n--) dst[n - 1] = '\0';
    } else if (strcmp(key, "category") == 0) {
        if (!peek(s, '"')) return skip(s);
        if (!str(s, buf, sizeof(buf))) return false;
        if (buf[0] >= 'A' && buf[0] <= 'D' && buf[1] >= '0' && buf[1] <= '7' && !buf[2]) {
            a->category = (uint8_t)((0xA + buf[0] - 'A') << 4 | (buf[1] - '0'));
        }
    } else if (strcmp(key, "squawk") == 0) {
        if (!peek(s, '"')) return skip(s);
        if (!str(s, buf, sizeof(buf))) return false;
        if (strlen(buf) == 4 && strspn(buf, "01234567") == 4) {
            a->squawk = (uint16_t)atoi(buf);
            a->fields |= ADSB_F_SQUAWK;
        }
    } else if (strcmp(key, "emergency") == 0) {
        if (!peek(s, '"')) return skip(s);
        if (!str(s, buf, sizeof(buf))) return false;
        a->emergency = emergency_of(buf);
    } else if (strcmp(key, "alt_baro") == 0 || strcmp(key, "altitude") == 0 || strcmp(key, "alt_geom") == 0) {
        bool baro = key[4] != 'g';
        if (peek(s, '"')) { // "ground"
            if (!str(s, buf, sizeof(buf))) return false;
            if (baro && strcmp(buf, "ground") == 0) {
                a->fields = (a->fields | ADSB_F_GROUND) & ~ADSB_F_ALT;
                seen->alt_baro = true;
            }
        } else if (num_or_skip(s, &d, &ok) && (baro || !seen->alt_baro)) {
            a->alt_ft = (int32_t)d;
            a->fields = (a->fields | ADSB_F_ALT) & ~ADSB_F_GROUND;
            seen->alt_baro |= baro;
        }
    } else if (strcmp(key, "lat") == 0 || strcmp(key, "lon") == 0) {
        if (num_or_skip(s, &d, &ok)) {
            if (key[1] == 'a') a->lat = d, seen->have_lat = true;
            else a->lon = d, seen->have_lon = true;
        }
    } else if (strcmp(key, "gs") == 0 || strcmp(key, "speed") == 0) {
        if (num_or_skip(s, &d, &ok)) {
            a->gs_kn = (float)d;
            a->fields |= ADSB_F_GS;
        }
    } else if (strcmp(key, "track") == 0) {
        if (num_or_skip(s, &d, &ok)) {
            a->track_deg = (float)d;
            a->fields |= ADSB_F_TRACK;
        }
    } else if (strcmp(key, "baro_rate") == 0 || strcmp(key, "vert_rate") == 0 || strcmp(key, "geom_rate") == 0) {
        int src = key[0] == 'g' ? RATE_GEOM : RATE_BARO;
        if (num_or_skip(s, &d, &ok) && src >= seen->rate_src) {
            a->vrate_fpm = (int16_t)(d < -32000 ? -32000 : d > 32000 ? 32000 : d);
            a->fields |= ADSB_F_VRATE;
            seen->rate_src = src;
        }
    } else if (strcmp(key, "seen_pos") == 0) {
        if (num_or_skip(s, &d, &ok)) a->seen_pos_s = (float)d;
    } else if (strcmp(key, "seen") == 0) {
        if (num_or_skip(s, &d, &ok)) a->seen_s = (float)d;
    } else {
        return skip(s);
    }
    return ok;
}

static bool aircraft(scan_t *s, adsb_aircraft_t *a)
{
    memset(a, 0, sizeof(*a));
    seen_t seen = {0};
    if (!eat(s, '{')) return false;
    if (!eat(s, '}')) {
        do {
            char key[16];
            if (!str(s, key, sizeof(key)) || !eat(s, ':') || !field(s, key, a, &seen)) return false;
        } while (eat(s, ','));
        if (!eat(s, '}')) return false;
    }
    if (seen.have_lat && seen.have_lon) a->fields |= ADSB_F_POS;
    return true;
}

int adsb_parse(const char *json, size_t len, adsb_cb_t cb, void *ctx, double *now)
{
    scan_t s = {json, json + len};
    *now = 0;
    if (!eat(&s, '{')) return -1;
    do {
        char key[16];
        if (!str(&s, key, sizeof(key)) || !eat(&s, ':')) return -1;
        if (strcmp(key, "now") == 0) {
            bool ok = true;
            if (!num_or_skip(&s, now, &ok) && !ok) return -1;
        } else if (strcmp(key, "aircraft") == 0) {
            if (!eat(&s, '[')) return -1;
            int n = 0;
            if (eat(&s, ']')) return 0;
            do {
                adsb_aircraft_t a;
                if (!aircraft(&s, &a)) break; // cut short: what came before stands
                n++;
                if (!cb(ctx, &a)) break;
            } while (eat(&s, ','));
            return n;
        } else if (!skip(&s)) {
            return -1;
        }
    } while (eat(&s, ','));
    return -1;
}

adsb_kind_t adsb_kind(const adsb_aircraft_t *a)
{
    static const adsb_kind_t a_set[8] = {ADSB_KIND_UNKNOWN, ADSB_KIND_LIGHT, ADSB_KIND_SMALL, ADSB_KIND_LARGE,
                                         ADSB_KIND_HIGH_VORTEX, ADSB_KIND_HEAVY, ADSB_KIND_FAST, ADSB_KIND_ROTOR};
    static const adsb_kind_t b_set[8] = {ADSB_KIND_UNKNOWN, ADSB_KIND_GLIDER, ADSB_KIND_BALLOON, ADSB_KIND_PARACHUTE,
                                         ADSB_KIND_ULTRALIGHT, ADSB_KIND_UNKNOWN, ADSB_KIND_UAV, ADSB_KIND_UNKNOWN};
    uint8_t set = a->category >> 4, n = a->category & 7;
    if (set == 0xA) return a_set[n];
    if (set == 0xB) return b_set[n];
    if (set == 0xC && n) return ADSB_KIND_SURFACE;
    return ADSB_KIND_UNKNOWN;
}

bool adsb_is_emergency(const adsb_aircraft_t *a)
{
    if ((a->fields & ADSB_F_SQUAWK) && (a->squawk == 7500 || a->squawk == 7600 || a->squawk == 7700)) return true;
    return a->emergency == ADSB_EMERG_GENERAL || a->emergency == ADSB_EMERG_NORDO ||
           a->emergency == ADSB_EMERG_UNLAWFUL || a->emergency == ADSB_EMERG_DOWNED;
}

const char *adsb_emergency_str(const adsb_aircraft_t *a)
{
    if (a->fields & ADSB_F_SQUAWK) {
        if (a->squawk == 7500) return "Hijack";
        if (a->squawk == 7600) return "Radio failure";
        if (a->squawk == 7700) return "Emergency";
    }
    switch (a->emergency) {
    case ADSB_EMERG_GENERAL: return "Emergency";
    case ADSB_EMERG_LIFEGUARD: return "Medical priority";
    case ADSB_EMERG_MINFUEL: return "Minimum fuel";
    case ADSB_EMERG_NORDO: return "Radio failure";
    case ADSB_EMERG_UNLAWFUL: return "Hijack";
    case ADSB_EMERG_DOWNED: return "Downed aircraft";
    default: return NULL;
    }
}

const char *adsb_category_str(uint8_t category)
{
    switch (category) {
    case 0xA1: return "Light (under 15,500 lb)";
    case 0xA2: return "Small (15,500-75,000 lb)";
    case 0xA3: return "Large (75,000-300,000 lb)";
    case 0xA4: return "Large, high vortex (B757)";
    case 0xA5: return "Heavy (over 300,000 lb)";
    case 0xA6: return "High performance";
    case 0xA7: return "Helicopter";
    case 0xB1: return "Glider";
    case 0xB2: return "Balloon / airship";
    case 0xB3: return "Parachutist";
    case 0xB4: return "Ultralight / hang glider";
    case 0xB6: return "Drone (UAV)";
    case 0xB7: return "Space vehicle";
    case 0xC1: return "Emergency vehicle";
    case 0xC2: return "Service vehicle";
    case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7: return "Obstacle";
    default: return "";
    }
}
