#include "map_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "map_pack.h"
#include "settings.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if CONFIG_FATFS_LFN_NONE
#error "The chart files (manifest.json) need FAT long file names. Set CONFIG_FATFS_LFN_HEAP=y in firmware/sdkconfig (idf.py menuconfig > Component config > FAT Filesystem support > Long filename support), or delete sdkconfig to regenerate it from sdkconfig.defaults."
#endif
#endif

static const char *TAG = "map_store";

#define NEW_DIR         MAP_ROOT "/ais_map.new"
#define MAX_FILES       (MAP_MAX_LEVELS + 2)
#define MAX_FILE_SIZE   (1024u * 1024 * 1024)
#define WRITE_BUF       (32 * 1024)

static bool s_card_ok;
static void (*s_on_replace)(void);

void map_store_on_replace(void (*fn)(void))
{
    s_on_replace = fn;
}

bool map_store_card_ok(void)
{
    return s_card_ok;
}

// A chart set copied onto the card by hand hasn't set the configured centre
// yet; do it the first time we see that set.
static void adopt_new_chart(void)
{
    map_manifest_t m;
    if (!map_store_get_manifest(&m)) return;
    settings_t cfg;
    settings_get(&cfg);
    if (cfg.map_created == m.created) return;
    ESP_LOGI(TAG, "new chart set on card: centre %.5f,%.5f", m.center_lat, m.center_lon);
    cfg.map_created = m.created;
    cfg.center_lat = m.center_lat;
    cfg.center_lon = m.center_lon;
    cfg.map_created = m.created;
    if (cfg.zoom_index >= m.nlevels) cfg.zoom_index = m.nlevels - 1;
    settings_set(&cfg);
}

esp_err_t map_store_mount(void)
{
    // The card stays mounted for good: it shares the SDMMC host with the
    // Wi-Fi co-processor, and unmounting would shut that host down.
    esp_err_t err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no microSD card (%s)", esp_err_to_name(err));
        return err;
    }
    s_card_ok = true;
    return ESP_OK;
}

esp_err_t map_store_init(void)
{
    if (!s_card_ok) return ESP_FAIL; // map_store_mount() found no card
    uint64_t total, free_bytes;
    map_store_usage(&total, &free_bytes);
    ESP_LOGI(TAG, "microSD: %u MB free of %u MB", (unsigned)(free_bytes >> 20), (unsigned)(total >> 20));
    adopt_new_chart();
    return ESP_OK;
}

void map_store_usage(uint64_t *total, uint64_t *free_bytes)
{
    *total = *free_bytes = 0;
    if (s_card_ok) esp_vfs_fat_info(MAP_ROOT, total, free_bytes);
}

static void remove_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char p[300];
    while ((e = readdir(d))) {
        // Everything but . and .., including the "._name" files macOS leaves on FAT cards.
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        if (unlink(p) != 0) ESP_LOGW(TAG, "could not delete %s: %s", p, strerror(errno));
    }
    closedir(d);
    rmdir(path);
}

static int level_cmp(const void *a, const void *b)
{
    float ra = ((const map_level_t *)a)->radius_nm, rb = ((const map_level_t *)b)->radius_nm;
    return (ra > rb) - (ra < rb);
}

static double num(cJSON *o, const char *key)
{
    return cJSON_GetNumberValue(cJSON_GetObjectItem(o, key)); // NaN if missing
}

static bool parse_manifest(const char *dir, map_manifest_t *m, char *err, size_t errlen)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/manifest.json", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "manifest.json missing");
        return false;
    }
    char *buf = malloc(8192);
    size_t n = fread(buf, 1, 8191, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        snprintf(err, errlen, "manifest.json is not valid JSON");
        return false;
    }

    bool ok = false;
    memset(m, 0, sizeof(*m));
    cJSON *proj = cJSON_GetObjectItem(root, "projection");
    if (num(root, "version") != 3 || !cJSON_IsString(proj) || strcmp(proj->valuestring, "EPSG:3857") != 0) {
        snprintf(err, errlen, "chart is from an older map builder; rebuild it");
        goto out;
    }
    m->center_lat = num(root, "center_lat");
    m->center_lon = num(root, "center_lon");
    m->pan_nm = num(root, "pan_nm");
    m->created = (uint32_t)num(root, "created");
    if (!(fabs(m->center_lat) <= 90) || !(fabs(m->center_lon) <= 180)) {
        snprintf(err, errlen, "manifest: bad centre position");
        goto out;
    }
    if (!(m->pan_nm > 0) || num(root, "tile_px") != MAP_TILE_PX) {
        snprintf(err, errlen, "manifest: bad pan area or tile size");
        goto out;
    }

    cJSON *lv;
    cJSON_ArrayForEach(lv, cJSON_GetObjectItem(root, "levels")) {
        if (m->nlevels >= MAP_MAX_LEVELS) break;
        map_level_t *l = &m->levels[m->nlevels];
        cJSON *file = cJSON_GetObjectItem(lv, "file"), *origin = cJSON_GetObjectItem(lv, "origin");
        if (!cJSON_IsString(file) || strchr(file->valuestring, '/') || strlen(file->valuestring) >= sizeof(l->file) ||
            cJSON_GetArraySize(origin) != 2) {
            snprintf(err, errlen, "manifest: bad level entry");
            goto out;
        }
        strlcpy(l->file, file->valuestring, sizeof(l->file));
        l->radius_nm = num(lv, "radius_nm");
        l->m_per_px = num(lv, "m_per_px");
        l->merc_px = num(lv, "merc_px");
        l->origin_x = cJSON_GetNumberValue(cJSON_GetArrayItem(origin, 0));
        l->origin_y = cJSON_GetNumberValue(cJSON_GetArrayItem(origin, 1));
        double cols = num(lv, "cols"), rows = num(lv, "rows");
        if (!(l->radius_nm > 0) || !(l->m_per_px > 0) || !(l->merc_px > 0) || isnan(l->origin_x) ||
            isnan(l->origin_y) || !(cols >= 1 && cols <= 4096) || !(rows >= 1 && rows <= 4096)) {
            snprintf(err, errlen, "manifest: bad georeference for %s", l->file);
            goto out;
        }
        l->cols = (int)cols;
        l->rows = (int)rows;
        m->nlevels++;
    }
    if (m->nlevels == 0) {
        snprintf(err, errlen, "manifest: no zoom levels");
        goto out;
    }
    qsort(m->levels, m->nlevels, sizeof(m->levels[0]), level_cmp);
    ok = true;
