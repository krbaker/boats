#include "map_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"

#include "settings.h"

static const char *TAG = "map_store";

#define NEW_DIR         MAP_ROOT "/maps/new"
#define MAX_FILES       16
#define MAX_FILE_SIZE   (4 * 1024 * 1024)

esp_err_t map_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = MAP_ROOT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    mkdir(MAP_ROOT "/maps", 0755);
    size_t total, used;
    map_store_usage(&total, &used);
    ESP_LOGI(TAG, "storage %u KiB used of %u KiB", (unsigned)(used / 1024), (unsigned)(total / 1024));
    return ESP_OK;
}

void map_store_usage(size_t *total, size_t *used)
{
    *total = *used = 0;
    esp_littlefs_info("storage", total, used);
}

static void remove_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char p[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof(p), "%s/%s", path, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static int level_cmp(const void *a, const void *b)
{
    float ra = ((const map_level_t *)a)->radius_nm, rb = ((const map_level_t *)b)->radius_nm;
    return (ra > rb) - (ra < rb);
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
    cJSON *lat = cJSON_GetObjectItem(root, "center_lat"), *lon = cJSON_GetObjectItem(root, "center_lon");
    cJSON *levels = cJSON_GetObjectItem(root, "levels");
    m->width = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "width"));
    m->height = cJSON_GetNumberValue(cJSON_GetObjectItem(root, "height"));
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) || fabs(lat->valuedouble) > 90 || fabs(lon->valuedouble) > 180) {
        snprintf(err, errlen, "manifest: bad centre position");
        goto out;
    }
    if (m->width != MAP_SIZE_PX || m->height != MAP_SIZE_PX) {
        snprintf(err, errlen, "maps must be %dx%d (got %dx%d)", MAP_SIZE_PX, MAP_SIZE_PX, m->width, m->height);
        goto out;
    }
    cJSON *proj = cJSON_GetObjectItem(root, "projection");
    if (!cJSON_IsString(proj) || strcmp(proj->valuestring, "EPSG:3857") != 0) {
        snprintf(err, errlen, "chart is from an older map builder; rebuild it");
        goto out;
    }
    m->center_lat = lat->valuedouble;
    m->center_lon = lon->valuedouble;

    cJSON *lv;
    cJSON_ArrayForEach(lv, levels) {
        if (m->nlevels >= MAP_MAX_LEVELS) break;
        map_level_t *l = &m->levels[m->nlevels];
        cJSON *file = cJSON_GetObjectItem(lv, "file"), *bbox = cJSON_GetObjectItem(lv, "bbox");
        if (!cJSON_IsString(file) || strchr(file->valuestring, '/') || cJSON_GetArraySize(bbox) != 4) {
            snprintf(err, errlen, "manifest: bad level entry");
            goto out;
        }
        strlcpy(l->file, file->valuestring, sizeof(l->file));
        l->radius_nm = cJSON_GetNumberValue(cJSON_GetObjectItem(lv, "radius_nm"));
        l->m_per_px = cJSON_GetNumberValue(cJSON_GetObjectItem(lv, "m_per_px"));
        l->bbox.x_min = cJSON_GetNumberValue(cJSON_GetArrayItem(bbox, 0));
        l->bbox.y_min = cJSON_GetNumberValue(cJSON_GetArrayItem(bbox, 1));
        l->bbox.x_max = cJSON_GetNumberValue(cJSON_GetArrayItem(bbox, 2));
        l->bbox.y_max = cJSON_GetNumberValue(cJSON_GetArrayItem(bbox, 3));
        if (!(l->m_per_px > 0) || !(l->bbox.x_max > l->bbox.x_min) || !(l->bbox.y_max > l->bbox.y_min)) {
            snprintf(err, errlen, "manifest: bad georeference for %s", l->file);
            goto out;
        }
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
    char err[64];
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

map_upload_t *map_upload_begin(void)
{
    remove_dir(NEW_DIR);
    mkdir(MAP_ROOT "/maps", 0755);
    if (mkdir(NEW_DIR, 0755) != 0) {
        ESP_LOGE(TAG, "mkdir %s failed", NEW_DIR);
        return NULL;
    }
    return calloc(1, sizeof(map_upload_t));
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
    if (!u->out) zfail(u, "could not write to storage");
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
                zfail(u, "storage full");
                break;
            }
            u->data_left -= take;
            data += take;
            len -= take;
            if (u->data_left == 0) {
                if (u->out) fclose(u->out);
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

static bool png_dims(const char *path, int *w, int *h)
{
    uint8_t b[24];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n < 24 || memcmp(b, "\x89PNG\r\n\x1a\n", 8) || memcmp(b + 12, "IHDR", 4)) return false;
    *w = b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
    *h = b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
    return true;
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
        int w, h;
        snprintf(path, sizeof(path), "%s/%s", NEW_DIR, m.levels[i].file);
        if (!png_dims(path, &w, &h)) {
            snprintf(err, errlen, "%s missing or not a PNG", m.levels[i].file);
            remove_dir(NEW_DIR);
            return ESP_FAIL;
        }
        if (w != MAP_SIZE_PX || h != MAP_SIZE_PX) {
            snprintf(err, errlen, "%s is %dx%d, expected %dx%d", m.levels[i].file, w, h, MAP_SIZE_PX, MAP_SIZE_PX);
            remove_dir(NEW_DIR);
            return ESP_FAIL;
        }
    }

    remove_dir(MAP_DIR);
    if (rename(NEW_DIR, MAP_DIR) != 0) {
        snprintf(err, errlen, "could not install map");
        remove_dir(NEW_DIR);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "installed map: %d levels centred %.5f,%.5f", m.nlevels, m.center_lat, m.center_lon);

    // The chart's centre becomes the configured position.
    settings_t cfg;
    settings_get(&cfg);
    cfg.center_lat = m.center_lat;
    cfg.center_lon = m.center_lon;
    if (cfg.zoom_index >= m.nlevels) cfg.zoom_index = m.nlevels - 1;
    settings_set(&cfg);
    settings_notify_map_changed();
    return ESP_OK;
}
