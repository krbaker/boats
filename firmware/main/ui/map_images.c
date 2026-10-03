#include "map_images.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "src/libs/lodepng/lodepng.h"

static const char *TAG = "map_images";

void lv_image_cache_drop(const void *src); // not exported via lvgl.h in 9.6

typedef struct {
    map_manifest_t manifest;
    lv_image_dsc_t levels[MAP_MAX_LEVELS];
} image_set_t;

static image_set_t *s_active;   // used by the UI (LVGL task only)
static image_set_t *s_pending;  // finished by the loader, waiting to be swapped in
static volatile map_images_state_t s_state = MAP_IMAGES_NONE;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

static void free_set(image_set_t *set)
{
    if (!set) return;
    for (int i = 0; i < MAP_MAX_LEVELS; i++) {
        free((void *)set->levels[i].data);
    }
    free(set);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = n > 0 ? heap_caps_malloc(n, MALLOC_CAP_SPIRAM) : NULL;
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = n;
    return buf;
}

// Decode a PNG file into a native RGB565 image descriptor.
static bool decode_level(const char *path, lv_image_dsc_t *dsc)
{
    size_t png_len;
    uint8_t *png = read_file(path, &png_len);
    if (!png) return false;

    unsigned char *rgb = NULL;
    unsigned w, h;
    unsigned err = lodepng_decode24(&rgb, &w, &h, png, png_len);
    free(png);
    if (err) {
        ESP_LOGE(TAG, "%s: %s", path, lodepng_error_text(err));
        return false;
    }

    uint16_t *px = heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM);
    if (!px) {
        lv_free(rgb);
        return false;
    }
    const unsigned char *s = rgb;
    for (unsigned i = 0; i < w * h; i++, s += 3) {
        px[i] = (uint16_t)(((s[0] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[2] >> 3));
    }
    lv_free(rgb);

    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;
    dsc->data_size = w * h * 2;
    dsc->data = (const uint8_t *)px;
    return true;
}

static void loader_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        image_set_t *set = calloc(1, sizeof(image_set_t));
        if (!map_store_get_manifest(&set->manifest)) {
            free(set);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            free_set(s_pending);
            s_pending = NULL;
            s_state = MAP_IMAGES_NONE;
            xSemaphoreGive(s_lock);
            continue;
        }

        s_state = MAP_IMAGES_LOADING;
        bool ok = true;
        int64_t t0 = esp_log_timestamp();
        for (int i = 0; i < set->manifest.nlevels && ok; i++) {
            char path[96];
            snprintf(path, sizeof(path), "%s/%s", MAP_DIR, set->manifest.levels[i].file);
            ok = decode_level(path, &set->levels[i]);
        }
        if (!ok) {
            free_set(set);
            s_state = MAP_IMAGES_ERROR;
            continue;
        }
        ESP_LOGI(TAG, "decoded %d chart levels in %lld ms", set->manifest.nlevels,
                 (long long)(esp_log_timestamp() - t0));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        free_set(s_pending);
        s_pending = set;
        xSemaphoreGive(s_lock);
    }
}

void map_images_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(loader_task, "map_load", 8192, NULL, 2, &s_task, 1);
}

void map_images_reload(void)
{
    xTaskNotifyGive(s_task);
}

bool map_images_poll(void)
{
    // The set replaced on the previous poll is no longer referenced by any widget.
    static image_set_t *retired;
    if (retired) {
        for (int i = 0; i < MAP_MAX_LEVELS; i++) lv_image_cache_drop(&retired->levels[i]);
        free_set(retired);
        retired = NULL;
    }

    image_set_t *old = NULL;
    bool swapped = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending) {
        old = s_active;
        s_active = s_pending;
        s_pending = NULL;
        s_state = MAP_IMAGES_READY;
        swapped = true;
    } else if (s_state == MAP_IMAGES_NONE && s_active) {
        old = s_active; // chart removed
        s_active = NULL;
        swapped = true;
    }
    xSemaphoreGive(s_lock);
    retired = old;
    return swapped;
}

map_images_state_t map_images_state(void)
{
    return s_state;
}

const map_manifest_t *map_images_manifest(void)
{
    return s_active ? &s_active->manifest : NULL;
}

const lv_image_dsc_t *map_images_level(int level)
{
    if (!s_active || level < 0 || level >= s_active->manifest.nlevels) return NULL;
    return &s_active->levels[level];
}
