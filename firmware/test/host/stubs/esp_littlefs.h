#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct { const char *base_path; const char *partition_label; bool format_if_mount_failed; } esp_vfs_littlefs_conf_t;
static inline esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *c) { (void)c; return ESP_OK; }
static inline esp_err_t esp_littlefs_info(const char *l, size_t *t, size_t *u) { (void)l; *t = *u = 0; return ESP_OK; }
