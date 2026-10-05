#pragma once
// Preview stub: there's no LVGL worker task to pause.
#include "esp_err.h"
#include "lvgl.h"
static inline esp_err_t esp_lv_adapter_refresh_now(lv_display_t *disp) { (void)disp; return ESP_OK; }
static inline esp_err_t esp_lv_adapter_pause(int32_t timeout_ms) { (void)timeout_ms; return ESP_OK; }
static inline esp_err_t esp_lv_adapter_resume(void) { return ESP_OK; }
