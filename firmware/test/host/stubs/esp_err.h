#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
static inline const char *esp_err_to_name(esp_err_t e) { return e ? "ESP_FAIL" : "ESP_OK"; }
