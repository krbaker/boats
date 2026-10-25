#include "ais.h"

#include <ctype.h>
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
    case 1: case 2: case 3: case 4: case 5: case 9: case 11: case 18: case 19: case 21: case 24: case 27: return true;
    default: return false;
    }
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
