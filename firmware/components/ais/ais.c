#include "ais.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Sentence framing
// ---------------------------------------------------------------------------

void ais_parser_init(ais_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Locate "!xxVDM," or "!xxVDO," anywhere in the line.
static const char *find_sentence(const char *line)
{
    for (const char *s = strchr(line, '!'); s; s = strchr(s + 1, '!')) {
        if (s[1] && s[2] && s[3] == 'V' && s[4] == 'D' && (s[5] == 'M' || s[5] == 'O') && s[6] == ',') {
            return s;
        }
    }
    return NULL;
}

static ais_frag_slot_t *find_slot(ais_parser_t *p, int seq_id, char channel, uint8_t count, uint32_t now_ms)
{
    ais_frag_slot_t *free_slot = NULL;
    ais_frag_slot_t *oldest = &p->slots[0];
    for (int i = 0; i < AIS_FRAG_SLOTS; i++) {
        ais_frag_slot_t *s = &p->slots[i];
        if (s->in_use && (uint32_t)(now_ms - s->started_ms) > AIS_FRAG_TIMEOUT_MS) {
            s->in_use = false;
        }
        if (s->in_use && s->seq_id == seq_id && s->channel == channel && s->count == count) {
            return s;
        }
        if (!s->in_use && !free_slot) {
            free_slot = s;
        }
        if (s->started_ms - oldest->started_ms > 0x80000000u) {
            oldest = s;
        }
    }
    ais_frag_slot_t *s = free_slot ? free_slot : oldest;
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    s->seq_id = seq_id;
    s->channel = channel;
    s->count = count;
    s->started_ms = now_ms;
    return s;
}

ais_result_t ais_parse_line(ais_parser_t *p, const char *line, uint32_t now_ms, ais_report_t *out)
{
    p->stats.lines++;

    const char *s = find_sentence(line);
    if (!s) {
        p->stats.ignored++;
        return AIS_RESULT_IGNORED;
    }

    // Checksum: XOR of everything between '!' and '*'.
    const char *star = strchr(s, '*');
    if (!star || hexval(star[1]) < 0 || hexval(star[2]) < 0) {
        p->stats.bad_checksum++;
        return AIS_RESULT_BAD_CHECKSUM;
    }
    uint8_t sum = 0;
    for (const char *c = s + 1; c < star; c++) {
        sum ^= (uint8_t)*c;
    }
    if (sum != (uint8_t)(hexval(star[1]) << 4 | hexval(star[2]))) {
        p->stats.bad_checksum++;
        return AIS_RESULT_BAD_CHECKSUM;
    }

    // Split "!AIVDM,count,num,seq,chan,payload,fill" into fields.
    char buf[128];
    size_t len = (size_t)(star - s);
    if (len >= sizeof(buf)) {
        p->stats.ignored++;
        return AIS_RESULT_IGNORED;
    }
    memcpy(buf, s, len);
    buf[len] = '\0';

    char *field[7];
    int nf = 0;
    char *cur = buf;
    while (nf < 7) {
        field[nf++] = cur;
        char *comma = strchr(cur, ',');
        if (!comma) break;
        *comma = '\0';
        cur = comma + 1;
    }
    if (nf != 7) {
        p->stats.ignored++;
        return AIS_RESULT_IGNORED;
    }

    int count = atoi(field[1]);
    int num = atoi(field[2]);
    int seq_id = field[3][0] ? atoi(field[3]) : -1;
    char channel = field[4][0];
    const char *payload = field[5];
    int fill_bits = field[6][0] ? atoi(field[6]) : 0;

    if (count < 1 || count > AIS_MAX_FRAGMENTS || num < 1 || num > count || strlen(payload) >= 64 ||
        fill_bits < 0 || fill_bits > 5) {
        p->stats.ignored++;
        return AIS_RESULT_IGNORED;
    }

    ais_result_t r;
    if (count == 1) {
        r = ais_decode_payload(payload, fill_bits, out);
    } else {
        ais_frag_slot_t *slot = find_slot(p, seq_id, channel, (uint8_t)count, now_ms);
        strcpy(slot->part[num - 1], payload);
        slot->received |= (uint16_t)(1u << (num - 1));
        if (num == count) {
            slot->fill_bits = fill_bits;
        }
        if (slot->received != (uint16_t)((1u << count) - 1)) {
            return AIS_RESULT_PENDING;
        }
        char full[AIS_MAX_PAYLOAD];
        full[0] = '\0';
        for (int i = 0; i < count; i++) {
            strcat(full, slot->part[i]);
        }
        slot->in_use = false;
        r = ais_decode_payload(full, slot->fill_bits, out);
    }

    if (r == AIS_RESULT_REPORT) {
        p->stats.reports++;
    } else {
        p->stats.unsupported++;
    }
    p->stats.by_type[out->msg_type <= 27 ? out->msg_type : 0]++;
    return r;
}

// ---------------------------------------------------------------------------
// Payload decoding
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t bits[AIS_MAX_PAYLOAD * 6 / 8 + 1];
    int nbits;
} bitbuf_t;

static bool unarmour(const char *payload, int fill_bits, bitbuf_t *b)
{
    memset(b, 0, sizeof(*b));
    int pos = 0;
    for (const char *c = payload; *c; c++) {
        int v = *c - 48;
        if (v > 40) v -= 8;
        if (v < 0 || v > 63) return false;
        for (int i = 5; i >= 0; i--) {
            if (v & (1 << i)) {
                b->bits[pos >> 3] |= (uint8_t)(0x80 >> (pos & 7));
            }
            pos++;
        }
    }
    b->nbits = pos - fill_bits;
    return b->nbits > 0;
}

static uint32_t get_u(const bitbuf_t *b, int start, int len)
{
    uint32_t v = 0;
    for (int i = start; i < start + len; i++) {
        v <<= 1;
        if (i < b->nbits && (b->bits[i >> 3] & (0x80 >> (i & 7)))) {
            v |= 1;
        }
    }
    return v;
}

static int32_t get_s(const bitbuf_t *b, int start, int len)
{
    uint32_t v = get_u(b, start, len);
    if (v & (1u << (len - 1))) {
        v |= ~((1u << len) - 1);
    }
    return (int32_t)v;
}

