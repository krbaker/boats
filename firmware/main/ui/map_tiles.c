#include "map_tiles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "map_pack.h"

static const char *TAG = "map_tiles";

void lv_image_cache_drop(const void *src); // not exported via lvgl.h in 9.6

#define CACHE_TILES 96      // 256x256 RGB565 = 128 KiB each, ~12 MiB of PSRAM
#define MAX_WANT    64      // visible tiles plus a prefetch ring
#define MAX_DONE    8

typedef struct {
    int16_t level, col, row;
} tile_key_t;

typedef struct {
    tile_key_t key;
    bool valid, blank;
    uint32_t used;          // frame that last asked for it
    lv_image_dsc_t dsc;
} entry_t;

typedef struct {
    tile_key_t key;
    uint32_t gen;
    bool blank;
    lv_image_dsc_t dsc;
} result_t;

static inline bool key_eq(tile_key_t a, tile_key_t b)
{
    return a.level == b.level && a.col == b.col && a.row == b.row;
}

// Shared between the LVGL task and the loader, under s_lock.
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static tile_key_t s_want[MAX_WANT];
static int s_nwant;
static result_t s_done[MAX_DONE];
static int s_ndone;
static map_manifest_t *s_pending;       // newly loaded chart set, waiting to be swapped in
static bool s_pending_set;              // s_pending (possibly NULL = no chart) is waiting
static uint32_t s_pending_gen;
static bool s_reload;
static volatile map_tiles_state_t s_state = MAP_TILES_NONE;

// LVGL task only.
static entry_t *s_cache;
static map_manifest_t *s_manifest;
static uint32_t s_gen;                  // chart set generation the cache belongs to
static uint32_t s_frame;
static tile_key_t s_frame_want[MAX_WANT];
static int s_frame_nwant;

// Loader task only.
static uint32_t s_loader_gen;
static map_manifest_t s_loader_manifest;
static bool s_loader_have;
static map_pack_t s_pack;
static int s_pack_level = -1;

// ---------------------------------------------------------------------------
// Loader task
// ---------------------------------------------------------------------------

static void publish_manifest(const map_manifest_t *m, map_tiles_state_t state)
{
    map_manifest_t *copy = NULL;
    if (m) {
        copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM);
        if (copy) *copy = *m;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_pending);
    s_pending = copy;
    s_pending_set = true;
    s_pending_gen = s_loader_gen;
    s_nwant = 0;
    for (int i = 0; i < s_ndone; i++) free((void *)s_done[i].dsc.data);
    s_ndone = 0;
    s_state = state;
    xSemaphoreGive(s_lock);
}

static void load_chart_set(void)
{
    map_pack_close(&s_pack);
    s_pack_level = -1;
    s_loader_gen++;
    s_loader_have = false;
    if (!map_store_card_ok()) {
        publish_manifest(NULL, MAP_TILES_NO_CARD);
        return;
    }
    s_state = MAP_TILES_LOADING;
    map_manifest_t m;
    if (!map_store_get_manifest(&m)) {
        publish_manifest(NULL, MAP_TILES_NONE);
        return;
    }
    for (int i = 0; i < m.nlevels; i++) {
        char path[96], err[96];
        snprintf(path, sizeof(path), "%s/%s", MAP_DIR, m.levels[i].file);
        if (!map_pack_check(path, m.levels[i].cols, m.levels[i].rows, MAP_TILE_PX, err, sizeof(err))) {
            ESP_LOGE(TAG, "%s", err);
            publish_manifest(NULL, MAP_TILES_ERROR);
            return;
        }
    }
    s_loader_manifest = m;
    s_loader_have = true;
    ESP_LOGI(TAG, "chart set: %d zoom levels, pan %.1f nm", m.nlevels, m.pan_nm);
    publish_manifest(&m, MAP_TILES_READY);
}

static void decode_tile(tile_key_t k, result_t *r)
{
    memset(r, 0, sizeof(*r));
    r->key = k;
    r->gen = s_loader_gen;
    r->blank = true;
    if (!s_loader_have || k.level < 0 || k.level >= s_loader_manifest.nlevels) return;
    if (s_pack_level != k.level) {
        map_pack_close(&s_pack);
        s_pack_level = -1;
        char path[96], err[96];
        snprintf(path, sizeof(path), "%s/%s", MAP_DIR, s_loader_manifest.levels[k.level].file);
        if (!map_pack_open(&s_pack, path, err, sizeof(err))) {
            ESP_LOGE(TAG, "%s", err);
            return;
        }
        s_pack_level = k.level;
    }
    size_t len;
    uint8_t *png = map_pack_read(&s_pack, k.col, k.row, &len);
    if (!png) return; // empty tile, or unreadable: leave it blank
    r->blank = !map_tile_decode(png, len, &r->dsc);
    free(png);
}

static bool in_done(tile_key_t k)
{
    for (int i = 0; i < s_ndone; i++) {
        if (key_eq(s_done[i].key, k)) return true;
    }
    return false;
}

