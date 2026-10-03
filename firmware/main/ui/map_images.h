// Decoded chart images (one RGB565 bitmap per zoom level, in PSRAM).

#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "map_store.h"

typedef enum {
    MAP_IMAGES_NONE,     // no chart uploaded
    MAP_IMAGES_LOADING,
    MAP_IMAGES_READY,
    MAP_IMAGES_ERROR,
} map_images_state_t;

void map_images_init(void);

// Start (re)loading the chart set in the background.
void map_images_reload(void);

// Call from the LVGL task. Returns true when a newly loaded set was swapped
// in since the last call (so the screen should refresh its image source).
bool map_images_poll(void);

map_images_state_t map_images_state(void);

// Valid while state is READY (LVGL task only).
const map_manifest_t *map_images_manifest(void);
const lv_image_dsc_t *map_images_level(int level);
