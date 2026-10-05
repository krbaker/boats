#pragma once
// Preview stub: nothing runs in parallel, so there's nothing to wait for.
#include <stdint.h>
#define pdMS_TO_TICKS(ms) (ms)
static inline void vTaskDelay(uint32_t ticks) { (void)ticks; }
