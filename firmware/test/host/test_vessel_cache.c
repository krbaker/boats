// Host-side unit tests for the vessel static-data cache. Run by run_tests.sh;
// the first argument is a scratch directory.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vessel_cache.h"

static int failures;
static const char *dir;
static char log_path[256], tmp_path[256];

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

#define DAY (24 * 60 * 60)
#define T0  1790000000u // a wall clock time in 2026

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void fresh(void)
{
    remove(log_path);
    remove(tmp_path);
}

static vc_vessel_t named(uint32_t mmsi, const char *name)
{
    vc_vessel_t v = {.mmsi = mmsi, .cls = 1, .fields = VC_F_NAME};
    snprintf(v.name, sizeof(v.name), "%s", name);
    return v;
}

static vc_vessel_t full(uint32_t mmsi)
{
    vc_vessel_t v = {.mmsi = mmsi, .cls = 1, .ship_type = 70, .fields = VC_F_ALL,
                     .dim_a = 225, .dim_b = 70, .dim_c = 1, .dim_d = 31};
    snprintf(v.name, sizeof(v.name), "EVER DIADEM");
    snprintf(v.callsign, sizeof(v.callsign), "3FOF8");
    return v;
}

static void test_mmsi_valid(void)
{
    CHECK(vc_mmsi_valid(351759000));
    CHECK(vc_mmsi_valid(111232511)); // SAR aircraft
    CHECK(!vc_mmsi_valid(0));
    CHECK(!vc_mmsi_valid(2320001));   // base station (00 prefix)
    CHECK(!vc_mmsi_valid(1000000000));
    CHECK(!vc_mmsi_valid(123456789));
    CHECK(!vc_mmsi_valid(111111111));
    CHECK(!vc_mmsi_valid(999999999));
}

static void test_merge(void)
{
    vessel_cache_t *vc = vc_open(NULL, 100);
    vc_vessel_t v, out;

    // Position-only reports don't create an entry, and invalid MMSIs are ignored.
    v = (vc_vessel_t){.mmsi = 351759000, .cls = 1};
    CHECK(!vc_update(vc, &v, T0));
    CHECK(!vc_get(vc, 351759000, &out));
    v = named(123456789, "DEFAULT");
    CHECK(!vc_update(vc, &v, T0));
    CHECK(!vc_get(vc, 123456789, &out));

    // Class B: 24A (name), then 24B (type, call sign, size), merged.
    v = named(338123456, "SEA DOG");
    v.cls = 2;
    CHECK(vc_update(vc, &v, T0));
    v = (vc_vessel_t){.mmsi = 338123456, .cls = 2, .ship_type = 37, .fields = VC_F_SHIPTYPE | VC_F_CALLSIGN | VC_F_DIM,
                      .dim_a = 8, .dim_b = 4, .dim_c = 2, .dim_d = 2};
    snprintf(v.callsign, sizeof(v.callsign), "WDA1234");
    CHECK(vc_update(vc, &v, T0 + 10));
    CHECK(vc_get(vc, 338123456, &out));
    CHECK(strcmp(out.name, "SEA DOG") == 0);
    CHECK(strcmp(out.callsign, "WDA1234") == 0);
    CHECK(out.ship_type == 37 && out.dim_a == 8 && out.dim_d == 2 && out.cls == 2);
    CHECK(out.fields == VC_F_ALL);
    CHECK(out.first_seen == T0 && out.last_seen == T0 + 10 && out.last_changed == T0 + 10);

    // The same data again changes nothing; a position report moves last_seen
    // only; nothing absent clears a known value.
    CHECK(!vc_update(vc, &v, T0 + 20));
    v = (vc_vessel_t){.mmsi = 338123456, .cls = 2};
    CHECK(!vc_update(vc, &v, T0 + 30));
    CHECK(vc_get(vc, 338123456, &out));
    CHECK(out.last_seen == T0 + 30 && out.last_changed == T0 + 10);
    CHECK(strcmp(out.name, "SEA DOG") == 0 && out.fields == VC_F_ALL);

    // A new name replaces the old one. An unset clock leaves the times alone.
    v = named(338123456, "SEA CAT");
    v.cls = 2;
    CHECK(vc_update(vc, &v, 0));
    CHECK(vc_get(vc, 338123456, &out));
    CHECK(strcmp(out.name, "SEA CAT") == 0 && out.last_seen == T0 + 30 && out.last_changed == 0);
    vc_close(vc);
}

