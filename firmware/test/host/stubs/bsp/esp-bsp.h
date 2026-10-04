// Host-test stand-in: the "card" is a scratch directory that is always present.
#pragma once
#include "esp_err.h"
static inline esp_err_t bsp_sdcard_mount(void) { return ESP_OK; }
