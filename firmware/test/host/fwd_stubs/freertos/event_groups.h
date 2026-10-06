#pragma once
#include <pthread.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
typedef struct { pthread_mutex_t m; EventBits_t bits; } *EventGroupHandle_t;
static inline EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t g = calloc(1, sizeof(*g));
    pthread_mutex_init(&g->m, NULL);
    return g;
}
static inline EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b)
{
    pthread_mutex_lock(&g->m);
    EventBits_t r = g->bits |= b;
    pthread_mutex_unlock(&g->m);
    return r;
}
// Returns the bits as they were before clearing, like FreeRTOS.
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b)
{
    pthread_mutex_lock(&g->m);
    EventBits_t r = g->bits;
    g->bits &= ~b;
    pthread_mutex_unlock(&g->m);
    return r;
}