static void loader_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool reload = s_reload;
            s_reload = false;
            xSemaphoreGive(s_lock);
            if (reload) {
                load_chart_set();
                continue;
            }

            // Take the most urgent wanted tile that isn't already waiting to be installed.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool found = false;
            tile_key_t k;
            while (s_nwant > 0 && !found) {
                k = s_want[0];
                memmove(s_want, s_want + 1, (--s_nwant) * sizeof(s_want[0]));
                found = !in_done(k);
            }
            bool full = s_ndone >= MAX_DONE;
            xSemaphoreGive(s_lock);
            if (!found) break;
            if (full) {
                // The LVGL task drains results every few tens of ms.
                vTaskDelay(pdMS_TO_TICKS(10));
                xSemaphoreTake(s_lock, portMAX_DELAY);
                if (s_nwant < MAX_WANT) {
                    memmove(s_want + 1, s_want, s_nwant * sizeof(s_want[0]));
                    s_want[0] = k;
                    s_nwant++;
                }
                xSemaphoreGive(s_lock);
                continue;
            }

            result_t r;
            decode_tile(k, &r);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (r.gen == s_loader_gen && s_ndone < MAX_DONE) {
                s_done[s_ndone++] = r;
            } else {
                free((void *)r.dsc.data);
            }
            xSemaphoreGive(s_lock);
        }
    }
}

// ---------------------------------------------------------------------------
// LVGL task
// ---------------------------------------------------------------------------

static void evict(entry_t *e)
{
    if (e->valid && !e->blank) {
        lv_image_cache_drop(&e->dsc);
        free((void *)e->dsc.data);
    }
    memset(e, 0, sizeof(*e));
}

static void install(const result_t *r)
{
    entry_t *slot = NULL;
    for (int i = 0; i < CACHE_TILES && !slot; i++) {
        if (!s_cache[i].valid) slot = &s_cache[i];
    }
    // Otherwise the least recently used, but never one the current frame is showing.
    if (!slot) {
        for (int i = 0; i < CACHE_TILES; i++) {
            entry_t *e = &s_cache[i];
            if (e->used != s_frame && (!slot || e->used < slot->used)) slot = e;
        }
    }
    if (!slot) {
        free((void *)r->dsc.data); // cache full of visible tiles
        return;
    }
    evict(slot);
    slot->key = r->key;
    slot->valid = true;
    slot->blank = r->blank;
    slot->used = s_frame;
    slot->dsc = r->dsc;
}

bool map_tiles_poll(void)
{
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending_set) {
        for (int i = 0; i < CACHE_TILES; i++) evict(&s_cache[i]);
        free(s_manifest);
        s_manifest = s_pending;
        s_pending = NULL;
        s_pending_set = false;
        s_gen = s_pending_gen;
        changed = true;
    }
    for (int i = 0; i < s_ndone; i++) {
        const result_t *r = &s_done[i];
        if (r->gen != s_gen) {
            free((void *)r->dsc.data);
            continue;
        }
        install(r);
        changed = true;
    }
    s_ndone = 0;
    xSemaphoreGive(s_lock);
    return changed;
}

static entry_t *lookup(int level, int col, int row)
{
    tile_key_t k = {level, col, row};
    for (int i = 0; i < CACHE_TILES; i++) {
        if (s_cache[i].valid && key_eq(s_cache[i].key, k)) return &s_cache[i];
    }
    return NULL;
}

void map_tiles_frame_begin(void)
{
    s_frame++;
    s_frame_nwant = 0;
}

static void want(int level, int col, int row)
{
    if (s_frame_nwant < MAX_WANT) s_frame_want[s_frame_nwant++] = (tile_key_t){level, col, row};
}

const lv_image_dsc_t *map_tiles_get(int level, int col, int row)
{
    entry_t *e = lookup(level, col, row);
    if (!e) {
        want(level, col, row);
        return NULL;
    }
    e->used = s_frame;
    return e->blank ? NULL : &e->dsc;
}

void map_tiles_prefetch(int level, int col, int row)
{
    entry_t *e = lookup(level, col, row);
    if (e) e->used = s_frame;
    else want(level, col, row);
}

void map_tiles_frame_end(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_want, s_frame_want, s_frame_nwant * sizeof(s_want[0]));
    s_nwant = s_frame_nwant;
    xSemaphoreGive(s_lock);
    if (s_frame_nwant) xTaskNotifyGive(s_task);
}

map_tiles_state_t map_tiles_state(void)
{
    return s_state;
}

const map_manifest_t *map_tiles_manifest(void)
{
    return s_manifest;
}

void map_tiles_reload(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_reload = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void map_tiles_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cache = heap_caps_calloc(CACHE_TILES, sizeof(entry_t), MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(loader_task, "map_tiles", 8192, NULL, 2, &s_task, 1);
    map_tiles_reload();
}
