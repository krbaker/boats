#pragma once
#include <stdbool.h>
#include "lvgl.h"
static inline bool bsp_display_lock(int t) { return true; }
static inline void bsp_display_unlock(void) {}
static inline int bsp_display_brightness_set(int p) { return 0; }
lv_indev_t *bsp_display_get_input_dev(void);
static inline int bsp_sdcard_mount(void) { return 0; }
