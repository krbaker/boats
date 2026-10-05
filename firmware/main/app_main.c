// AIS display for the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C.

#include "bsp/esp-bsp.h"
#include "esp_log.h"

#include "map_tiles.h"
#include "map_store.h"
#include "nmea_client.h"
#include "settings.h"
#include "tide.h"
#include "ui.h"
#include "vessel_db.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "AIS display starting");
    ESP_ERROR_CHECK(settings_init());
    map_store_init();
    vessel_db_init();

    // Upside down, the adapter rotates the picture as it goes to the panel but
    // LVGL still works upright, so the touch points need the same 180 degree
    // turn: mirrored in both x and y.
#if CONFIG_AIS_DISPLAY_UPSIDE_DOWN
    const bool flip = true;
#else
    const bool flip = false;
#endif
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = flip ? ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy = CONFIG_AIS_TOUCH_SWAP_XY,
            .mirror_x = CONFIG_AIS_TOUCH_MIRROR_X ^ flip,
            .mirror_y = CONFIG_AIS_TOUCH_MIRROR_Y ^ flip,
        },
    };
    disp_cfg.lv_adapter_cfg.task_core_id = 0;
    disp_cfg.lv_adapter_cfg.task_stack_size = 16 * 1024;
    disp_cfg.lv_adapter_cfg.stack_in_psram = false;
    if (!bsp_display_start_with_config(&disp_cfg)) {
        ESP_LOGE(TAG, "display init failed");
    }

    wifi_mgr_start();
    tide_start();
    nmea_client_start();
    map_tiles_init();
    web_server_start();

    ui_start(); // also applies the saved backlight brightness
    ESP_LOGI(TAG, "running");
}
