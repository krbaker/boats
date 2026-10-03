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
    // Type 4 base station report: valid but unsupported.
    CHECK(ais_parse_line(&p, "!AIVDM,1,1,,A,403OviQuMGCqWrRO9>E6fE700@GO,0*4D", 0, &r) == AIS_RESULT_UNSUPPORTED);
    CHECK(p.stats.bad_checksum == 2);
    CHECK(p.stats.unsupported == 1);
}

static void test_strings(void)
{
    CHECK(strcmp(ais_ship_type_str(70), "Cargo") == 0);
    CHECK(strcmp(ais_ship_type_str(36), "Sailing") == 0);
    CHECK(strcmp(ais_nav_status_str(5), "Moored") == 0);
}

int main(void)
{
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