static void get_str(const bitbuf_t *b, int start, int nchars, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < nchars && n + 1 < cap; i++) {
        if (start + (i + 1) * 6 > b->nbits) break;
        uint32_t v = get_u(b, start + i * 6, 6);
        if (v == 0) break; // '@' terminates
        out[n++] = (char)(v < 32 ? v + 64 : v);
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

static void set_pos(ais_report_t *r, int32_t lon_raw, int32_t lat_raw, double div)
{
    double lon = lon_raw / div;
    double lat = lat_raw / div;
    if (lon >= -180.0 && lon <= 180.0 && lat >= -90.0 && lat <= 90.0 && !(lon == 0.0 && lat == 0.0)) {
        r->lon = lon;
        r->lat = lat;
        r->fields |= AIS_F_POS;
    }
}

static void set_sog(ais_report_t *r, uint32_t raw)
{
    if (raw < 1023) {
        r->sog_kn = raw / 10.0f;
        r->fields |= AIS_F_SOG;
    }
}

static void set_cog(ais_report_t *r, uint32_t raw)
{
    if (raw < 3600) {
        r->cog_deg = raw / 10.0f;
        r->fields |= AIS_F_COG;
    }
}

static void set_heading(ais_report_t *r, uint32_t raw)
{
    if (raw < 360) {
        r->heading = (uint16_t)raw;
        r->fields |= AIS_F_HEADING;
    }
}

static void set_dims(ais_report_t *r, const bitbuf_t *b, int start)
{
    r->dim_a = (uint16_t)get_u(b, start, 9);
    r->dim_b = (uint16_t)get_u(b, start + 9, 9);
    r->dim_c = (uint8_t)get_u(b, start + 18, 6);
    r->dim_d = (uint8_t)get_u(b, start + 24, 6);
    if (r->dim_a + r->dim_b > 0) {
        r->fields |= AIS_F_DIM;
    }
}

static void set_str_field(ais_report_t *r, uint32_t flag, char *dst, size_t cap, const bitbuf_t *b, int start, int nchars)
{
    get_str(b, start, nchars, dst, cap);
    if (dst[0]) {
        r->fields |= flag;
    }
}

// ---------------------------------------------------------------------------
// Type 8 weather and water: IMO289 met/hydro (DAC 1 FI 31) and the US
// environmental message (DAC 367 FI 33). Values outside their physical range
// are taken as "not available".
// ---------------------------------------------------------------------------

static uint8_t met_trend(uint32_t raw)
{
    return raw == 0 ? AIS_TREND_STEADY : raw == 1 ? AIS_TREND_FALLING : raw == 2 ? AIS_TREND_RISING : AIS_TREND_UNKNOWN;
}

static void met_wind(ais_met_t *m, uint32_t speed, uint32_t gust, uint32_t dir, uint32_t gust_dir)
{
    if (speed < 127 && dir < 360) {
        m->wind_kn = (float)speed;
        m->wind_dir = (uint16_t)dir;
        m->fields |= AIS_MET_WIND;
    }
    if (gust < 127) {
        m->gust_kn = (float)gust;
        m->gust_dir = gust_dir < 360 ? (uint16_t)gust_dir : 360;
        m->fields |= AIS_MET_GUST;
    }
}

static void met_pressure(ais_met_t *m, uint32_t raw, uint32_t trend)
{
    if (raw <= 402) { // 0 is 799 hPa or less, 402 is 1201 or more
        m->pressure_hpa = (uint16_t)(raw + 799);
        m->pressure_trend = met_trend(trend);
        m->fields |= AIS_MET_PRESSURE;
    }
}

static void met_waves(ais_met_t *m, bool swell, uint32_t h, uint32_t period, uint32_t dir)
{
    if (h > 251) return;
    float *mh = swell ? &m->swell_m : &m->wave_m;
    uint8_t *ms = swell ? &m->swell_s : &m->wave_s;
    uint16_t *md = swell ? &m->swell_dir : &m->wave_dir;
    *mh = h / 10.0f;
    *ms = period <= 60 ? (uint8_t)period : 0;
    *md = dir < 360 ? (uint16_t)dir : 360;
    m->fields |= swell ? AIS_MET_SWELL : AIS_MET_WAVES;
}

static void met_water_temp(ais_met_t *m, float c)
{
    if (c >= -10.0f && c <= 50.0f) {
        m->water_c = c;
        m->fields |= AIS_MET_WATER_TEMP;
    }
}

static void met_salinity(ais_met_t *m, uint32_t raw)
{
    if (raw <= 501) {
        m->salinity = raw / 10.0f;
        m->fields |= AIS_MET_SALINITY;
    }
}

static void met_time(ais_met_t *m, uint32_t day, uint32_t hour, uint32_t minute)
{
    if (day >= 1 && day <= 31 && hour < 24 && minute < 60) {
        m->day = (uint8_t)day;
        m->hour = (uint8_t)hour;
        m->minute = (uint8_t)minute;
        m->fields |= AIS_MET_TIME;
    }
}

static bool met_pos(ais_met_t *m, int32_t lon_raw, int32_t lat_raw, double div)
{
    double lon = lon_raw / div, lat = lat_raw / div;
    if (lon < -180 || lon > 180 || lat < -90 || lat > 90 || (lon == 0 && lat == 0)) return false;
    m->lon = lon;
    m->lat = lat;
    m->fields |= AIS_MET_POS;
    return true;
}

// IMO289 meteorological and hydrological data: one station, 360 bits.
static void met_imo(const bitbuf_t *b, ais_report_t *out)
{
    if (b->nbits < 350) return;
    ais_met_t *m = &out->met[out->nmet++];
    memset(m, 0, sizeof(*m));
    m->site = AIS_MET_SITE_IMO;
    met_pos(m, get_s(b, 56, 25), get_s(b, 81, 24), 60000.0); // 1/1000 minute
    met_time(m, get_u(b, 106, 5), get_u(b, 111, 5), get_u(b, 116, 6));
    met_wind(m, get_u(b, 122, 7), get_u(b, 129, 7), get_u(b, 136, 9), get_u(b, 145, 9));
    int32_t t = get_s(b, 154, 11);
    if (t >= -600 && t <= 600) {
        m->air_c = t / 10.0f;
        m->fields |= AIS_MET_AIR_TEMP;
    }
    uint32_t rh = get_u(b, 165, 7);
    if (rh <= 100) {
        m->humidity = (uint8_t)rh;
        m->fields |= AIS_MET_HUMIDITY;
    }
    int32_t dew = get_s(b, 172, 10);
    if (dew >= -200 && dew <= 500) {
        m->dew_c = dew / 10.0f;
        m->fields |= AIS_MET_DEW;
    }
    met_pressure(m, get_u(b, 182, 9), get_u(b, 191, 2));
    uint32_t vis = get_u(b, 194, 7);
    if (vis < 127) {
        m->vis_nm = vis / 10.0f;
        m->fields |= AIS_MET_VIS;
    }
    uint32_t wl = get_u(b, 201, 12); // cm, from 10 m below the datum
    if (wl <= 4000) {
        m->water_m = (wl - 1000) / 100.0f;
        m->water_trend = met_trend(get_u(b, 213, 2));
        m->fields |= AIS_MET_WATER_LEVEL;
    }
    uint32_t cs = get_u(b, 215, 8), cd = get_u(b, 223, 9);
    if (cs <= 251) {
        m->current_kn = cs / 10.0f;
        m->current_dir = cd < 360 ? (uint16_t)cd : 360;
        m->fields |= AIS_MET_CURRENT;
    }
    met_waves(m, false, get_u(b, 276, 8), get_u(b, 284, 6), get_u(b, 290, 9));
    met_waves(m, true, get_u(b, 299, 8), get_u(b, 307, 6), get_u(b, 313, 9));
    uint32_t sea = get_u(b, 322, 4);
    if (sea <= 12) {
        m->sea_state = (uint8_t)sea;
        m->fields |= AIS_MET_SEA_STATE;
    }
    int32_t wt = get_s(b, 326, 10);
    if (wt != 501) met_water_temp(m, wt / 10.0f);
    met_salinity(m, get_u(b, 339, 9));
}

// The US environmental message: up to 8 sensor reports of 112 bits, each for
// a site (0-127) of the sending station. Its location and name come in
// reports of their own.
#define US_REPORT_BITS 112

static ais_met_t *met_site(ais_report_t *out, uint8_t site)
{
    for (int i = 0; i < out->nmet; i++) {
        if (out->met[i].site == site) return &out->met[i];
    }
    if (out->nmet == AIS_MET_SITES) return NULL;
    ais_met_t *m = &out->met[out->nmet++];
    memset(m, 0, sizeof(*m));
    m->site = site;
    return m;
}

static void met_us(const bitbuf_t *b, ais_report_t *out)
{
    int n = (b->nbits - 56) / US_REPORT_BITS;
    for (int i = 0; i < n; i++) {
        int o = 56 + i * US_REPORT_BITS, p = o + 27; // p: the report's own fields
        uint32_t type = get_u(b, o, 4);
        ais_met_t *m = met_site(out, (uint8_t)get_u(b, o + 20, 7));
        if (!m) break;
        if (type != 0 && type != 1) met_time(m, get_u(b, o + 4, 5), get_u(b, o + 9, 5), get_u(b, o + 14, 6));
        switch (type) {
        case 0: // site location
            met_pos(m, get_s(b, p + 6, 28), get_s(b, p + 34, 27), 600000.0);
            break;
        case 1: // station name
            get_str(b, p, 14, m->name, sizeof(m->name));
            if (m->name[0]) m->fields |= AIS_MET_NAME;
            break;
        case 2: // wind
            met_wind(m, get_u(b, p, 7), get_u(b, p + 7, 7), get_u(b, p + 14, 9), get_u(b, p + 23, 9));
            break;
        case 3: { // water level, cm
            int32_t cm = get_s(b, p + 1, 16);
            if (cm > -32768 && cm < 32767) {
                m->water_m = cm / 100.0f;
                m->water_trend = met_trend(get_u(b, p + 17, 2));
                m->fields |= AIS_MET_WATER_LEVEL;
            }
            break;
        }
        case 4: { // current, the first (shallowest) of three
            uint32_t cs = get_u(b, p, 8), cd = get_u(b, p + 8, 9);
            if (cs <= 251) {
                m->current_kn = cs / 10.0f;
                m->current_dir = cd < 360 ? (uint16_t)cd : 360;
                m->fields |= AIS_MET_CURRENT;
            }
            break;
        }
        case 7: { // sea state
            met_waves(m, true, get_u(b, p, 8), get_u(b, p + 8, 6), get_u(b, p + 14, 9));
            uint32_t sea = get_u(b, p + 23, 4);
            if (sea <= 12) {
                m->sea_state = (uint8_t)sea;
                m->fields |= AIS_MET_SEA_STATE;
            }
            met_water_temp(m, get_u(b, p + 30, 10) / 10.0f - 10.0f);
            met_waves(m, false, get_u(b, p + 50, 8), get_u(b, p + 58, 6), get_u(b, p + 64, 9));
            met_salinity(m, get_u(b, p + 76, 9));
            break;
        }
        case 8: // salinity
            met_water_temp(m, get_u(b, p, 10) / 10.0f - 10.0f);
            met_salinity(m, get_u(b, p + 36, 9));
            break;
        case 9: { // weather
            int32_t t = get_s(b, p, 11);
            if (t >= -600 && t <= 600) {
                m->air_c = t / 10.0f;
                m->fields |= AIS_MET_AIR_TEMP;
            }
            uint32_t vis = get_u(b, p + 16, 8);
            if (vis <= 250) {
                m->vis_nm = vis / 10.0f;
                m->fields |= AIS_MET_VIS;
            }
            uint32_t dew = get_u(b, p + 24, 10); // 0.1 degree from -20
            if (dew <= 700) {
                m->dew_c = dew / 10.0f - 20.0f;
                m->fields |= AIS_MET_DEW;
            }
            met_pressure(m, get_u(b, p + 37, 9), get_u(b, p + 46, 2));
            break;
        }
        default: // currents in 3D, horizontal flow, air gap, ...: not shown
            break;
        }
    }
}

ais_result_t ais_decode_payload(const char *payload, int fill_bits, ais_report_t *out)
{
    bitbuf_t b; // on the stack: the network feed and the SDR decode in different tasks
    memset(out, 0, sizeof(*out));
    if (!unarmour(payload, fill_bits, &b)) return AIS_RESULT_UNSUPPORTED;
    if (b.nbits >= 6) out->msg_type = (uint8_t)get_u(&b, 0, 6);
    if (b.nbits < 38) return AIS_RESULT_UNSUPPORTED;
    out->mmsi = get_u(&b, 8, 30);

    switch (out->msg_type) {
    case 1:
    case 2:
    case 3:
        if (b.nbits < 137) return AIS_RESULT_UNSUPPORTED;
        out->cls = AIS_CLASS_A;
        out->nav_status = (uint8_t)get_u(&b, 38, 4);
        out->fields |= AIS_F_NAVSTAT;
        set_sog(out, get_u(&b, 50, 10));
        set_pos(out, get_s(&b, 61, 28), get_s(&b, 89, 27), 600000.0);
        set_cog(out, get_u(&b, 116, 12));
        set_heading(out, get_u(&b, 128, 9));
        return AIS_RESULT_REPORT;

    case 5:
        if (b.nbits < 302) return AIS_RESULT_UNSUPPORTED;
        out->cls = AIS_CLASS_A;
        set_str_field(out, AIS_F_CALLSIGN, out->callsign, sizeof(out->callsign), &b, 70, 7);
        set_str_field(out, AIS_F_NAME, out->name, sizeof(out->name), &b, 112, 20);
        out->ship_type = (uint8_t)get_u(&b, 232, 8);
        if (out->ship_type) out->fields |= AIS_F_SHIPTYPE;
        set_dims(out, &b, 240);
        set_str_field(out, AIS_F_DEST, out->destination, sizeof(out->destination), &b, 302, 20);
        return AIS_RESULT_REPORT;

    case 18:
    case 19:
        if (b.nbits < 133) return AIS_RESULT_UNSUPPORTED;
        out->cls = AIS_CLASS_B;
        set_sog(out, get_u(&b, 46, 10));
        set_pos(out, get_s(&b, 57, 28), get_s(&b, 85, 27), 600000.0);
        set_cog(out, get_u(&b, 112, 12));
        set_heading(out, get_u(&b, 124, 9));
        if (out->msg_type == 19 && b.nbits >= 301) {
            set_str_field(out, AIS_F_NAME, out->name, sizeof(out->name), &b, 143, 20);
            out->ship_type = (uint8_t)get_u(&b, 263, 8);
            if (out->ship_type) out->fields |= AIS_F_SHIPTYPE;
            set_dims(out, &b, 271);
        }
        return AIS_RESULT_REPORT;

    case 24: {
        out->cls = AIS_CLASS_B;
        uint32_t part = get_u(&b, 38, 2);
        if (part == 0) {
            if (b.nbits < 160) return AIS_RESULT_UNSUPPORTED;
            set_str_field(out, AIS_F_NAME, out->name, sizeof(out->name), &b, 40, 20);
        } else if (part == 1) {
            if (b.nbits < 162) return AIS_RESULT_UNSUPPORTED;
            out->ship_type = (uint8_t)get_u(&b, 40, 8);
            if (out->ship_type) out->fields |= AIS_F_SHIPTYPE;
            set_str_field(out, AIS_F_CALLSIGN, out->callsign, sizeof(out->callsign), &b, 90, 7);
            set_dims(out, &b, 132);
        } else {
            return AIS_RESULT_UNSUPPORTED;
        }
        return AIS_RESULT_REPORT;
    }

    case 4:    // base station report: only its clock is used
    case 11: { // UTC/date response (from any station): the same layout, likewise
        if (b.nbits < 168) return AIS_RESULT_UNSUPPORTED;
        out->cls = AIS_CLASS_BASE;
        uint32_t y = get_u(&b, 38, 14), mo = get_u(&b, 52, 4), d = get_u(&b, 56, 5);
        uint32_t h = get_u(&b, 61, 5), mi = get_u(&b, 66, 6), s = get_u(&b, 72, 6);
        if (y >= 2000 && y <= 9999 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h <= 23 && mi <= 59 && s <= 59) {
            out->utc.year = (uint16_t)y;
            out->utc.month = (uint8_t)mo;
            out->utc.day = (uint8_t)d;
            out->utc.hour = (uint8_t)h;
            out->utc.minute = (uint8_t)mi;
            out->utc.second = (uint8_t)s;
            out->fields |= AIS_F_UTC;
        }
        set_pos(out, get_s(&b, 79, 28), get_s(&b, 107, 27), 600000.0);
        return AIS_RESULT_REPORT;
    }

    case 9: { // SAR aircraft: speed in whole knots, not tenths
        if (b.nbits < 128) return AIS_RESULT_UNSUPPORTED;
        out->cls = AIS_CLASS_SAR;
        uint32_t alt = get_u(&b, 38, 12), sog = get_u(&b, 50, 10);
        if (alt < 4095) {
            out->alt_m = (uint16_t)alt;
            out->fields |= AIS_F_ALT;
        }
        if (sog < 1023) {
            out->sog_kn = (float)sog;
            out->fields |= AIS_F_SOG;
        }
        set_pos(out, get_s(&b, 61, 28), get_s(&b, 89, 27), 600000.0);
        set_cog(out, get_u(&b, 116, 12));
        return AIS_RESULT_REPORT;
    }

    case 21: { // aid to navigation
        if (b.nbits < 272) return AIS_RESULT_UNSUPPORTED;
        out->cls = AIS_CLASS_ATON;
        out->aton_type = (uint8_t)get_u(&b, 38, 5);
        set_str_field(out, AIS_F_NAME, out->name, sizeof(out->name), &b, 43, 20); // (the name extension doesn't fit)
        set_pos(out, get_s(&b, 164, 28), get_s(&b, 192, 27), 600000.0);
        set_dims(out, &b, 219);
        // Off position only means something with a valid time stamp (second 0-59).
        if (get_u(&b, 253, 6) <= 59 && get_u(&b, 259, 1)) out->aton_flags |= AIS_ATON_OFF_POS;
        if (get_u(&b, 269, 1)) out->aton_flags |= AIS_ATON_VIRTUAL;
        out->fields |= AIS_F_ATON;
        return AIS_RESULT_REPORT;
    }

    case 8: { // binary broadcast: only weather and water stations are decoded
        if (b.nbits < 56) return AIS_RESULT_UNSUPPORTED;
        uint32_t dac = get_u(&b, 40, 10), fi = get_u(&b, 50, 6);
        if (dac == 1 && fi == 31) met_imo(&b, out);
        else if (dac == 367 && fi == 33) met_us(&b, out);
        for (int i = 0; i < out->nmet; i++) {
            if (out->met[i].fields) return AIS_RESULT_REPORT;
        }
        out->nmet = 0;
        return AIS_RESULT_UNSUPPORTED;
    }

    case 14: // safety broadcast: text after 2 spare bits, to the end of the message
        if (b.nbits < 46) return AIS_RESULT_UNSUPPORTED;
        set_str_field(out, AIS_F_TEXT, out->text, sizeof(out->text), &b, 40, (b.nbits - 40) / 6);
        return (out->fields & AIS_F_TEXT) ? AIS_RESULT_REPORT : AIS_RESULT_UNSUPPORTED;

    case 27:
        if (b.nbits < 95) return AIS_RESULT_UNSUPPORTED;
        out->cls = AIS_CLASS_A;
        out->nav_status = (uint8_t)get_u(&b, 40, 4);
        out->fields |= AIS_F_NAVSTAT;
        set_pos(out, get_s(&b, 44, 18), get_s(&b, 62, 17), 600.0);
        if (get_u(&b, 79, 6) < 63) {
            out->sog_kn = (float)get_u(&b, 79, 6);
            out->fields |= AIS_F_SOG;
        }
        if (get_u(&b, 85, 9) < 360) {
            out->cog_deg = (float)get_u(&b, 85, 9);
            out->fields |= AIS_F_COG;
        }
        return AIS_RESULT_REPORT;

    default:
        return AIS_RESULT_UNSUPPORTED;
    }
}

// ---------------------------------------------------------------------------
// Display strings
// ---------------------------------------------------------------------------

const char *ais_ship_type_str(uint8_t t)
{
    switch (t) {
    case 0: return "Not available";
    case 30: return "Fishing";
    case 31: case 32: return "Towing";
    case 33: return "Dredging";
    case 34: return "Diving ops";
    case 35: return "Military ops";
    case 36: return "Sailing";
    case 37: return "Pleasure craft";
    case 50: return "Pilot vessel";
    case 51: return "Search and rescue";
    case 52: return "Tug";
    case 53: return "Port tender";
    case 54: return "Anti-pollution";
    case 55: return "Law enforcement";
    case 58: return "Medical transport";
    case 59: return "Noncombatant";
    default: break;
    }
    if (t >= 20 && t <= 29) return "Wing in ground";
    if (t >= 40 && t <= 49) return "High speed craft";
    if (t >= 60 && t <= 69) return "Passenger";
    if (t >= 70 && t <= 79) return "Cargo";
    if (t >= 80 && t <= 89) return "Tanker";
    if (t >= 90 && t <= 99) return "Other";
    return "Reserved";
}

const char *ais_nav_status_str(uint8_t s)
{
    static const char *const names[] = {
        "Under way (engine)", "At anchor", "Not under command", "Restricted manoeuvrability",
        "Constrained by draught", "Moored", "Aground", "Fishing",
        "Under way (sailing)", "Reserved", "Reserved", "Towing astern",
        "Pushing / towing", "Reserved", "AIS-SART", "Not defined",
    };
    return s < 16 ? names[s] : "Not defined";
}

const char *ais_class_str(ais_class_t cls)
{
    switch (cls) {
    case AIS_CLASS_A: return "A";
    case AIS_CLASS_B: return "B";
    case AIS_CLASS_ATON: return "Aid to navigation";
    case AIS_CLASS_SAR: return "SAR aircraft";
    case AIS_CLASS_BASE: return "Base station";
    default: return "?";
    }
}

const char *ais_aton_type_str(uint8_t type)
{
    static const char *const names[32] = {
        "Aid to navigation", "Reference point", "RACON", "Fixed structure", "Aid to navigation",
        "Light", "Sector light", "Leading light (front)", "Leading light (rear)",
        "North cardinal beacon", "East cardinal beacon", "South cardinal beacon", "West cardinal beacon",
        "Port hand beacon", "Starboard hand beacon", "Preferred channel beacon (port hand)",
        "Preferred channel beacon (starboard hand)", "Isolated danger beacon", "Safe water beacon",
        "Special mark beacon",
        "North cardinal buoy", "East cardinal buoy", "South cardinal buoy", "West cardinal buoy",
        "Port hand buoy", "Starboard hand buoy", "Preferred channel buoy (port hand)",
        "Preferred channel buoy (starboard hand)", "Isolated danger buoy", "Safe water buoy", "Special mark buoy",
        "Light vessel / LANBY / rig",
    };
    return type < 32 ? names[type] : "Aid to navigation";
}

bool ais_msg_type_used(uint8_t type)
{
    switch (type) {
    case 1: case 2: case 3: case 4: case 5: case 8: case 9: case 11: case 14: case 18: case 19: case 21: case 24:
    case 27:
        return true; // (of type 8, only weather and water stations)
    default: return false;
    }
}

ais_safety_t ais_safety_kind(uint32_t mmsi, const char *text)
{
    uint32_t prefix = mmsi / 1000000; // 970 AIS-SART, 972 man overboard, 974 EPIRB
    bool beacon = prefix == 970 || prefix == 972 || prefix == 974;
    if (strstr(text, "MAYDAY")) return AIS_SAFETY_DISTRESS;
    if (beacon && strstr(text, "ACTIVE")) return AIS_SAFETY_DISTRESS;
    if (beacon && strstr(text, "TEST")) return AIS_SAFETY_TEST;
    return AIS_SAFETY_TEXT;
}

// ---------------------------------------------------------------------------
// 406 MHz beacon IDs (Cospas-Sarsat T.001): the 15 hex characters are bits
// 26-85 of the beacon's message
// ---------------------------------------------------------------------------

typedef struct { uint16_t lo, hi; const char *iso, *name; } mid_country_t;

static const mid_country_t *mid_find(uint16_t mid)
{
    // ISO 3166 codes for the flag images; territories with a flag of their own keep it.
    static const mid_country_t k[] = {
        {201, 201, "al", "Albania"}, {202, 202, "ad", "Andorra"}, {203, 203, "at", "Austria"}, {204, 204, "pt", "Azores"},
        {205, 205, "be", "Belgium"}, {206, 206, "by", "Belarus"}, {207, 207, "bg", "Bulgaria"}, {208, 208, "va", "Vatican"},
        {209, 210, "cy", "Cyprus"}, {211, 211, "de", "Germany"}, {212, 212, "cy", "Cyprus"}, {213, 213, "ge", "Georgia"},
        {214, 214, "md", "Moldova"}, {215, 215, "mt", "Malta"}, {216, 216, "am", "Armenia"}, {218, 218, "de", "Germany"},
        {219, 220, "dk", "Denmark"}, {224, 225, "es", "Spain"}, {226, 228, "fr", "France"}, {229, 229, "mt", "Malta"},
        {230, 230, "fi", "Finland"}, {231, 231, "fo", "Faroe Islands"}, {232, 235, "gb", "United Kingdom"}, {236, 236, "gi", "Gibraltar"},
        {237, 237, "gr", "Greece"}, {238, 238, "hr", "Croatia"}, {239, 241, "gr", "Greece"}, {242, 242, "ma", "Morocco"},
        {243, 243, "hu", "Hungary"}, {244, 246, "nl", "Netherlands"}, {247, 247, "it", "Italy"}, {248, 249, "mt", "Malta"},
        {250, 250, "ie", "Ireland"}, {251, 251, "is", "Iceland"}, {252, 252, "li", "Liechtenstein"}, {253, 253, "lu", "Luxembourg"},
        {254, 254, "mc", "Monaco"}, {255, 255, "pt", "Madeira"}, {256, 256, "mt", "Malta"}, {257, 259, "no", "Norway"},
        {261, 261, "pl", "Poland"}, {262, 262, "me", "Montenegro"}, {263, 263, "pt", "Portugal"}, {264, 264, "ro", "Romania"},
        {265, 266, "se", "Sweden"}, {267, 267, "sk", "Slovakia"}, {268, 268, "sm", "San Marino"}, {269, 269, "ch", "Switzerland"},
        {270, 270, "cz", "Czech Republic"}, {271, 271, "tr", "Turkey"}, {272, 272, "ua", "Ukraine"}, {273, 273, "ru", "Russia"},
        {274, 274, "mk", "North Macedonia"}, {275, 275, "lv", "Latvia"}, {276, 276, "ee", "Estonia"}, {277, 277, "lt", "Lithuania"},
        {278, 278, "si", "Slovenia"}, {279, 279, "rs", "Serbia"},
        {301, 301, "ai", "Anguilla"}, {303, 303, "us", "Alaska (USA)"}, {304, 305, "ag", "Antigua and Barbuda"},
        {306, 306, "bq", "Caribbean Netherlands"}, {307, 307, "aw", "Aruba"}, {308, 309, "bs", "Bahamas"}, {310, 310, "bm", "Bermuda"},
        {311, 311, "bs", "Bahamas"}, {312, 312, "bz", "Belize"}, {314, 314, "bb", "Barbados"}, {316, 316, "ca", "Canada"},
        {319, 319, "ky", "Cayman Islands"}, {321, 321, "cr", "Costa Rica"}, {323, 323, "cu", "Cuba"}, {325, 325, "dm", "Dominica"},
        {327, 327, "do", "Dominican Republic"}, {329, 329, "gp", "Guadeloupe"}, {330, 330, "gd", "Grenada"}, {331, 331, "gl", "Greenland"},
        {332, 332, "gt", "Guatemala"}, {334, 334, "hn", "Honduras"}, {336, 336, "ht", "Haiti"}, {338, 338, "us", "USA"},
        {339, 339, "jm", "Jamaica"}, {341, 341, "kn", "Saint Kitts and Nevis"}, {343, 343, "lc", "Saint Lucia"}, {345, 345, "mx", "Mexico"},
        {347, 347, "mq", "Martinique"}, {348, 348, "ms", "Montserrat"}, {350, 350, "ni", "Nicaragua"}, {351, 357, "pa", "Panama"},
        {358, 358, "pr", "Puerto Rico"}, {359, 359, "sv", "El Salvador"}, {361, 361, "pm", "Saint Pierre and Miquelon"},
        {362, 362, "tt", "Trinidad and Tobago"}, {364, 364, "tc", "Turks and Caicos"}, {366, 369, "us", "USA"}, {370, 374, "pa", "Panama"},
        {375, 377, "vc", "Saint Vincent and the Grenadines"}, {378, 378, "vg", "British Virgin Islands"},
        {379, 379, "vi", "US Virgin Islands"},
        {401, 401, "af", "Afghanistan"}, {403, 403, "sa", "Saudi Arabia"}, {405, 405, "bd", "Bangladesh"}, {408, 408, "bh", "Bahrain"},
        {410, 410, "bt", "Bhutan"}, {412, 414, "cn", "China"}, {416, 416, "tw", "Taiwan"}, {417, 417, "lk", "Sri Lanka"},
        {419, 419, "in", "India"}, {422, 422, "ir", "Iran"}, {423, 423, "az", "Azerbaijan"}, {425, 425, "iq", "Iraq"},
        {428, 428, "il", "Israel"}, {431, 432, "jp", "Japan"}, {434, 434, "tm", "Turkmenistan"}, {436, 436, "kz", "Kazakhstan"},
        {437, 437, "uz", "Uzbekistan"}, {438, 438, "jo", "Jordan"}, {440, 441, "kr", "South Korea"}, {443, 443, "ps", "Palestine"},
        {445, 445, "kp", "North Korea"}, {447, 447, "kw", "Kuwait"}, {450, 450, "lb", "Lebanon"}, {451, 451, "kg", "Kyrgyzstan"},
        {453, 453, "mo", "Macao"}, {455, 455, "mv", "Maldives"}, {457, 457, "mn", "Mongolia"}, {459, 459, "np", "Nepal"},
        {461, 461, "om", "Oman"}, {463, 463, "pk", "Pakistan"}, {466, 466, "qa", "Qatar"}, {468, 468, "sy", "Syria"},
        {470, 471, "ae", "United Arab Emirates"}, {472, 472, "tj", "Tajikistan"}, {473, 473, "ye", "Yemen"}, {475, 475, "ye", "Yemen"},
        {477, 477, "hk", "Hong Kong"}, {478, 478, "ba", "Bosnia and Herzegovina"},
        {501, 501, "tf", "Adelie Land"}, {503, 503, "au", "Australia"}, {506, 506, "mm", "Myanmar"}, {508, 508, "bn", "Brunei"},
        {510, 510, "fm", "Micronesia"}, {511, 511, "pw", "Palau"}, {512, 512, "nz", "New Zealand"}, {514, 515, "kh", "Cambodia"},
        {516, 516, "cx", "Christmas Island"}, {518, 518, "ck", "Cook Islands"}, {520, 520, "fj", "Fiji"}, {523, 523, "cc", "Cocos Islands"},
        {525, 525, "id", "Indonesia"}, {529, 529, "ki", "Kiribati"}, {531, 531, "la", "Laos"}, {533, 533, "my", "Malaysia"},
        {536, 536, "mp", "Northern Mariana Islands"}, {538, 538, "mh", "Marshall Islands"}, {540, 540, "nc", "New Caledonia"},
        {542, 542, "nu", "Niue"}, {544, 544, "nr", "Nauru"}, {546, 546, "pf", "French Polynesia"}, {548, 548, "ph", "Philippines"},
        {550, 550, "tl", "Timor-Leste"}, {553, 553, "pg", "Papua New Guinea"}, {555, 555, "pn", "Pitcairn"},
        {557, 557, "sb", "Solomon Islands"}, {559, 559, "as", "American Samoa"}, {561, 561, "ws", "Samoa"}, {563, 566, "sg", "Singapore"},
        {567, 567, "th", "Thailand"}, {570, 570, "to", "Tonga"}, {572, 572, "tv", "Tuvalu"}, {574, 574, "vn", "Vietnam"},
        {576, 577, "vu", "Vanuatu"}, {578, 578, "wf", "Wallis and Futuna"},
        {601, 601, "za", "South Africa"}, {603, 603, "ao", "Angola"}, {605, 605, "dz", "Algeria"},
        {607, 607, "tf", "Saint Paul and Amsterdam"}, {608, 608, "sh", "Ascension Island"}, {609, 609, "bi", "Burundi"},
        {610, 610, "bj", "Benin"}, {611, 611, "bw", "Botswana"}, {612, 612, "cf", "Central African Republic"},
        {613, 613, "cm", "Cameroon"}, {615, 615, "cg", "Congo"}, {616, 616, "km", "Comoros"}, {617, 617, "cv", "Cape Verde"},
        {618, 618, "tf", "Crozet Archipelago"}, {619, 619, "ci", "Cote d'Ivoire"}, {620, 620, "km", "Comoros"},
        {621, 621, "dj", "Djibouti"}, {622, 622, "eg", "Egypt"}, {624, 624, "et", "Ethiopia"}, {625, 625, "er", "Eritrea"},
        {626, 626, "ga", "Gabon"}, {627, 627, "gh", "Ghana"}, {629, 629, "gm", "Gambia"}, {630, 630, "gw", "Guinea-Bissau"},
        {631, 631, "gq", "Equatorial Guinea"}, {632, 632, "gn", "Guinea"}, {633, 633, "bf", "Burkina Faso"}, {634, 634, "ke", "Kenya"},
        {635, 635, "tf", "Kerguelen Islands"}, {636, 637, "lr", "Liberia"}, {638, 638, "ss", "South Sudan"}, {642, 642, "ly", "Libya"},
        {644, 644, "ls", "Lesotho"}, {645, 645, "mu", "Mauritius"}, {647, 647, "mg", "Madagascar"}, {649, 649, "ml", "Mali"},
        {650, 650, "mz", "Mozambique"}, {654, 654, "mr", "Mauritania"}, {655, 655, "mw", "Malawi"}, {656, 656, "ne", "Niger"},
        {657, 657, "ng", "Nigeria"}, {659, 659, "na", "Namibia"}, {660, 660, "re", "Reunion"}, {661, 661, "rw", "Rwanda"},
        {662, 662, "sd", "Sudan"}, {663, 663, "sn", "Senegal"}, {664, 664, "sc", "Seychelles"}, {665, 665, "sh", "Saint Helena"},
        {666, 666, "so", "Somalia"}, {667, 667, "sl", "Sierra Leone"}, {668, 668, "st", "Sao Tome and Principe"},
        {669, 669, "sz", "Eswatini"}, {670, 670, "td", "Chad"}, {671, 671, "tg", "Togo"}, {672, 672, "tn", "Tunisia"},
        {674, 674, "tz", "Tanzania"}, {675, 675, "ug", "Uganda"}, {676, 676, "cd", "DR Congo"}, {677, 677, "tz", "Tanzania"},
        {678, 678, "zm", "Zambia"}, {679, 679, "zw", "Zimbabwe"},
        {701, 701, "ar", "Argentina"}, {710, 710, "br", "Brazil"}, {720, 720, "bo", "Bolivia"}, {725, 725, "cl", "Chile"},
        {730, 730, "co", "Colombia"}, {735, 735, "ec", "Ecuador"}, {740, 740, "fk", "Falkland Islands"}, {745, 745, "gf", "French Guiana"},
        {750, 750, "gy", "Guyana"}, {755, 755, "py", "Paraguay"}, {760, 760, "pe", "Peru"}, {765, 765, "sr", "Suriname"},
        {770, 770, "uy", "Uruguay"}, {775, 775, "ve", "Venezuela"},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (mid >= k[i].lo && mid <= k[i].hi) return &k[i];
    }
    return NULL;
}

const char *ais_mid_country(uint16_t mid)
{
    const mid_country_t *c = mid_find(mid);
    return c ? c->name : NULL;
}

const char *ais_mid_iso(uint16_t mid)
{
    const mid_country_t *c = mid_find(mid);
    return c ? c->iso : NULL;
}

uint16_t ais_mmsi_mid(uint32_t mmsi)
{
    uint32_t mid;
    if (mmsi >= 200000000 && mmsi < 800000000) mid = mmsi / 1000000;              // MIDxxxxxx: ship
    else if (mmsi >= 800000000 && mmsi < 900000000) mid = mmsi / 100000 % 1000;   // 8MIDxxxxx: handheld
    else if (mmsi >= 980000000 && mmsi < 1000000000) mid = mmsi / 10000 % 1000;   // 98MID/99MID: craft, AtoN
    else if (mmsi >= 111000000 && mmsi < 112000000) mid = mmsi / 1000 % 1000;     // 111MIDxxx: SAR aircraft
    else if (mmsi >= 10000000 && mmsi < 100000000) mid = mmsi / 100000;           // 0MIDxxxxx: group
    else if (mmsi >= 1000000 && mmsi < 10000000) mid = mmsi / 10000;              // 00MIDxxxx: coast station
    else return 0; // 970/972/974 beacons and the like carry no country
    return mid_find((uint16_t)mid) ? (uint16_t)mid : 0;
}

// Bits first..last (numbered as in T.001, 26..85) of a beacon ID.
static uint32_t id_bits(const uint8_t *nib, int first, int last)
{
    uint32_t v = 0;
    for (int b = first; b <= last; b++) {
        int i = b - 26;
        v = v << 1 | ((nib[i / 4] >> (3 - i % 4)) & 1);
    }
    return v;
}

// A 6-bit modified-Baudot character (ITA2 with a letters/figures bit on top).
static char baudot6(uint32_t c)
{
    static const char letters[33] = "\0T\rO HNM\nLRGIPCVEZDBSYFXAWJ\0UQK\0";
    static const char figures[32] = {
        [1] = '5', [3] = '9', [4] = ' ', [10] = '4', [12] = '8', [13] = '0', [16] = '3',
        [21] = '6', [23] = '/', [24] = '-', [25] = '2', [28] = '7', [29] = '1',
    };
    char ch = (c & 0x20) ? letters[c & 0x1f] : figures[c & 0x1f];
    return ch >= ' ' ? ch : '?';
}

bool ais_beacon_id_parse(const char *text, ais_beacon_id_t *out)
{
    for (const char *p = text; *p; p++) {
        if (!isxdigit((unsigned char)*p) || (p > text && isxdigit((unsigned char)p[-1]))) continue;
        int n = 0;
        while (isxdigit((unsigned char)p[n])) n++;
        if (n != 15) {
            p += n - 1;
            continue;
        }
        uint8_t nib[15];
        for (int i = 0; i < 15; i++) nib[i] = (uint8_t)hexval(p[i]);
        ais_beacon_id_t id = {.beacon_no = -1, .t_prefix = p > text && (p[-1] == 'T' || p[-1] == 't')};
        for (int i = 0; i < 15; i++) id.hex[i] = (char)toupper((unsigned char)p[i]);
        id.country = (uint16_t)id_bits(nib, 27, 36);
        id.country_name = ais_mid_country(id.country);
        if (!id.country_name) {
            p += n - 1;
            continue;
        }
        if (id_bits(nib, 26, 26)) { // user protocols
            switch (id_bits(nib, 37, 39)) {
            case 0: id.what = "orbitography (a satellite-system beacon)"; break;
            case 1: {
                id.what = "ELT (aircraft), coded with its registration";
                char reg[8];
                for (int i = 0; i < 7; i++) reg[i] = baudot6(id_bits(nib, 40 + 6 * i, 45 + 6 * i));
                reg[7] = 0;
                snprintf(id.ident, sizeof(id.ident), "aircraft %s", reg);
                id.beacon_no = (int)id_bits(nib, 82, 83);
                break;
            }
            case 2: {
                id.what = "maritime beacon, coded with its ship's MMSI or call sign";
                char s[7];
                bool digits = true;
                for (int i = 0; i < 6; i++) {
                    s[i] = baudot6(id_bits(nib, 40 + 6 * i, 45 + 6 * i));
                    digits = digits && s[i] >= '0' && s[i] <= '9';
                }
                s[6] = 0;
                if (digits) id.mmsi = id.country * 1000000u + (uint32_t)atoi(s);
                else snprintf(id.ident, sizeof(id.ident), "call sign %s", s);
                char b = baudot6(id_bits(nib, 76, 81));
                if (b >= '0' && b <= '9') id.beacon_no = b - '0';
                break;
            }
            case 3: {
                static const char *const types[8] = {
                    "ELT (aircraft)", "ELT (aircraft, 24-bit address)", "EPIRB (float-free)",
                    "ELT (aircraft operator)", "EPIRB (not float-free)", NULL, "PLB (personal)", NULL,
                };
                uint32_t t = id_bits(nib, 40, 42);
                id.what = types[t] ? types[t] : "beacon, coded with a serial number";
                if (t == 1) snprintf(id.ident, sizeof(id.ident), "aircraft address %06lX",
                                     (unsigned long)id_bits(nib, 44, 67));
                else snprintf(id.ident, sizeof(id.ident), "serial %lu", (unsigned long)id_bits(nib, 44, 63));
                break;
            }
            case 4: id.what = "beacon with a national (country-assigned) ID"; break;
            case 6: {
                id.what = "beacon coded with its ship's call sign";
                char cs[8];
                for (int i = 0; i < 4; i++) cs[i] = baudot6(id_bits(nib, 40 + 6 * i, 45 + 6 * i));
                for (int i = 0; i < 3; i++) {
                    uint32_t d = id_bits(nib, 64 + 4 * i, 67 + 4 * i);
                    cs[4 + i] = d < 10 ? (char)('0' + d) : ' ';
                }
                cs[7] = 0;
                snprintf(id.ident, sizeof(id.ident), "call sign %s", cs);
                char b = baudot6(id_bits(nib, 76, 81));
                if (b >= '0' && b <= '9') id.beacon_no = b - '0';
                break;
            }
            case 7: id.what = "test beacon"; break;
            default: id.what = "beacon (unknown user protocol)"; break;
            }
        } else { // location protocols
            uint32_t pc = id_bits(nib, 37, 40);
            switch (pc) {
            case 2: case 12:
                id.what = pc == 2 ? "EPIRB, coded with its ship's MMSI" : "ship security alert, coded with the ship's MMSI";
                id.mmsi = id.country * 1000000u + id_bits(nib, 41, 60);
                if (pc == 2) id.beacon_no = (int)id_bits(nib, 61, 64);
                break;
            case 3:
                id.what = "ELT (aircraft), coded with its 24-bit address";
                snprintf(id.ident, sizeof(id.ident), "aircraft address %06lX", (unsigned long)id_bits(nib, 41, 64));
                break;
            case 4: case 6: case 7:
                id.what = pc == 4 ? "ELT (aircraft), coded with a serial number"
                        : pc == 6 ? "EPIRB, coded with a serial number" : "PLB (personal), coded with a serial number";
                snprintf(id.ident, sizeof(id.ident), "serial %lu (type approval %lu)",
                         (unsigned long)id_bits(nib, 51, 64), (unsigned long)id_bits(nib, 41, 50));
                break;
            case 5: id.what = "ELT (aircraft), coded with its operator"; break;
            case 8: id.what = "ELT (aircraft) with a national ID"; break;
            case 9: id.what = "ELT (aircraft, distress tracking)"; break;
            case 10: id.what = "EPIRB with a national ID"; break;
            case 11: id.what = "PLB (personal) with a national ID"; break;
            case 13: id.what = "beacon with return-link service"; break;
            case 14: case 15: id.what = "test beacon"; break;
            default: id.what = "beacon (unknown location protocol)"; break;
            }
        }
        *out = id;
        return true;
    }
    return false;
}

void ais_beacon_id_describe(const ais_beacon_id_t *id, const char *ship_name, char *buf, size_t len)
{
    size_t n = 0;
#define ADD(...) do { if (n < len) n += (size_t)snprintf(buf + n, len - n, __VA_ARGS__); } while (0)
    if (len) buf[0] = 0;
    ADD("406 MHz beacon ID %s\n", id->hex);
    ADD("%c%s\n", toupper((unsigned char)id->what[0]), id->what + 1);
    if (id->mmsi) {
        ADD("Ship MMSI %lu", (unsigned long)id->mmsi);
        if (ship_name && ship_name[0]) ADD(" (%s)", ship_name);
        ADD(", %s\n", id->country_name);
    } else {
        ADD("%s%s%s\n", id->ident[0] ? id->ident : "", id->ident[0] ? ", " : "", id->country_name);
    }
    if (id->beacon_no >= 0) ADD("Beacon no. %d\n", id->beacon_no);
    if (id->t_prefix) ADD("The \"T\" before the ID may mean a test\n");
#undef ADD
}

const char *ais_msg_type_str(uint8_t type)
{
    static const char *const names[28] = {
        "Too short to tell", "Position report (Class A)", "Position report (Class A, assigned)",
        "Position report (Class A, interrogated)", "Base station report", "Static and voyage data (Class A)",
        "Binary addressed message", "Binary acknowledge", "Binary broadcast message", "SAR aircraft position",
        "UTC/date inquiry", "UTC/date response", "Addressed safety message", "Safety acknowledge",
        "Safety broadcast message", "Interrogation", "Assignment mode command", "DGNSS broadcast",
        "Position report (Class B)", "Extended position report (Class B)", "Data link management",
        "Aid-to-navigation report", "Channel management", "Group assignment command",
        "Static data report (Class B)", "Single slot binary message", "Multiple slot binary message",
        "Long-range position report",
    };
    return type < 28 ? names[type] : "Unknown";
}
