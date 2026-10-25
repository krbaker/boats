#pragma once
// Preview stub: nothing runs in parallel, so there's nothing to wait for.
#include <stdint.h>
#define pdMS_TO_TICKS(ms) (ms)
static inline void vTaskDelay(uint32_t ticks) { (void)ticks; }
typedef void (*TaskFunction_t)(void *);
static inline int xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, int prio, void *handle)
{
    (void)fn, (void)name, (void)stack, (void)arg, (void)prio, (void)handle;
    return 1;
}
