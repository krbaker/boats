// Chart tiles for the map screen: a background task decodes tiles from the
// microSD tile packs into an RGB565 cache in PSRAM.
//
// Each frame, the map asks for the tiles it needs (map_tiles_get). Tiles not
// yet decoded are queued; map_tiles_poll reports when new ones have arrived so
// the map can redraw.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "map_store.h"

typedef enum {
    MAP_TILES_NO_CARD,      // no microSD card
    MAP_TILES_NONE,         // card, but no chart set on it
    MAP_TILES_LOADING,
    MAP_TILES_READY,
    MAP_TILES_ERROR,
} map_tiles_state_t;

void map_tiles_init(void);

// Re-read the chart set (after an upload) in the background.
void map_tiles_reload(void);

// LVGL task. Installs freshly decoded tiles / a newly loaded chart set.
// Returns true if anything changed, so the map should redraw.
bool map_tiles_poll(void);

map_tiles_state_t map_tiles_state(void);

// Valid while state is READY (LVGL task only).
const map_manifest_t *map_tiles_manifest(void);

// LVGL task, while drawing a frame: begin, then get (visible tiles) and
// prefetch (tiles about to come into view), then end.
// get returns the decoded tile, or NULL if it is blank or still loading.
void map_tiles_frame_begin(void);
const lv_image_dsc_t *map_tiles_get(int level, int col, int row);
void map_tiles_prefetch(int level, int col, int row);
void map_tiles_frame_end(void);

// Decode a PNG tile into a native RGB565 image (pixels malloc'd, in PSRAM).
bool map_tile_decode(const uint8_t *png, size_t len, lv_image_dsc_t *dsc);
