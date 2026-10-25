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
    uint8_t bit[512];
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

static void test_type_counts(void)
{
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    ais_parse_line(&p, "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A", 0, &r);
    ais_parse_line(&p, "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A", 0, &r);
    CHECK(p.stats.by_type[1] == 2);
    CHECK(ais_msg_type_used(21) && ais_msg_type_used(4) && !ais_msg_type_used(8));
    CHECK(strcmp(ais_msg_type_str(21), "Aid-to-navigation report") == 0);
}

int main(void)
{
    test_type9_sar();
    test_type21_aton();
    test_type4_base();
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
