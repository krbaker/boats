#pragma once
#include <stdint.h>
static inline int esp_vfs_fat_info(const char *base, uint64_t *total, uint64_t *free_bytes)
{
    *total = *free_bytes = 1ull << 30;
    return 0;
}
