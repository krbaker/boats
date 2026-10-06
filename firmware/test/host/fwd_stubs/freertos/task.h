#pragma once
#include <pthread.h>
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef struct { TaskFunction_t fn; void *arg; } host_task_t;
static void *host_task_entry(void *p)
{
    host_task_t t = *(host_task_t *)p;
    t.fn(t.arg);
    return NULL;
}
static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, int prio,
                                     TaskHandle_t *out)
{
    (void)name; (void)stack; (void)prio; (void)out;
    static host_task_t t;
    t = (host_task_t){fn, arg};
    pthread_t th;
    pthread_create(&th, NULL, host_task_entry, &t);
    pthread_detach(th);
    return pdTRUE;
}
