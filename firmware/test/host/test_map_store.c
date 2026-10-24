// Feeds zip files to the streaming map uploader in awkward chunk sizes.
// Usage: test_map_store <ok|fail> file.zip [<ok|fail> file.zip ...]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_pack.h"
#include "map_store.h"
#include "settings.h"

static settings_t s_cfg = {.zoom_index = 7};
static int s_map_changed;
void settings_get(settings_t *out) { *out = s_cfg; }
int settings_set(const settings_t *in) { s_cfg = *in; return 0; }
void settings_notify_map_changed(void) { s_map_changed++; }

static int run(const char *expect, const char *path, size_t chunk)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    static uint8_t buf[1 << 22];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    int changed_before = s_map_changed;
    // A card that had the chart copied on from a Mac also holds "._name" files.
    FILE *junk = fopen(MAP_DIR "/._z0.pak", "w");
    if (junk) fclose(junk);

    char berr[160];
    map_upload_t *u = map_upload_begin(berr, sizeof(berr));
    if (!u) {
        printf("FAIL %s: %s\n", path, berr);
        return 1;
    }
    for (size_t off = 0; off < n; off += chunk) {
        size_t len = n - off < chunk ? n - off : chunk;
        if (map_upload_feed(u, buf + off, len) != ESP_OK) break;
    }
    char err[96] = "";
    bool ok = map_upload_finish(u, err, sizeof(err)) == ESP_OK;
    bool want = strcmp(expect, "ok") == 0;
    if (ok != want) {
        printf("FAIL %s (chunk %zu): expected %s, got %s %s\n", path, chunk, expect, ok ? "ok" : "fail", err);
        return 1;
    }
    if (ok) {
        map_manifest_t m;
        if (!map_store_get_manifest(&m) || m.nlevels != 3 || m.levels[0].radius_nm > m.levels[1].radius_nm ||
            s_cfg.center_lat != m.center_lat || s_cfg.map_created != 1234567 || s_map_changed != changed_before + 1 ||
            s_cfg.zoom_index != 2 || m.levels[0].cols != 3 || m.levels[0].rows != 2) {
            printf("FAIL %s: installed map not as expected\n", path);
            return 1;
        }
        // Read tiles back out of an installed pack.
        map_pack_t pk;
        char perr[96], ppath[128];
        snprintf(ppath, sizeof(ppath), "%s/%s", MAP_DIR, m.levels[0].file);
        size_t len0, len1, bad;
        if (!map_pack_open(&pk, ppath, perr, sizeof(perr))) {
            printf("FAIL %s: %s\n", path, perr);
            return 1;
        }
        uint8_t *t0 = map_pack_read(&pk, 0, 0, &len0), *t1 = map_pack_read(&pk, 2, 1, &len1);
        uint8_t *t2 = map_pack_read(&pk, 3, 0, &bad);
        bool good = !t0 && len0 == 0 && t1 && len1 > 24 && memcmp(t1, "\x89PNG", 4) == 0 && !t2 && bad == 1;
        free(t0);
        free(t1);
        free(t2);
        map_pack_close(&pk);
        if (!good) {
            printf("FAIL %s: tiles not read back correctly\n", path);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int failures = 0;
    map_store_mount();
    map_store_init();
    static const size_t chunks[] = {1, 7, 30, 4096, 1 << 22};
    for (int i = 1; i + 1 < argc; i += 2) {
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            failures += run(argv[i], argv[i + 1], chunks[c]);
        }
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all map upload tests passed\n");
    return 0;
}
