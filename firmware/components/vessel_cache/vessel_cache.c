// Vessel static-data cache: see vessel_cache.h and VESSEL_CACHE_PLAN.md.
//
// RAM: a dense array of entries plus an open-addressing (linear probing)
// index by MMSI. File: an append-only log of fixed 64-byte records, each a
// full snapshot of one vessel with its own CRC. On load a later record for an
// MMSI replaces an earlier one. Compaction rewrites the log with one record
// per vessel, through a temporary file.

#define _POSIX_C_SOURCE 200809L // strnlen, fsync, mkdir under -std=c11

#include "vessel_cache.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAGIC           0x01004356u // "VC", format version 1, little-endian
#define LOAD_CHUNK      64          // records read at a time
#define COMPACT_SLACK   256         // superseded records tolerated before compacting

typedef struct {
    vc_vessel_t v;
    uint32_t disk_last_seen; // last_seen as last written to the log
    uint32_t next_write_s;   // not written again before this (monotonic)
    bool dirty;              // differs from what's in the log
} entry_t;

struct vessel_cache {
    pthread_mutex_t lock; // entries and index
    pthread_mutex_t io;   // the files; taken before lock, never after
    char *log_path, *tmp_path;
    entry_t *e;
    int count, capacity;
    int32_t *slot; // entry index, -1 = empty
    uint32_t mask;
    int file_records;
    bool need_compact;
    uint32_t records_written, compactions;
    char last_error[96];
};

// ---------------------------------------------------------------------------
// Index
// ---------------------------------------------------------------------------

static uint32_t home(const vessel_cache_t *vc, uint32_t mmsi)
{
    return (mmsi * 2654435761u) & vc->mask;
}

static int32_t find_slot(const vessel_cache_t *vc, uint32_t mmsi)
{
    for (uint32_t h = home(vc, mmsi);; h = (h + 1) & vc->mask) {
        if (vc->slot[h] < 0) return -1;
        if (vc->e[vc->slot[h]].v.mmsi == mmsi) return (int32_t)h;
    }
}

static entry_t *find(const vessel_cache_t *vc, uint32_t mmsi)
{
    int32_t h = find_slot(vc, mmsi);
    return h < 0 ? NULL : &vc->e[vc->slot[h]];
}

static void index_insert(vessel_cache_t *vc, uint32_t mmsi, int32_t idx)
{
    uint32_t h = home(vc, mmsi);
    while (vc->slot[h] >= 0) h = (h + 1) & vc->mask;
    vc->slot[h] = idx;
}

// Backward-shift deletion: no tombstones, so probes stay short.
static void index_remove(vessel_cache_t *vc, uint32_t mmsi)
{
    int32_t found = find_slot(vc, mmsi);
    if (found < 0) return;
    uint32_t i = (uint32_t)found;
    for (uint32_t j = (i + 1) & vc->mask; vc->slot[j] >= 0; j = (j + 1) & vc->mask) {
        uint32_t k = home(vc, vc->e[vc->slot[j]].v.mmsi);
        if (((j - k) & vc->mask) >= ((j - i) & vc->mask)) { // i is on j's probe path
            vc->slot[i] = vc->slot[j];
            i = j;
        }
    }
    vc->slot[i] = -1;
}

// A new, zeroed entry for mmsi. When full, the vessel seen least recently
// makes room.
static entry_t *add(vessel_cache_t *vc, uint32_t mmsi)
{
    int32_t idx;
    if (vc->count < vc->capacity) {
        idx = vc->count++;
    } else {
        idx = 0;
        for (int i = 1; i < vc->count; i++) {
            if (vc->e[i].v.last_seen < vc->e[idx].v.last_seen) idx = i;
        }
        index_remove(vc, vc->e[idx].v.mmsi);
    }
    entry_t *e = &vc->e[idx];
    memset(e, 0, sizeof(*e));
    e->v.mmsi = mmsi;
    index_insert(vc, mmsi, idx);
    return e;
}

// ---------------------------------------------------------------------------
// Records
// ---------------------------------------------------------------------------

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint16_t get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

// Layout (VESSEL_CACHE_PLAN.md): magic 0, MMSI 4, class 8, ship type 9,
// fields 10, dims 12-17, name 18 (20), call sign 38 (7), first seen 45,
// last seen 49, last changed 53, reserved 57 (3), CRC-32 60.
static void encode(const vc_vessel_t *v, uint8_t *r)
{
    memset(r, 0, VC_RECORD_SIZE);
    put32(r, MAGIC);
    put32(r + 4, v->mmsi);
    r[8] = v->cls;
    r[9] = v->ship_type;
    put16(r + 10, v->fields);
    put16(r + 12, v->dim_a);
    put16(r + 14, v->dim_b);
    r[16] = v->dim_c;
    r[17] = v->dim_d;
    memcpy(r + 18, v->name, strnlen(v->name, 20));
    memcpy(r + 38, v->callsign, strnlen(v->callsign, 7));
    put32(r + 45, v->first_seen);
    put32(r + 49, v->last_seen);
    put32(r + 53, v->last_changed);
    put32(r + 60, crc32(r, 60));
}

