// Cache of vessels' static data (name, call sign, type, size), kept in RAM
// and logged to a file so it survives restarts. See VESSEL_CACHE_PLAN.md.
//
// Pure C (stdio + pthreads) with no ESP-IDF dependencies so it can be
// unit-tested on the host. Thread-safe: lookups and updates only touch RAM;
// vc_flush() does the file I/O without blocking them for long.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bits in vc_vessel_t.fields marking which values are known.
#define VC_F_NAME       (1u << 0)
#define VC_F_CALLSIGN   (1u << 1)
#define VC_F_SHIPTYPE   (1u << 2)
#define VC_F_DIM        (1u << 3)
#define VC_F_ALL        (VC_F_NAME | VC_F_CALLSIGN | VC_F_SHIPTYPE | VC_F_DIM)

#define VC_LOG_NAME     "cache.log"
#define VC_TMP_NAME     "cache.tmp"
#define VC_RECORD_SIZE  64

#define VC_WRITE_INTERVAL_S (10 * 60)       // at most one write per vessel this often
#define VC_LAST_SEEN_WRITE_S (24 * 60 * 60) // write "last seen" when the stored one is this old

typedef struct {
    uint32_t mmsi;
    uint8_t cls;            // ais_class_t; 0 = unknown
    uint8_t ship_type;
    uint16_t fields;        // VC_F_*
    uint16_t dim_a, dim_b;
    uint8_t dim_c, dim_d;
    char name[21];
    char callsign[8];
    uint32_t first_seen;    // Unix time; 0 if the clock wasn't set
    uint32_t last_seen;
    uint32_t last_changed;
} vc_vessel_t;

typedef struct {
    int count;              // vessels in the cache
    int capacity;
    int file_records;       // records in the log (count + superseded ones)
    uint32_t records_written;
    uint32_t compactions;
    bool ram_only;          // no directory (no card): nothing is saved
    char last_error[96];    // "" if the last file operation worked
} vc_stats_t;

typedef struct vessel_cache vessel_cache_t;

// Loads dir/cache.log (creating dir if needed). dir NULL runs in RAM only.
// Returns NULL only if out of memory.
vessel_cache_t *vc_open(const char *dir, int capacity);
void vc_close(vessel_cache_t *vc);

// MMSIs the cache ignores: not 9 digits, or a typical unconfigured default.
bool vc_mmsi_valid(uint32_t mmsi);

// Copy a vessel's cached data. False if it isn't cached.
bool vc_get(vessel_cache_t *vc, uint32_t mmsi, vc_vessel_t *out);

// Merge what was just received for a vessel. in->fields marks the static
// values present (a value that's absent never clears a cached one); in->cls
// is ignored when 0. Call for every message, also with fields 0, so "last
// seen" stays current. A vessel isn't added until it sends static data.
// wall is the Unix time (0 if unknown). Returns true if anything changed.
bool vc_update(vessel_cache_t *vc, const vc_vessel_t *in, uint32_t wall);

// Write changed vessels to the log, and compact it when needed. now_s is a
// monotonic clock in seconds, for the per-vessel write limit; force ignores
// the limit (before a restart). Returns records written, or -1 on error.
int vc_flush(vessel_cache_t *vc, uint32_t now_s, bool force);

// Forget every vessel, in RAM and on file.
void vc_clear(vessel_cache_t *vc);

void vc_get_stats(vessel_cache_t *vc, vc_stats_t *out);

#ifdef __cplusplus
}
#endif