out:
    cJSON_Delete(root);
    return ok;
}

bool map_store_get_manifest(map_manifest_t *out)
{
    char err[64];
    return parse_manifest(MAP_DIR, out, err, sizeof(err));
}

// ---------------------------------------------------------------------------
// Streaming ZIP reader
// ---------------------------------------------------------------------------

typedef enum { Z_HEADER, Z_NAME, Z_EXTRA, Z_DATA, Z_DONE, Z_ERROR } zstate_t;

struct map_upload {
    zstate_t state;
    uint8_t hdr[30];
    size_t have;            // bytes collected for the current header/name
    uint32_t data_left;
    uint16_t name_len, extra_left;
    char name[64];
    FILE *out;
    int nfiles;
    char err[200];
};

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void zfail(map_upload_t *u, const char *msg)
{
    if (u->state != Z_ERROR) {
        strlcpy(u->err, msg, sizeof(u->err));
        u->state = Z_ERROR;
    }
}

map_upload_t *map_upload_begin(char *err, size_t errlen)
{
    if (!s_card_ok) {
        snprintf(err, errlen, "no microSD card");
        return NULL;
    }
    remove_dir(NEW_DIR);
    if (mkdir(NEW_DIR, 0755) != 0) {
        snprintf(err, errlen, "could not create %s on the microSD card: %s", NEW_DIR, strerror(errno));
        ESP_LOGE(TAG, "%s", err);
        return NULL;
    }
    map_upload_t *u = calloc(1, sizeof(map_upload_t));
    if (!u) snprintf(err, errlen, "out of memory");
    return u;
}

