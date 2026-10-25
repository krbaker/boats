// Host tests for the aircraft.json reader (components/adsb_parse), on files in
// the formats of readsb, dump1090-fa and dump1090-mutability saved in
// fixtures/adsb/. Run by run_tests.sh.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adsb_parse.h"

static int failures;

#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

static char *load(const char *name, size_t *len)
{
    char path[256];
    snprintf(path, sizeof(path), "fixtures/adsb/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL: can't open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n); // not NUL-terminated: the reader mustn't need it
    *len = fread(buf, 1, n, f);
    fclose(f);
    return buf;
}

typedef struct {
    adsb_aircraft_t a[16];
    int n, stop_after;
} got_t;

static bool collect(void *ctx, const adsb_aircraft_t *a)
{
    got_t *g = ctx;
    g->a[g->n++] = *a;
    return g->n != g->stop_after;
}

static int parse_file(const char *name, got_t *g, double *now)
{
    size_t len;
    char *j = load(name, &len);
    memset(g, 0, sizeof(*g));
    int n = adsb_parse(j, len, collect, g, now);
    free(j);
    return n;
}

static void test_readsb(void)
{
    got_t g;
    double now;
    CHECK(parse_file("readsb.json", &g, &now) == 6);
    CHECK(NEAR(now, 1761431234.5, 1e-6));

    const adsb_aircraft_t *a = &g.a[0]; // an airliner on approach, with nested arrays to skip
    CHECK(a->addr == 0xa1b2c3 && !a->non_icao);
    CHECK(strcmp(a->flight, "ASA123") == 0 && strcmp(a->type, "B739") == 0 && strcmp(a->reg, "N123AS") == 0);
    CHECK(a->category == 0xA3 && adsb_kind(a) == ADSB_KIND_LARGE);
    CHECK((a->fields & ADSB_F_POS) && NEAR(a->lat, 47.612345, 1e-9) && NEAR(a->lon, -122.401234, 1e-9));
    CHECK((a->fields & ADSB_F_ALT) && a->alt_ft == 4825); // barometric, not the geometric 5000
    CHECK((a->fields & ADSB_F_GS) && NEAR(a->gs_kn, 231.4, 1e-3));
    CHECK((a->fields & ADSB_F_TRACK) && NEAR(a->track_deg, 172.31, 1e-3));
    CHECK((a->fields & ADSB_F_VRATE) && a->vrate_fpm == -1216);
    CHECK((a->fields & ADSB_F_SQUAWK) && a->squawk == 4433);
    CHECK(!adsb_is_emergency(a) && adsb_emergency_str(a) == NULL);
    CHECK(NEAR(a->seen_pos_s, 0.4, 1e-6) && NEAR(a->seen_s, 0.1, 1e-6));

    a = &g.a[1]; // a helicopter on the ground
    CHECK(adsb_kind(a) == ADSB_KIND_ROTOR);
    CHECK((a->fields & ADSB_F_GROUND) && !(a->fields & ADSB_F_ALT));
    CHECK(strcmp(a->flight, "N60HX") == 0);

    a = &g.a[2]; // TIS-B: no ICAO address, no category
    CHECK(a->non_icao && a->addr == 0x2c4f01);
    CHECK(adsb_kind(a) == ADSB_KIND_UNKNOWN && a->category == 0);
    CHECK(a->flight[0] == '\0' && !(a->fields & ADSB_F_SQUAWK));

    a = &g.a[3]; // squawking 7700, heavy; \u escape in the call sign; baro rate over geometric
    CHECK(adsb_is_emergency(a) && strcmp(adsb_emergency_str(a), "Emergency") == 0);
    CHECK(adsb_kind(a) == ADSB_KIND_HEAVY);
    CHECK(strcmp(a->flight, "UAL9?") == 0);
    CHECK(a->vrate_fpm == 640);

    a = &g.a[4]; // no position
    CHECK(!(a->fields & ADSB_F_POS) && a->alt_ft == 38000);

    a = &g.a[5]; // geometric altitude only; a balloon with radio failure
    CHECK((a->fields & ADSB_F_ALT) && a->alt_ft == 2100);
    CHECK(adsb_kind(a) == ADSB_KIND_BALLOON);
    CHECK(adsb_is_emergency(a) && strcmp(adsb_emergency_str(a), "Radio failure") == 0);
}

static void test_dump1090_fa(void)
{
    got_t g;
    double now;
    CHECK(parse_file("dump1090_fa.json", &g, &now) == 1);
    const adsb_aircraft_t *a = &g.a[0];
    CHECK(a->addr == 0xab1234 && adsb_kind(a) == ADSB_KIND_LIGHT);
    CHECK(a->alt_ft == 2500 && a->vrate_fpm == -500 && NEAR(a->seen_pos_s, 5.5, 1e-6));
}

static void test_mutability(void)
{
    got_t g;
    double now;
    CHECK(parse_file("mutability.json", &g, &now) == 2);
    CHECK(NEAR(now, 1500000000.0, 1e-6));
    const adsb_aircraft_t *a = &g.a[0]; // the old field names
    CHECK(a->alt_ft == 3200 && a->vrate_fpm == 1600 && NEAR(a->gs_kn, 180, 1e-6));
    CHECK(adsb_is_emergency(a) && strcmp(adsb_emergency_str(a), "Hijack") == 0);
    CHECK(g.a[1].fields & ADSB_F_GROUND);
}

static void test_edges(void)
{
    got_t g;
    double now;
    size_t len;
    char *j = load("readsb.json", &len);

    // Cut short anywhere: never more than were whole, and no crash (ASan).
    int whole = 0;
    for (size_t cut = 0; cut <= len; cut++) {
        memset(&g, 0, sizeof(g));
        int n = adsb_parse(j, cut, collect, &g, &now);
        CHECK(n <= 6);
        if (n > whole) whole = n;
    }
    CHECK(whole == 6);
    // Cut inside the third aircraft: the first two come through.
    const char *third = strstr(j, "~2c4f01");
    memset(&g, 0, sizeof(g));
    CHECK(adsb_parse(j, (size_t)(third - j), collect, &g, &now) == 2);

    // The callback can stop it.
    memset(&g, 0, sizeof(g));
    g.stop_after = 3;
    CHECK(adsb_parse(j, len, collect, &g, &now) == 3);
    free(j);

    // Not an aircraft.json.
    static const char *const bad[] = {"", "[]", "<html>404</html>", "{\"now\":1}", "{\"aircraft\":{}}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(adsb_parse(bad[i], strlen(bad[i]), collect, &g, &now) == -1);
    }
    memset(&g, 0, sizeof(g));
    CHECK(adsb_parse("{\"aircraft\":[]}", 15, collect, &g, &now) == 0);
    // Wrong types are skipped, not misread.
    const char *odd = "{\"aircraft\":[{\"hex\":12,\"lat\":\"x\",\"lon\":null,\"squawk\":7700,\"alt_baro\":true,\"category\":\"Z9\"}]}";
    memset(&g, 0, sizeof(g));
    CHECK(adsb_parse(odd, strlen(odd), collect, &g, &now) == 1);
    CHECK(g.a[0].fields == 0 && g.a[0].category == 0 && g.a[0].addr == 0);
}

static void test_strings(void)
{
    CHECK(strcmp(adsb_category_str(0xA7), "Helicopter") == 0);
    CHECK(strcmp(adsb_category_str(0), "") == 0);
    adsb_aircraft_t a = {.category = 0xC2};
    CHECK(adsb_kind(&a) == ADSB_KIND_SURFACE);
    a.category = 0xB6;
    CHECK(adsb_kind(&a) == ADSB_KIND_UAV);
    a = (adsb_aircraft_t){.emergency = ADSB_EMERG_LIFEGUARD};
    CHECK(!adsb_is_emergency(&a) && strcmp(adsb_emergency_str(&a), "Medical priority") == 0);
}

int main(void)
{
    test_readsb();
    test_dump1090_fa();
    test_mutability();
    test_edges();
    test_strings();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("adsb parse: all tests passed\n");
    return 0;
}
