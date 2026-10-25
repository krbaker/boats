// Host-side unit tests for the AIS parser. Build and run with `make` in this directory.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ais.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(fabs((double)(a) - (double)(b)) < (eps))

static void test_type1(void)
{
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    CHECK(ais_parse_line(&p, "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A\r\n", 0, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 1);
    CHECK(r.mmsi == 371798000);
    CHECK(r.cls == AIS_CLASS_A);
    CHECK(r.fields & AIS_F_POS);
    CHECK_NEAR(r.lat, 48.38163, 1e-4);
    CHECK_NEAR(r.lon, -123.39538, 1e-4);
    CHECK_NEAR(r.sog_kn, 12.3, 1e-3);
    CHECK_NEAR(r.cog_deg, 224.0, 1e-3);
    CHECK(r.heading == 215);
}

static void test_type5_multipart(void)
{
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    CHECK(ais_parse_line(&p, "!AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C", 100, &r) ==
          AIS_RESULT_PENDING);
    CHECK(ais_parse_line(&p, "!AIVDM,2,2,1,A,88888888880,2*25", 200, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 5);
    CHECK(r.mmsi == 351759000);
    CHECK(strcmp(r.name, "EVER DIADEM") == 0);
    CHECK(strcmp(r.callsign, "3FOF8") == 0);
    CHECK(strcmp(r.destination, "NEW YORK") == 0);
    CHECK(r.ship_type == 70);
    CHECK(r.dim_a == 225 && r.dim_b == 70 && r.dim_c == 1 && r.dim_d == 31);
}

static void test_fragment_timeout(void)
{
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    ais_parse_line(&p, "!AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C", 0, &r);
    // Second half arrives after the timeout: stale first half must be dropped.
    CHECK(ais_parse_line(&p, "!AIVDM,2,2,1,A,88888888880,2*25", AIS_FRAG_TIMEOUT_MS + 500, &r) == AIS_RESULT_PENDING);
}

static void test_timestamp_prefixes(void)
{
    static const char *lines[] = {
        "\\s:rORBCOMM000,c:1696300000*4F\\!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A",
        "2023-10-03T01:02:03.456Z !AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A",
        "1696300000.123 !AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A",
        "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A,rMySource,1696300000",
        "$PGHP,1,2023,10,3,1,2,3,0,,,,,*00 !BSVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        ais_parser_t p;
        ais_report_t r;
        ais_parser_init(&p);
        ais_result_t res = ais_parse_line(&p, lines[i], 0, &r);
        if (strstr(lines[i], "!BSVDM")) {
            // Different talker means a different checksum; verify it is
            // recognised as AIS (rejected on checksum, not ignored).
            CHECK(res == AIS_RESULT_BAD_CHECKSUM);
        } else {
            CHECK(res == AIS_RESULT_REPORT);
            CHECK(r.mmsi == 371798000);
        }
    }
}

static void test_rejects(void)
{
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    CHECK(ais_parse_line(&p, "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4B", 0, &r) == AIS_RESULT_BAD_CHECKSUM);
    CHECK(ais_parse_line(&p, "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0", 0, &r) == AIS_RESULT_BAD_CHECKSUM);
    CHECK(ais_parse_line(&p, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", 0, &r) ==
          AIS_RESULT_IGNORED);
    CHECK(ais_parse_line(&p, "", 0, &r) == AIS_RESULT_IGNORED);
    CHECK(ais_parse_line(&p, "garbage !!! ,,,***", 0, &r) == AIS_RESULT_IGNORED);
    // Type 8 binary broadcast: valid but unsupported, and still counted by type.
    CHECK(ais_parse_line(&p, "!AIVDM,1,1,,A,8000000000000000000,0*1E", 0, &r) == AIS_RESULT_UNSUPPORTED);
    CHECK(p.stats.bad_checksum == 2);
    CHECK(p.stats.unsupported == 1);
    CHECK(p.stats.by_type[8] == 1);
}

static void test_strings(void)
{
    CHECK(strcmp(ais_ship_type_str(70), "Cargo") == 0);
    CHECK(strcmp(ais_ship_type_str(36), "Sailing") == 0);
    CHECK(strcmp(ais_nav_status_str(5), "Moored") == 0);
}

// Build payloads bit by bit, for the message types there's no handy real sentence for.
typedef struct {
    uint8_t bit[1024];
    int n;
} bits_t;

static void put(bits_t *b, uint32_t v, int len)
{
    for (int i = len - 1; i >= 0; i--) b->bit[b->n++] = (v >> i) & 1;
}

static void put_str(bits_t *b, const char *s, int nchars)
{
    for (int i = 0; i < nchars; i++) {
        char c = *s ? *s++ : '@';
        put(b, c >= 64 ? c - 64 : c, 6);
    }
}

// Degrees to the 1/10000-minute units of a position field.
static uint32_t deg(double d) { return (uint32_t)(int32_t)lround(d * 600000.0); }

// The armoured payload, padded to whole characters; *fill = bits added.
static void armour(const bits_t *b, char *out, int *fill)
{
    int n = (b->n + 5) / 6;
    *fill = n * 6 - b->n;
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 6; k++) v = v << 1 | (i * 6 + k < b->n ? b->bit[i * 6 + k] : 0);
        out[i] = (char)(v < 40 ? v + 48 : v + 56);
    }
    out[n] = '\0';
}

static void test_type9_sar(void)
{
    bits_t b = {0};
    put(&b, 9, 6); put(&b, 0, 2); put(&b, 111232511, 30);
    put(&b, 303, 12);          // altitude, m
    put(&b, 42, 10);           // speed, whole knots
    put(&b, 0, 1);
    put(&b, deg(-6.27884), 28);
    put(&b, deg(58.144), 27);
    put(&b, 1541, 12);         // course, tenths
    put(&b, 0, 20); put(&b, 0, 20); // to the full 168 bits
    char pl[64];
    int fill;
    armour(&b, pl, &fill);
    ais_report_t r;
    CHECK(ais_decode_payload(pl, fill, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 9 && r.mmsi == 111232511 && r.cls == AIS_CLASS_SAR);
    CHECK((r.fields & AIS_F_ALT) && r.alt_m == 303);
    CHECK((r.fields & AIS_F_SOG) && r.sog_kn == 42);
    CHECK_NEAR(r.cog_deg, 154.1, 1e-3);
    CHECK_NEAR(r.lat, 58.144, 1e-5);
    CHECK_NEAR(r.lon, -6.27884, 1e-5);
}

static void test_type21_aton(void)
{
    bits_t b = {0};
    put(&b, 21, 6); put(&b, 0, 2); put(&b, 993672072, 30);
    put(&b, 25, 5);                         // starboard hand buoy
    put_str(&b, "SEATTLE LT 3", 20);
    put(&b, 1, 1);
    put(&b, deg(-122.3550), 28);
    put(&b, deg(47.6005), 27);
    put(&b, 2, 9); put(&b, 2, 9); put(&b, 1, 6); put(&b, 1, 6);
    put(&b, 7, 4);                          // EPFD
    put(&b, 30, 6);                         // time stamp: a valid second
    put(&b, 1, 1);                          // off position
    put(&b, 0, 8); put(&b, 0, 1);
    put(&b, 1, 1);                          // virtual
    put(&b, 0, 2);
    char pl[80];
    int fill;
    armour(&b, pl, &fill);
    ais_report_t r;
    CHECK(ais_decode_payload(pl, fill, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 21 && r.mmsi == 993672072 && r.cls == AIS_CLASS_ATON);
    CHECK((r.fields & AIS_F_ATON) && r.aton_type == 25);
    CHECK(strcmp(r.name, "SEATTLE LT 3") == 0);
    CHECK(r.aton_flags == (AIS_ATON_VIRTUAL | AIS_ATON_OFF_POS));
    CHECK_NEAR(r.lat, 47.6005, 1e-5);
    CHECK_NEAR(r.lon, -122.3550, 1e-5);
    CHECK(strcmp(ais_aton_type_str(25), "Starboard hand buoy") == 0);

    // Off position means nothing without a valid time stamp.
    b.n = 253;
    put(&b, 61, 6); put(&b, 1, 1); put(&b, 0, 8); put(&b, 0, 1); put(&b, 0, 1); put(&b, 0, 2);
    armour(&b, pl, &fill);
    CHECK(ais_decode_payload(pl, fill, &r) == AIS_RESULT_REPORT);
    CHECK(r.aton_flags == 0);
}

static void test_type4_base(void)
{
    ais_report_t r;
    // The gpsd documentation's sample base station report.
    CHECK(ais_decode_payload("403OviQuMGCqWrRO9>E6fE700@GO", 0, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 4 && r.mmsi == 3669702 && r.cls == AIS_CLASS_BASE);
    CHECK(r.fields & AIS_F_UTC);
    CHECK(r.utc.year == 2007 && r.utc.month == 5 && r.utc.day == 14);
    CHECK(r.utc.hour == 19 && r.utc.minute == 57 && r.utc.second == 39);

    // Type 11, a UTC/date response, has the same layout: the same payload with ';' (11) for '4'.
    CHECK(ais_decode_payload(";03OviQuMGCqWrRO9>E6fE700@GO", 0, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 11 && r.mmsi == 3669702 && r.cls == AIS_CLASS_BASE);
    CHECK((r.fields & AIS_F_UTC) && r.utc.year == 2007 && r.utc.second == 39);
    CHECK(ais_msg_type_used(11));
}

static void test_type14_safety(void)
{
    bits_t b = {0};
    put(&b, 14, 6); put(&b, 0, 2); put(&b, 970123456, 30); put(&b, 0, 2);
    put_str(&b, "SART ACTIVE", 11);
    char pl[80];
    int fill;
    armour(&b, pl, &fill);
    ais_report_t r;
    CHECK(ais_decode_payload(pl, fill, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 14 && r.mmsi == 970123456);
    CHECK((r.fields & AIS_F_TEXT) && strcmp(r.text, "SART ACTIVE") == 0);
    CHECK(ais_safety_kind(r.mmsi, r.text) == AIS_SAFETY_DISTRESS);
    CHECK(ais_safety_kind(972000001, "MOB TEST") == AIS_SAFETY_TEST);
    CHECK(ais_safety_kind(366123456, "SART ACTIVE") == AIS_SAFETY_TEXT); // not a beacon's MMSI
    CHECK(ais_safety_kind(366123456, "MAYDAY MAYDAY") == AIS_SAFETY_DISTRESS);
    CHECK(ais_msg_type_used(14));
}

static void put_s(bits_t *b, int32_t v, int len) { put(b, (uint32_t)v & ((1u << len) - 1), len); }

static void test_type8_met_imo(void)
{
    bits_t b = {0};
    put(&b, 8, 6); put(&b, 0, 2); put(&b, 3669999, 30); put(&b, 0, 2); put(&b, 1, 10); put(&b, 31, 6);
    put_s(&b, (int32_t)lround(-122.4 * 60000), 25);
    put_s(&b, (int32_t)lround(47.6 * 60000), 24);
    put(&b, 1, 1);
    put(&b, 25, 5); put(&b, 14, 5); put(&b, 30, 6);   // 25th, 14:30 UTC
    put(&b, 14, 7); put(&b, 22, 7);                   // wind 14 kn, gust 22
    put(&b, 315, 9); put(&b, 320, 9);
    put_s(&b, -25, 11);                               // -2.5 C
    put(&b, 80, 7);                                   // 80 %
    put_s(&b, 501, 10);                               // dew point: not available
    put(&b, 1013 - 799, 9); put(&b, 2, 2);            // 1013 hPa, rising
    put(&b, 0, 1); put(&b, 52, 7);                    // 5.2 nm
    put(&b, 1000 + 231, 12); put(&b, 1, 2);           // 2.31 m, falling
    put(&b, 12, 8); put(&b, 45, 9);                   // current 1.2 kn to 045
    put(&b, 255, 8); put(&b, 360, 9); put(&b, 31, 5); // current 2: none
    put(&b, 255, 8); put(&b, 360, 9); put(&b, 31, 5); // current 3: none
    put(&b, 12, 8); put(&b, 8, 6); put(&b, 225, 9);   // waves 1.2 m, 8 s, from 225
    put(&b, 255, 8); put(&b, 63, 6); put(&b, 360, 9); // swell: none
    put(&b, 4, 4);                                    // Beaufort 4
    put_s(&b, 112, 10);                               // water 11.2 C
    put(&b, 7, 3); put(&b, 305, 9); put(&b, 3, 2); put(&b, 0, 10);
    char pl[80];
    int fill;
    armour(&b, pl, &fill);
    ais_report_t r;
    CHECK(ais_decode_payload(pl, fill, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 8 && r.mmsi == 3669999 && r.nmet == 1);
    const ais_met_t *m = &r.met[0];
    CHECK(m->site == AIS_MET_SITE_IMO);
    CHECK((m->fields & AIS_MET_POS) && fabs(m->lat - 47.6) < 1e-4 && fabs(m->lon + 122.4) < 1e-4);
    CHECK((m->fields & AIS_MET_TIME) && m->day == 25 && m->hour == 14 && m->minute == 30);
    CHECK((m->fields & AIS_MET_WIND) && m->wind_kn == 14 && m->wind_dir == 315);
    CHECK((m->fields & AIS_MET_GUST) && m->gust_kn == 22);
    CHECK((m->fields & AIS_MET_AIR_TEMP) && fabs(m->air_c + 2.5) < 1e-4);
    CHECK((m->fields & AIS_MET_HUMIDITY) && m->humidity == 80);
    CHECK(!(m->fields & AIS_MET_DEW));
    CHECK((m->fields & AIS_MET_PRESSURE) && m->pressure_hpa == 1013 && m->pressure_trend == AIS_TREND_RISING);
    CHECK((m->fields & AIS_MET_VIS) && fabs(m->vis_nm - 5.2) < 1e-4);
    CHECK((m->fields & AIS_MET_WATER_LEVEL) && fabs(m->water_m - 2.31) < 1e-4 && m->water_trend == AIS_TREND_FALLING);
    CHECK((m->fields & AIS_MET_CURRENT) && fabs(m->current_kn - 1.2) < 1e-4 && m->current_dir == 45);
    CHECK((m->fields & AIS_MET_WAVES) && fabs(m->wave_m - 1.2) < 1e-4 && m->wave_s == 8 && m->wave_dir == 225);
    CHECK(!(m->fields & AIS_MET_SWELL));
    CHECK((m->fields & AIS_MET_SEA_STATE) && m->sea_state == 4);
    CHECK((m->fields & AIS_MET_WATER_TEMP) && fabs(m->water_c - 11.2) < 1e-4);
    CHECK((m->fields & AIS_MET_SALINITY) && fabs(m->salinity - 30.5) < 1e-4);
}

// One US sensor report: the 27-bit header, then 85 bits of its own.
static void us_report(bits_t *b, int type, int site)
{
    put(b, type, 4); put(b, 25, 5); put(b, 14, 5); put(b, 30, 6); put(b, site, 7);
}

static void pad_to(bits_t *b, int n) { while (b->n < n) b->bit[b->n++] = 0; }

static void test_type8_met_us(void)
{
    bits_t b = {0};
    put(&b, 8, 6); put(&b, 0, 2); put(&b, 3669998, 30); put(&b, 0, 2); put(&b, 367, 10); put(&b, 33, 6);
    int start = b.n;
    us_report(&b, 0, 5);                              // site 5's location
    put(&b, 0, 6); put_s(&b, (int32_t)lround(-122.3 * 600000), 28); put_s(&b, (int32_t)lround(47.5 * 600000), 27);
    pad_to(&b, start + 112);
    us_report(&b, 1, 5);                              // its name
    put_str(&b, "ALKI POINT", 14);
    pad_to(&b, start + 224);
    us_report(&b, 2, 5);                              // wind 9 kn from 200, gust 15
    put(&b, 9, 7); put(&b, 15, 7); put(&b, 200, 9); put(&b, 360, 9);
    pad_to(&b, start + 336);
    us_report(&b, 3, 5);                              // water level 1.85 m, rising
    put(&b, 0, 1); put_s(&b, 185, 16); put(&b, 2, 2);
    pad_to(&b, start + 448);
    us_report(&b, 2, 9);                              // another site's wind
    put(&b, 20, 7); put(&b, 127, 7); put(&b, 90, 9); put(&b, 360, 9);
    pad_to(&b, start + 560);
    char pl[120];
    int fill;
    armour(&b, pl, &fill);
    ais_report_t r;
    CHECK(ais_decode_payload(pl, fill, &r) == AIS_RESULT_REPORT);
    CHECK(r.msg_type == 8 && r.nmet == 2);
    const ais_met_t *m = &r.met[0];
    CHECK(m->site == 5);
    CHECK((m->fields & AIS_MET_POS) && fabs(m->lat - 47.5) < 1e-5 && fabs(m->lon + 122.3) < 1e-5);
    CHECK((m->fields & AIS_MET_NAME) && strcmp(m->name, "ALKI POINT") == 0);
    CHECK((m->fields & AIS_MET_WIND) && m->wind_kn == 9 && m->wind_dir == 200);
    CHECK((m->fields & AIS_MET_GUST) && m->gust_kn == 15 && m->gust_dir == 360);
    CHECK((m->fields & AIS_MET_WATER_LEVEL) && fabs(m->water_m - 1.85) < 1e-4 && m->water_trend == AIS_TREND_RISING);
    CHECK(r.met[1].site == 9 && (r.met[1].fields & AIS_MET_WIND) && r.met[1].wind_kn == 20);
    CHECK(!(r.met[1].fields & AIS_MET_GUST));
}

static void test_type_counts(void)
{
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    ais_parse_line(&p, "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A", 0, &r);
    ais_parse_line(&p, "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A", 0, &r);
    CHECK(p.stats.by_type[1] == 2);
    CHECK(ais_msg_type_used(21) && ais_msg_type_used(4) && !ais_msg_type_used(10));
    CHECK(strcmp(ais_msg_type_str(21), "Aid-to-navigation report") == 0);
}

int main(void)
{
    test_type9_sar();
    test_type21_aton();
    test_type4_base();
    test_type14_safety();
    test_type8_met_imo();
    test_type8_met_us();
    test_type_counts();
    test_type1();
    test_type5_multipart();
    test_fragment_timeout();
    test_timestamp_prefixes();
    test_rejects();
    test_strings();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all AIS tests passed\n");
    return 0;
}