static void start_entry(map_upload_t *u)
{
    // Keep only the base name, in case the zip was re-made with a folder.
    char *base = strrchr(u->name, '/');
    base = base ? base + 1 : u->name;
    if (!base[0]) { // directory entry
        u->out = NULL;
        return;
    }
    for (const char *c = base; *c; c++) {
        if (!(isalnum((unsigned char)*c) || *c == '_' || *c == '-' || *c == '.') || base[0] == '.') {
            zfail(u, "zip contains an unexpected file name");
            return;
        }
    }
    if (++u->nfiles > MAX_FILES) {
        zfail(u, "too many files in zip");
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", NEW_DIR, base);
    u->out = fopen(path, "wb");
    if (!u->out) {
        char msg[200];
        snprintf(msg, sizeof(msg), "could not create %s on the microSD card: %s", path, strerror(errno));
        ESP_LOGE(TAG, "%s", msg);
        zfail(u, msg);
    }
    else setvbuf(u->out, NULL, _IOFBF, WRITE_BUF);
}

esp_err_t map_upload_feed(map_upload_t *u, const uint8_t *data, size_t len)
{
    while (len > 0 && u->state != Z_ERROR) {
        switch (u->state) {
        case Z_HEADER: {
            size_t take = sizeof(u->hdr) - u->have;
            if (u->have < 4) take = 4 - u->have; // read the signature first
            if (take > len) take = len;
            memcpy(u->hdr + u->have, data, take);
            u->have += take;
            data += take;
            len -= take;
            if (u->have == 4) {
                uint32_t sig = rd32(u->hdr);
                if (sig == 0x02014b50 || sig == 0x06054b50) { // central directory: all entries read
                    u->state = Z_DONE;
                    break;
                }
                if (sig != 0x04034b50) {
                    zfail(u, "not a zip file");
                    break;
                }
            }
            if (u->have < sizeof(u->hdr)) break;
            uint16_t flags = rd16(u->hdr + 6), method = rd16(u->hdr + 8);
            uint32_t csize = rd32(u->hdr + 18), usize = rd32(u->hdr + 22);
            u->name_len = rd16(u->hdr + 26);
            u->extra_left = rd16(u->hdr + 28);
            if (method != 0 || (flags & 0x0008) || csize != usize) {
                zfail(u, "zip must be uncompressed (use the map builder's map_set.zip)");
                break;
            }
            if (csize > MAX_FILE_SIZE) {
                zfail(u, "file in zip too large");
                break;
            }
            if (u->name_len == 0 || u->name_len >= sizeof(u->name)) {
                zfail(u, "bad file name in zip");
                break;
            }
            u->data_left = csize;
            u->have = 0;
            u->state = Z_NAME;
            break;
        }
        case Z_NAME: {
            size_t take = u->name_len - u->have;
            if (take > len) take = len;
            memcpy(u->name + u->have, data, take);
            u->have += take;
            data += take;
            len -= take;
            if (u->have == u->name_len) {
                u->name[u->name_len] = '\0';
                u->state = Z_EXTRA;
            }
            break;
        }
        case Z_EXTRA: {
            size_t take = u->extra_left < len ? u->extra_left : len;
            u->extra_left -= take;
            data += take;
            len -= take;
            if (u->extra_left == 0) {
                start_entry(u);
                if (u->state == Z_ERROR) break;
                u->state = u->data_left ? Z_DATA : Z_HEADER;
                if (u->state == Z_HEADER && u->out) {
                    fclose(u->out);
                    u->out = NULL;
                }
                u->have = 0;
            }
            break;
        }
        case Z_DATA: {
            size_t take = u->data_left < len ? u->data_left : len;
            if (u->out && fwrite(data, 1, take, u->out) != take) {
                char msg[200];
                snprintf(msg, sizeof(msg), "writing %s to the microSD card failed: %s", u->name, strerror(errno));
                ESP_LOGE(TAG, "%s", msg);
                zfail(u, msg);
                break;
            }
            u->data_left -= take;
            data += take;
            len -= take;
            if (u->data_left == 0) {
                if (u->out && fclose(u->out) != 0) {
                    u->out = NULL;
                    char msg[200];
                    snprintf(msg, sizeof(msg), "writing %s to the microSD card failed: %s", u->name, strerror(errno));
                    ESP_LOGE(TAG, "%s", msg);
                    zfail(u, msg);
                    break;
                }
                u->out = NULL;
                u->have = 0;
                u->state = Z_HEADER;
            }
            break;
        }
        case Z_DONE:
            return ESP_OK; // ignore the central directory
        case Z_ERROR:
            break;
        }
    }
    return u->state == Z_ERROR ? ESP_FAIL : ESP_OK;
}

static void cleanup(map_upload_t *u)
{
    if (u->out) fclose(u->out);
    free(u);
}

void map_upload_abort(map_upload_t *u)
{
    if (!u) return;
    cleanup(u);
    remove_dir(NEW_DIR);
}

esp_err_t map_upload_finish(map_upload_t *u, char *err, size_t errlen)
{
    if (u->state == Z_ERROR) {
        snprintf(err, errlen, "%s", u->err);
        map_upload_abort(u);
        return ESP_FAIL;
    }
    if ((u->state != Z_DONE && u->state != Z_HEADER) || (u->state == Z_HEADER && u->have)) {
        snprintf(err, errlen, "upload was truncated");
        map_upload_abort(u);
        return ESP_FAIL;
    }
    cleanup(u);

    map_manifest_t m;
    if (!parse_manifest(NEW_DIR, &m, err, errlen)) {
        remove_dir(NEW_DIR);
        return ESP_FAIL;
    }
    for (int i = 0; i < m.nlevels; i++) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", NEW_DIR, m.levels[i].file);
        if (!map_pack_check(path, m.levels[i].cols, m.levels[i].rows, MAP_TILE_PX, err, errlen)) {
            // Report the file name rather than our scratch path.
            char why[96];
            strlcpy(why, err, sizeof(why));
            char *p = strstr(why, NEW_DIR "/");
            snprintf(err, errlen, "%s", p ? p + strlen(NEW_DIR "/") : why);
            remove_dir(NEW_DIR);
            return ESP_FAIL;
        }
    }

    if (s_on_replace) s_on_replace();
    remove_dir(MAP_DIR);
    if (rename(NEW_DIR, MAP_DIR) != 0) {
        snprintf(err, errlen, "could not install the chart: renaming %s failed: %s", NEW_DIR, strerror(errno));
        ESP_LOGE(TAG, "%s", err);
        remove_dir(NEW_DIR);
        settings_notify_map_changed(); // the old chart set may be partly gone
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "installed map: %d levels centred %.5f,%.5f", m.nlevels, m.center_lat, m.center_lon);

    // The chart's centre becomes the configured position.
    settings_t cfg;
    settings_get(&cfg);
    cfg.center_lat = m.center_lat;
    cfg.center_lon = m.center_lon;
    cfg.map_created = m.created;
    if (cfg.zoom_index >= m.nlevels) cfg.zoom_index = m.nlevels - 1;
    settings_set(&cfg);
    settings_notify_map_changed();
    return ESP_OK;
}
