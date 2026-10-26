// AIS display for the Waveshare ESP32-P4-WIFI6-Touch-LCD-3.4C.

#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_system.h"

#include "adsb.h"
#include "ais_fwd.h"
#include "fw_update.h"
#include "https_lock.h"
#include "log_buf.h"

#include "map_tiles.h"
#include "map_store.h"
#include "nmea_client.h"
#include "sdr_probe.h"
#include "settings.h"
#include "tide.h"
#include "ui.h"
#include "vessel_db.h"
#include "weather.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "main";

// Why the last run ended, and if it crashed, what the core dump says.
static void log_last_reset(void)
{
    esp_reset_reason_t why = esp_reset_reason();
    if (why == ESP_RST_POWERON || why == ESP_RST_SW) return;
    ESP_LOGW(TAG, "last reset: reason %d", why);
    if (esp_core_dump_image_check() != ESP_OK) return;
    char reason[96] = "";
    esp_core_dump_get_panic_reason(reason, sizeof(reason));
    esp_core_dump_summary_t *s = malloc(sizeof(*s));
    if (s && esp_core_dump_get_summary(s) == ESP_OK) {
        ESP_LOGW(TAG, "core dump: task %s, pc 0x%08lx, ra 0x%08lx, mcause 0x%lx, mtval 0x%08lx: %s", s->exc_task,
                 (unsigned long)s->exc_pc, (unsigned long)s->ex_info.ra, (unsigned long)s->ex_info.mcause,
                 (unsigned long)s->ex_info.mtval, reason);
    } else {
        ESP_LOGW(TAG, "core dump saved: %s", reason);
    }
    free(s);
}

void app_main(void)
{
    log_buf_init();
    ESP_LOGI(TAG, "AIS display starting");
    log_last_reset();
    map_store_mount(); // the settings are on the card
    ESP_ERROR_CHECK(settings_init());
    https_lock_init(); // before the weather and tide tasks fetch
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
    disp_cfg.lv_adapter_cfg.task_stack_size = 10 * 1024; // ~6 KB used at most (web page: Memory)
    disp_cfg.lv_adapter_cfg.stack_in_psram = false;
    if (!bsp_display_start_with_config(&disp_cfg)) {
        ESP_LOGE(TAG, "display init failed");
    }

    wifi_mgr_start();
    tide_start();
    weather_start();
    nmea_client_start();
    adsb_start();
    ais_fwd_start();
    map_tiles_init();
    web_server_start();
#if CONFIG_AIS_SDR_PROBE
    sdr_probe_start();
#endif

    ui_start(); // also applies the saved backlight brightness
    fw_update_confirm_later(); // a trial image (just updated from the web) is kept once it has run a minute
    ESP_LOGI(TAG, "running");
}
