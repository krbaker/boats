#pragma once
#include <stdint.h>
#include "esp_err.h"
static inline esp_err_t esp_vfs_fat_info(const char *base, uint64_t *total, uint64_t *free_bytes)
{
    (void)base;
    *total = *free_bytes = 1ull << 30;
    return ESP_OK;
}
