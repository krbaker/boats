#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "map_tiles.h"
#include "src/libs/lodepng/lodepng.h"

static const char *TAG = "map_tiles";

bool map_tile_decode(const uint8_t *png, size_t len, lv_image_dsc_t *dsc)
{
    // LVGL's patched lodepng hands back an lv_draw_buf_t*, not raw pixels; the
    // packed RGB888 data lives in ->data.
    unsigned char *out = NULL;
    unsigned w, h;
    unsigned err = lodepng_decode24(&out, &w, &h, png, len);
    lv_draw_buf_t *rgb = (lv_draw_buf_t *)out;
    if (err) {
        ESP_LOGE(TAG, "tile: %s", lodepng_error_text(err));
        if (rgb) lv_draw_buf_destroy(rgb);
        return false;
    }

    uint16_t *px = heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM);
    if (!px) {
        lv_draw_buf_destroy(rgb);
        return false;
    }
    const unsigned char *s = rgb->data;
    for (unsigned i = 0; i < w * h; i++, s += 3) {
        px[i] = (uint16_t)(((s[0] & 0xF8) << 8) | ((s[1] & 0xFC) << 3) | (s[2] >> 3));
    }
    lv_draw_buf_destroy(rgb);

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
