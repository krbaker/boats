// Chart storage: the map set (manifest + one tile pack per zoom level) lives
// on the microSD card in /sdcard/ais_map. It arrives either as an upload of
// the map builder's map_set.zip, or copied onto the card by hand.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "geo.h"

#ifndef MAP_ROOT
#define MAP_ROOT            "/sdcard"   // overridden by the host tests
#endif
#define MAP_DIR             MAP_ROOT "/ais_map"
#define MAP_MAX_LEVELS      8
#define MAP_TILE_PX         256

typedef struct {
    char file[16];          // tile pack, e.g. "z0.pak"
    float radius_nm;        // view radius at this zoom
    float m_per_px;         // ground metres per pixel at the chart centre
    double merc_px;         // Web Mercator metres per pixel
    double origin_x;        // EPSG:3857 metres of the tile grid's top-left corner
    double origin_y;
    int cols, rows;
} map_level_t;

typedef struct {
    uint32_t created;       // build time; identifies the chart set
    double center_lat, center_lon;
    float pan_nm;           // how far from the centre the view may pan
    int nlevels;
    map_level_t levels[MAP_MAX_LEVELS];  // sorted most zoomed-in first
} map_manifest_t;

// Mount the microSD card. If the chart set on it is new (copied on by hand),
// its centre becomes the configured position.
esp_err_t map_store_init(void);
bool map_store_card_ok(void);

// Read the current manifest. Returns false if there is no (valid) chart set.
bool map_store_get_manifest(map_manifest_t *out);

// Streaming upload of a map_set.zip (STORED entries only).
typedef struct map_upload map_upload_t;
// Returns NULL, with err set, if the card can't be written.
map_upload_t *map_upload_begin(char *err, size_t errlen);
esp_err_t map_upload_feed(map_upload_t *u, const uint8_t *data, size_t len);
// Validate, install, update the configured centre. On failure err explains why.
esp_err_t map_upload_finish(map_upload_t *u, char *err, size_t errlen);
void map_upload_abort(map_upload_t *u);

// Called just before an upload replaces the chart set, so whoever reads the
// tile packs can close them first (FAT doesn't stop open files being deleted).
void map_store_on_replace(void (*fn)(void));

// Card capacity and free space in bytes (0 if no card).
void map_store_usage(uint64_t *total, uint64_t *free_bytes);