static bool decode(const uint8_t *r, vc_vessel_t *v)
{
    if (get32(r) != MAGIC || get32(r + 60) != crc32(r, 60)) return false;
    memset(v, 0, sizeof(*v));
    v->mmsi = get32(r + 4);
    v->cls = r[8];
    v->ship_type = r[9];
    v->fields = get16(r + 10) & VC_F_ALL;
    v->dim_a = get16(r + 12);
    v->dim_b = get16(r + 14);
    v->dim_c = r[16];
    v->dim_d = r[17];
    memcpy(v->name, r + 18, 20);
    memcpy(v->callsign, r + 38, 7);
    v->first_seen = get32(r + 45);
    v->last_seen = get32(r + 49);
    v->last_changed = get32(r + 53);
    return true;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// The file state below is changed with io held, and also under lock so that
// vc_get_stats() doesn't wait for file I/O.
static void set_error(vessel_cache_t *vc, const char *what)
{
    int err = errno;
    pthread_mutex_lock(&vc->lock);
    snprintf(vc->last_error, sizeof(vc->last_error), "%s: %s", what, strerror(err));
    pthread_mutex_unlock(&vc->lock);
}

static void set_error_text(vessel_cache_t *vc, const char *text)
{
    pthread_mutex_lock(&vc->lock);
    snprintf(vc->last_error, sizeof(vc->last_error), "%s", text);
    pthread_mutex_unlock(&vc->lock);
}

// After n records were written: a compaction (the log now holds just them) or
// an append.
static void written(vessel_cache_t *vc, int n, bool compacted)
{
    pthread_mutex_lock(&vc->lock);
    vc->file_records = compacted ? n : vc->file_records + n;
    vc->records_written += n;
    if (compacted) vc->compactions++;
    vc->last_error[0] = '\0';
    if (vc->file_records > 2 * vc->count + COMPACT_SLACK) vc->need_compact = true;
    pthread_mutex_unlock(&vc->lock);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void load(vessel_cache_t *vc)
{
    // A crash during compaction can leave the new log as cache.tmp (old one
    // already deleted), or a half-written cache.tmp beside the old log.
    if (exists(vc->tmp_path)) {
        if (exists(vc->log_path)) remove(vc->tmp_path);
        else rename(vc->tmp_path, vc->log_path);
    }
    FILE *f = fopen(vc->log_path, "rb");
    if (!f) {
        if (errno != ENOENT) set_error(vc, "opening the log");
        return;
    }
    uint8_t *buf = malloc(LOAD_CHUNK * VC_RECORD_SIZE);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t n;
    while ((n = fread(buf, 1, LOAD_CHUNK * VC_RECORD_SIZE, f)) > 0) {
        if (n % VC_RECORD_SIZE) vc->need_compact = true; // torn last record: rewrite before appending
        for (size_t off = 0; off + VC_RECORD_SIZE <= n; off += VC_RECORD_SIZE) {
            vc_vessel_t v;
            if (!decode(buf + off, &v) || !vc_mmsi_valid(v.mmsi)) {
                vc->need_compact = true;
                continue;
            }
            vc->file_records++;
            entry_t *e = find(vc, v.mmsi);
            if (!e) e = add(vc, v.mmsi);
            e->v = v;
            e->disk_last_seen = v.last_seen;
        }
    }
    if (ferror(f)) set_error(vc, "reading the log");
    free(buf);
    fclose(f);
    if (vc->file_records > 2 * vc->count + COMPACT_SLACK) vc->need_compact = true;
}

static bool write_all(vessel_cache_t *vc, FILE *f, const uint8_t *buf, size_t n, const char *what)
{
    if (fwrite(buf, VC_RECORD_SIZE, n, f) != n || fflush(f) != 0 || fsync(fileno(f)) != 0) {
        set_error(vc, what);
        fclose(f);
        return false;
    }
    if (fclose(f) != 0) {
        set_error(vc, what);
        return false;
    }
    return true;
}

// Marks the vessels in buf as needing a write again, after a failed one.
static void redirty(vessel_cache_t *vc, const uint8_t *buf, int n)
{
    pthread_mutex_lock(&vc->lock);
    for (int i = 0; i < n; i++) {
        entry_t *e = find(vc, get32(buf + i * VC_RECORD_SIZE + 4));
        if (e) {
            e->dirty = true;
            e->next_write_s = 0;
        }
    }
    pthread_mutex_unlock(&vc->lock);
}

// Called with io held. Returns records written, or -1.
static int compact(vessel_cache_t *vc)
{
    pthread_mutex_lock(&vc->lock);
    int n = vc->count;
    uint8_t *buf = malloc(n ? (size_t)n * VC_RECORD_SIZE : 1);
    if (buf) {
        for (int i = 0; i < n; i++) {
            entry_t *e = &vc->e[i];
            encode(&e->v, buf + i * VC_RECORD_SIZE);
            e->dirty = false;
            e->disk_last_seen = e->v.last_seen;
        }
    }
    pthread_mutex_unlock(&vc->lock);
    if (!buf) {
        set_error_text(vc, "out of memory compacting the log");
        return -1;
    }

    FILE *f = fopen(vc->tmp_path, "wb");
    bool ok = f ? write_all(vc, f, buf, n, "writing the compacted log") : (set_error(vc, "creating the compacted log"), false);
    if (ok && remove(vc->log_path) != 0 && errno != ENOENT) ok = (set_error(vc, "deleting the old log"), false);
    if (ok && rename(vc->tmp_path, vc->log_path) != 0) ok = (set_error(vc, "renaming the compacted log"), false);
    if (!ok) {
        redirty(vc, buf, n);
        free(buf);
        return -1;
    }
    free(buf);
    vc->need_compact = false;
    written(vc, n, true);
    return n;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

static char *join(const char *dir, const char *name)
{
    size_t len = strlen(dir) + strlen(name) + 2;
    char *p = malloc(len);
    if (p) snprintf(p, len, "%s/%s", dir, name);
    return p;
}

vessel_cache_t *vc_open(const char *dir, int capacity)
{
    vessel_cache_t *vc = calloc(1, sizeof(*vc));
    if (!vc) return NULL;
    pthread_mutex_init(&vc->lock, NULL);
    pthread_mutex_init(&vc->io, NULL);
    uint32_t size = 16;
    while (size < 2u * (uint32_t)capacity) size <<= 1; // load factor at most 1/2
    vc->capacity = capacity;
    vc->mask = size - 1;
    vc->e = calloc(capacity, sizeof(entry_t));
    vc->slot = malloc(size * sizeof(int32_t));
    if (dir) {
        vc->log_path = join(dir, VC_LOG_NAME);
        vc->tmp_path = join(dir, VC_TMP_NAME);
    }
    if (!vc->e || !vc->slot || (dir && (!vc->log_path || !vc->tmp_path))) {
        vc_close(vc);
        return NULL;
    }
    memset(vc->slot, 0xff, size * sizeof(int32_t));
    if (dir) {
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) set_error(vc, "creating the cache directory");
        load(vc);
    }
    return vc;
}

void vc_close(vessel_cache_t *vc)
{
    if (!vc) return;
    pthread_mutex_destroy(&vc->lock);
    pthread_mutex_destroy(&vc->io);
    free(vc->e);
    free(vc->slot);
    free(vc->log_path);
    free(vc->tmp_path);
    free(vc);
}

bool vc_mmsi_valid(uint32_t mmsi)
{
    if (mmsi < 100000000u || mmsi > 999999999u) return false;
    if (mmsi == 123456789u || mmsi == 987654321u) return false;
    uint32_t d = mmsi % 10, m = mmsi;
    while (m && m % 10 == d) m /= 10;
    return m != 0; // all nine digits the same: 111111111 etc.
}

bool vc_get(vessel_cache_t *vc, uint32_t mmsi, vc_vessel_t *out)
{
    pthread_mutex_lock(&vc->lock);
    entry_t *e = find(vc, mmsi);
    if (e) *out = e->v;
    pthread_mutex_unlock(&vc->lock);
    return e != NULL;
}

// Copies the static values present in `in` into v. True if any differed.
static bool merge(vc_vessel_t *v, const vc_vessel_t *in)
{
    bool changed = false;
    if (in->cls && in->cls != v->cls) {
        v->cls = in->cls;
        changed = true;
    }
    if ((in->fields & VC_F_NAME) && (!(v->fields & VC_F_NAME) || strcmp(v->name, in->name) != 0)) {
        snprintf(v->name, sizeof(v->name), "%s", in->name);
        changed = true;
    }
    if ((in->fields & VC_F_CALLSIGN) && (!(v->fields & VC_F_CALLSIGN) || strcmp(v->callsign, in->callsign) != 0)) {
        snprintf(v->callsign, sizeof(v->callsign), "%s", in->callsign);
        changed = true;
    }
    if ((in->fields & VC_F_SHIPTYPE) && (!(v->fields & VC_F_SHIPTYPE) || v->ship_type != in->ship_type)) {
        v->ship_type = in->ship_type;
        changed = true;
    }
    if ((in->fields & VC_F_DIM) && (!(v->fields & VC_F_DIM) || v->dim_a != in->dim_a || v->dim_b != in->dim_b ||
                                    v->dim_c != in->dim_c || v->dim_d != in->dim_d)) {
        v->dim_a = in->dim_a;
        v->dim_b = in->dim_b;
        v->dim_c = in->dim_c;
        v->dim_d = in->dim_d;
        changed = true;
    }
    v->fields |= in->fields & VC_F_ALL;
    return changed;
}

bool vc_update(vessel_cache_t *vc, const vc_vessel_t *in, uint32_t wall)
{
    if (!vc_mmsi_valid(in->mmsi)) return false;
    pthread_mutex_lock(&vc->lock);
    entry_t *e = find(vc, in->mmsi);
    if (!e && !(in->fields & VC_F_ALL)) {
        pthread_mutex_unlock(&vc->lock);
        return false; // nothing worth remembering yet
    }
    if (!e) e = add(vc, in->mmsi);
    bool changed = merge(&e->v, in);
    if (changed) {
        e->v.last_changed = wall;
        e->dirty = true;
    }
    if (wall) {
        if (!e->v.first_seen) e->v.first_seen = wall;
        e->v.last_seen = wall;
        if (wall - e->disk_last_seen >= VC_LAST_SEEN_WRITE_S) e->dirty = true;
    }
    pthread_mutex_unlock(&vc->lock);
    return changed;
}

int vc_flush(vessel_cache_t *vc, uint32_t now_s, bool force)
{
    if (!vc->log_path) return 0;
    pthread_mutex_lock(&vc->io);
    if (vc->need_compact) {
        int n = compact(vc);
        pthread_mutex_unlock(&vc->io);
        return n;
    }

    pthread_mutex_lock(&vc->lock);
    int n = 0;
    for (int i = 0; i < vc->count; i++) {
        if (vc->e[i].dirty && (force || now_s >= vc->e[i].next_write_s)) n++;
    }
    uint8_t *buf = n ? malloc((size_t)n * VC_RECORD_SIZE) : NULL;
    if (buf) {
        int k = 0;
        for (int i = 0; i < vc->count && k < n; i++) {
            entry_t *e = &vc->e[i];
            if (!e->dirty || !(force || now_s >= e->next_write_s)) continue;
            encode(&e->v, buf + k++ * VC_RECORD_SIZE);
            e->dirty = false;
            e->disk_last_seen = e->v.last_seen;
            e->next_write_s = now_s + VC_WRITE_INTERVAL_S;
        }
    }
    pthread_mutex_unlock(&vc->lock);
    if (!n) {
        pthread_mutex_unlock(&vc->io);
        return 0;
    }
    if (!buf) {
        set_error_text(vc, "out of memory writing the log");
        pthread_mutex_unlock(&vc->io);
        return -1;
    }

    FILE *f = fopen(vc->log_path, "ab");
    bool ok = f ? write_all(vc, f, buf, n, "appending to the log") : (set_error(vc, "opening the log"), false);
    if (ok) {
        written(vc, n, false);
        if (vc->need_compact && compact(vc) < 0) ok = false;
    } else {
        redirty(vc, buf, n);
        vc->need_compact = true; // a partial append leaves a torn record
    }
    free(buf);
    pthread_mutex_unlock(&vc->io);
    return ok ? n : -1;
}

void vc_clear(vessel_cache_t *vc)
{
    pthread_mutex_lock(&vc->io);
    pthread_mutex_lock(&vc->lock);
    vc->count = 0;
    memset(vc->slot, 0xff, (vc->mask + 1) * sizeof(int32_t));
    vc->file_records = 0;
    vc->need_compact = false;
    vc->last_error[0] = '\0';
    pthread_mutex_unlock(&vc->lock);
    if (vc->log_path) {
        if (remove(vc->log_path) != 0 && errno != ENOENT) set_error(vc, "deleting the log");
        remove(vc->tmp_path);
    }
    pthread_mutex_unlock(&vc->io);
}

void vc_get_stats(vessel_cache_t *vc, vc_stats_t *out)
{
    pthread_mutex_lock(&vc->lock);
    out->count = vc->count;
    out->capacity = vc->capacity;
    out->file_records = vc->file_records;
    out->records_written = vc->records_written;
    out->compactions = vc->compactions;
    out->ram_only = vc->log_path == NULL;
    snprintf(out->last_error, sizeof(out->last_error), "%s", vc->last_error);
    pthread_mutex_unlock(&vc->lock);
}
