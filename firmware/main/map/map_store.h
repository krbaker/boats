// Chart storage: the uploaded map set (manifest + one PNG per zoom level)
// lives in LittleFS under /data/maps/cur.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "geo.h"

#ifndef MAP_ROOT
#define MAP_ROOT            "/data"     // overridden by the host tests
#endif
#define MAP_DIR             MAP_ROOT "/maps/cur"
#define MAP_MAX_LEVELS      8
#define MAP_SIZE_PX         800

typedef struct {
    char file[32];
    float radius_nm;
    float m_per_px;
    geo_bbox_t bbox;
} map_level_t;

typedef struct {
    double center_lat, center_lon;
    int width, height;
    int nlevels;
    map_level_t levels[MAP_MAX_LEVELS];  // sorted most zoomed-in first
} map_manifest_t;

esp_err_t map_store_init(void);

// Read the current manifest. Returns false if no map has been uploaded.
bool map_store_get_manifest(map_manifest_t *out);

// Streaming upload of a map_set.zip (STORED entries only).
typedef struct map_upload map_upload_t;
map_upload_t *map_upload_begin(void);
esp_err_t map_upload_feed(map_upload_t *u, const uint8_t *data, size_t len);
// Validate, install, update the configured centre. On failure err explains why.
esp_err_t map_upload_finish(map_upload_t *u, char *err, size_t errlen);
void map_upload_abort(map_upload_t *u);

void map_store_usage(size_t *total, size_t *used);