static void test_reload_and_write_limit(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 100);
    vc_stats_t st;
    vc_vessel_t v = full(351759000), out;
    CHECK(vc_update(vc, &v, T0));
    CHECK(vc_flush(vc, 100, false) == 1);
    CHECK(vc_flush(vc, 101, false) == 0); // nothing changed
    CHECK(file_size(log_path) == VC_RECORD_SIZE);

    // Renamed: written once, but not until 10 minutes after the last write.
    v = named(351759000, "EVER DAZZLE");
    CHECK(vc_update(vc, &v, T0 + 60));
    CHECK(vc_flush(vc, 160, false) == 0);
    v = named(351759000, "EVER DIADEM"); // a faulty transponder flapping
    CHECK(vc_update(vc, &v, T0 + 70));
    v = named(351759000, "EVER DAZZLE");
    CHECK(vc_update(vc, &v, T0 + 80));
    CHECK(vc_flush(vc, 100 + VC_WRITE_INTERVAL_S - 1, false) == 0);
    CHECK(vc_flush(vc, 100 + VC_WRITE_INTERVAL_S, false) == 1);
    CHECK(vc_flush(vc, 100 + 2 * VC_WRITE_INTERVAL_S, false) == 0);

    // Force ignores the limit (before a restart).
    v = named(351759000, "EVER DIADEM");
    CHECK(vc_update(vc, &v, T0 + 90));
    CHECK(vc_flush(vc, 100 + VC_WRITE_INTERVAL_S + 1, true) == 1);
    vc_get_stats(vc, &st);
    CHECK(st.count == 1 && st.file_records == 3 && st.records_written == 3 && !st.ram_only && !st.last_error[0]);
    vc_close(vc);

    vc = vc_open(dir, 100);
    CHECK(vc_get(vc, 351759000, &out));
    CHECK(strcmp(out.name, "EVER DIADEM") == 0 && strcmp(out.callsign, "3FOF8") == 0);
    CHECK(out.ship_type == 70 && out.dim_a == 225 && out.dim_b == 70 && out.dim_c == 1 && out.dim_d == 31);
    CHECK(out.cls == 1 && out.fields == VC_F_ALL);
    CHECK(out.first_seen == T0 && out.last_seen == T0 + 90 && out.last_changed == T0 + 90);
    vc_get_stats(vc, &st);
    CHECK(st.count == 1 && st.file_records == 3);
    vc_close(vc);
}

static void test_last_seen(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 100);
    vc_vessel_t v = full(351759000), pos = {.mmsi = 351759000, .cls = 1}, out;
    vc_update(vc, &v, T0);
    CHECK(vc_flush(vc, 0, false) == 1);
    // A vessel at anchor for most of a day: no writes.
    vc_update(vc, &pos, T0 + DAY - 1);
    CHECK(vc_flush(vc, 10000, false) == 0);
    // A day after the stored "last seen": written.
    vc_update(vc, &pos, T0 + DAY);
    CHECK(vc_flush(vc, 20000, false) == 1);
    vc_close(vc);
    vc = vc_open(dir, 100);
    CHECK(vc_get(vc, 351759000, &out) && out.last_seen == T0 + DAY);
    vc_close(vc);
}

static void test_torn_and_corrupt(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 100);
    for (uint32_t i = 0; i < 5; i++) {
        vc_vessel_t v = full(351759000 + i);
        vc_update(vc, &v, T0);
    }
    CHECK(vc_flush(vc, 0, false) == 5);
    vc_close(vc);

    // Corrupt record 2, then a power cut part way through an append.
    FILE *f = fopen(log_path, "r+b");
    fseek(f, 2 * VC_RECORD_SIZE + 20, SEEK_SET);
    fputc('X', f);
    fseek(f, 0, SEEK_END);
    fwrite("VC\1\0garbage", 1, 11, f);
    fclose(f);

    vc = vc_open(dir, 100);
    vc_vessel_t out;
    CHECK(vc_get(vc, 351759000, &out) && strcmp(out.name, "EVER DIADEM") == 0);
    CHECK(!vc_get(vc, 351759002, &out));
    CHECK(vc_get(vc, 351759004, &out));
    // The next flush rewrites the log before appending anything.
    vc_vessel_t v = named(351759001, "NEW NAME");
    vc_update(vc, &v, T0 + 1);
    CHECK(vc_flush(vc, 0, false) == 4);
    CHECK(file_size(log_path) == 4 * VC_RECORD_SIZE);
    CHECK(vc_flush(vc, 0, false) == 0);
    vc_close(vc);
    vc = vc_open(dir, 100);
    CHECK(vc_get(vc, 351759001, &out) && strcmp(out.name, "NEW NAME") == 0);
    vc_close(vc);
}

// A power cut part way through an append, alone: the next append must not
// land after the torn bytes, where it couldn't be read back.
static void test_torn_tail_only(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 100);
    vc_vessel_t v = full(351759000), out;
    vc_update(vc, &v, T0);
    vc_flush(vc, 0, false);
    vc_close(vc);
    FILE *f = fopen(log_path, "ab");
    fwrite("VC\1\0", 1, 4, f);
    fclose(f);

    vc = vc_open(dir, 100);
    v = full(351759001);
    vc_update(vc, &v, T0);
    vc_flush(vc, 0, false);
    vc_close(vc);
    CHECK(file_size(log_path) % VC_RECORD_SIZE == 0);
    vc = vc_open(dir, 100);
    CHECK(vc_get(vc, 351759000, &out) && vc_get(vc, 351759001, &out));
    vc_close(vc);
}

