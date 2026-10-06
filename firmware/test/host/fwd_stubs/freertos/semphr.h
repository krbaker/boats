#pragma once
#include <pthread.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t m = malloc(sizeof(*m));
    pthread_mutex_init(m, NULL);
    return m;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t t) { (void)t; pthread_mutex_lock(m); return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { pthread_mutex_unlock(m); return pdTRUE; }
