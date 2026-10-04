#include "map_pack.h"

#include <stdlib.h>
#include <string.h>

static const char k_magic[8] = "AISPAK1";  // includes the trailing NUL

static uint16_t rd16(const uint8_t *b) { return b[0] | b[1] << 8; }
static uint32_t rd32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

bool map_pack_open(map_pack_t *p, const char *path, char *err, size_t errlen)
{
    memset(p, 0, sizeof(*p));
    p->f = fopen(path, "rb");
    if (!p->f) {
        snprintf(err, errlen, "cannot open %s", path);
        return false;
    }
    uint8_t h[MAP_PACK_HEADER_SIZE];
    if (fread(h, 1, sizeof(h), p->f) != sizeof(h) || memcmp(h, k_magic, 8) != 0) {
        snprintf(err, errlen, "%s is not a tile pack", path);
        goto fail;
    }
    p->cols = rd16(h + 8);
    p->rows = rd16(h + 10);
    p->tile_px = rd16(h + 12);
    size_t n = (size_t)p->cols * p->rows;
    uint8_t *raw = malloc(n * 8);
    p->index = malloc(n * sizeof(map_pack_entry_t));
    if (!n || !raw || !p->index || fread(raw, 8, n, p->f) != n) {
        free(raw);
        snprintf(err, errlen, "%s: bad tile index", path);
        goto fail;
    }
    for (size_t i = 0; i < n; i++) {
        p->index[i].offset = rd32(raw + i * 8);
        p->index[i].length = rd32(raw + i * 8 + 4);
    }
    free(raw);
    return true;
fail:
    map_pack_close(p);
    return false;
}

void map_pack_close(map_pack_t *p)
{
    if (p->f) fclose(p->f);
    free(p->index);
    memset(p, 0, sizeof(*p));
}

uint8_t *map_pack_read(map_pack_t *p, int col, int row, size_t *len)
{
    *len = 1;
    if (!p->f || col < 0 || row < 0 || col >= p->cols || row >= p->rows) return NULL;
    const map_pack_entry_t *e = &p->index[(size_t)row * p->cols + col];
    *len = 0;
    if (e->length == 0) return NULL;
    uint8_t *buf = malloc(e->length);
    if (!buf || fseek(p->f, e->offset, SEEK_SET) != 0 || fread(buf, 1, e->length, p->f) != e->length) {
        free(buf);
        *len = 1;
        return NULL;
    }
    *len = e->length;
    return buf;
}

bool map_pack_check(const char *path, int cols, int rows, int tile_px, char *err, size_t errlen)
{
    map_pack_t p;
    if (!map_pack_open(&p, path, err, errlen)) return false;
    bool ok = false;
    if (p.cols != cols || p.rows != rows || p.tile_px != tile_px) {
        snprintf(err, errlen, "%s is %dx%d tiles of %d px, manifest says %dx%d of %d", path, p.cols, p.rows,
                 p.tile_px, cols, rows, tile_px);
        goto out;
    }
    fseek(p.f, 0, SEEK_END);
    long size = ftell(p.f);
    for (size_t i = 0; i < (size_t)cols * rows; i++) {
        const map_pack_entry_t *e = &p.index[i];
        if (e->length && (uint64_t)e->offset + e->length > (uint64_t)size) {
            snprintf(err, errlen, "%s is truncated", path);
            goto out;
        }
    }
    ok = true;
out:
    map_pack_close(&p);
    return ok;
}
