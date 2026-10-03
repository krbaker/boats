// Feeds zip files to the streaming map uploader in awkward chunk sizes.
// Usage: test_map_store <ok|fail> file.zip [<ok|fail> file.zip ...]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    map_upload_t *u = map_upload_begin();
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
            s_cfg.center_lat != m.center_lat || s_map_changed != changed_before + 1 || s_cfg.zoom_index != 2) {
            printf("FAIL %s: installed map not as expected\n", path);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int failures = 0;
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
