#include "https_lock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;

void https_lock_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

void https_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void https_unlock(void)
{
    xSemaphoreGive(s_lock);
}