static void test_compaction_crash(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 100);
    vc_vessel_t v = full(351759000), out;
    vc_update(vc, &v, T0);
    vc_flush(vc, 0, false);
    vc_close(vc);

    // Crash after deleting the old log, before the rename.
    CHECK(rename(log_path, tmp_path) == 0);
    vc = vc_open(dir, 100);
    CHECK(vc_get(vc, 351759000, &out));
    CHECK(file_size(log_path) == VC_RECORD_SIZE && file_size(tmp_path) < 0);
    vc_close(vc);

    // Crash while writing cache.tmp: the old log stands.
    FILE *f = fopen(tmp_path, "wb");
    fwrite("half", 1, 4, f);
    fclose(f);
    vc = vc_open(dir, 100);
    CHECK(vc_get(vc, 351759000, &out));
    CHECK(file_size(tmp_path) < 0);
    vc_close(vc);
}

static void test_bloat_compacts(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 100);
    vc_stats_t st;
    for (int i = 0; i < 400; i++) {
        vc_vessel_t v = named(351759000 + i % 3, i % 2 ? "ONE" : "TWO");
        vc_update(vc, &v, T0 + i);
        CHECK(vc_flush(vc, i, true) >= 1);
    }
    vc_get_stats(vc, &st);
    CHECK(st.count == 3 && st.compactions >= 1 && st.file_records <= 2 * 3 + 256);
    CHECK(file_size(log_path) == (long)st.file_records * VC_RECORD_SIZE);
    vc_close(vc);
}

static void test_eviction(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 4);
    vc_vessel_t out;
    for (uint32_t i = 0; i < 4; i++) {
        vc_vessel_t v = full(351759000 + i);
        vc_update(vc, &v, T0 + i);
    }
    CHECK(vc_flush(vc, 0, false) == 4);
    vc_vessel_t pos = {.mmsi = 351759000};
    vc_update(vc, &pos, T0 + 10); // the first one heard again: 351759001 is now the oldest
    vc_vessel_t v = full(351759009);
    vc_update(vc, &v, T0 + 11);
    CHECK(vc_get(vc, 351759000, &out));
    CHECK(!vc_get(vc, 351759001, &out));
    CHECK(vc_get(vc, 351759009, &out));
    CHECK(vc_flush(vc, 0, false) == 1);
    vc_close(vc);

    // The log holds 5 vessels; loading into 4 drops the one whose stored
    // "last seen" is oldest (351759000: its newer one wasn't due a write yet).
    vc = vc_open(dir, 4);
    vc_stats_t st;
    vc_get_stats(vc, &st);
    CHECK(st.count == 4);
    CHECK(!vc_get(vc, 351759000, &out) && vc_get(vc, 351759001, &out) && vc_get(vc, 351759009, &out));
    vc_close(vc);
}

// Many inserts and evictions against a simple model, to exercise the index.
static void test_index_stress(void)
{
    enum { CAP = 64, N = 20000 };
    vessel_cache_t *vc = vc_open(NULL, CAP);
    uint32_t model[CAP], seen[CAP];
    int count = 0;
    srand(1);
    for (int i = 0; i < N; i++) {
        uint32_t mmsi = 200000000 + (uint32_t)(rand() % 300) * 7919;
        vc_vessel_t v = named(mmsi, "X");
        vc_update(vc, &v, T0 + i);
        int k;
        for (k = 0; k < count && model[k] != mmsi; k++) {}
        if (k == count) {
            if (count < CAP) {
                count++;
            } else {
                k = 0;
                for (int j = 1; j < count; j++) if (seen[j] < seen[k]) k = j;
            }
            model[k] = mmsi;
        }
        seen[k] = T0 + i;
    }
    vc_stats_t st;
    vc_get_stats(vc, &st);
    CHECK(st.count == CAP);
    int found = 0;
    for (int k = 0; k < count; k++) {
        vc_vessel_t out;
        if (vc_get(vc, model[k], &out) && out.last_seen == seen[k]) found++;
    }
    CHECK(found == CAP);
    vc_close(vc);
}

static void test_clear(void)
{
    fresh();
    vessel_cache_t *vc = vc_open(dir, 100);
    vc_vessel_t v = full(351759000), out;
    vc_update(vc, &v, T0);
    vc_flush(vc, 0, false);
    vc_clear(vc);
    CHECK(!vc_get(vc, 351759000, &out));
    CHECK(file_size(log_path) < 0);
    vc_close(vc);
    vc = vc_open(dir, 100);
    CHECK(!vc_get(vc, 351759000, &out));
    vc_close(vc);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s scratch-dir\n", argv[0]);
        return 2;
    }
    dir = argv[1];
    snprintf(log_path, sizeof(log_path), "%s/%s", dir, VC_LOG_NAME);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", dir, VC_TMP_NAME);

    test_mmsi_valid();
    test_merge();
    test_reload_and_write_limit();
    test_last_seen();
    test_torn_and_corrupt();
    test_torn_tail_only();
    test_compaction_crash();
    test_bloat_compacts();
    test_eviction();
    test_index_stress();
    test_clear();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all vessel cache tests passed\n");
    return 0;
}
