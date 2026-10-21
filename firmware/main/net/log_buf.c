#include "log_buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_BUF_SIZE (64 * 1024) // a power of two, so s_total can wrap

static char *s_buf;
static size_t s_total; // bytes ever written; s_buf holds the last LOG_BUF_SIZE of them
static SemaphoreHandle_t s_lock;
static vprintf_like_t s_console;

static int log_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int ret = s_console(fmt, args);
    // Only from a running task: interrupts and early boot just get the console.
    if (!xPortInIsrContext() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(50))) {
        static char line[256];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n >= (int)sizeof(line)) { // cut short: still end the line
            n = sizeof(line) - 1;
            line[n - 1] = '\n';
        }
        for (int i = 0; i < n; i++) s_buf[s_total++ % LOG_BUF_SIZE] = line[i];
        xSemaphoreGive(s_lock);
    }
    va_end(copy);
    return ret;
}

void log_buf_init(void)
{
    s_buf = heap_caps_malloc(LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (!s_buf || !s_lock) return;
    s_console = esp_log_set_vprintf(log_vprintf);
}

char *log_buf_copy(size_t *len)
{
    char *out = heap_caps_malloc(LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;
    *len = 0;
    if (!s_console) return out; // never started: an empty log

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = MIN(s_total, LOG_BUF_SIZE);
    size_t start = (s_total - n) % LOG_BUF_SIZE;
    size_t first = MIN(n, LOG_BUF_SIZE - start);
    memcpy(out, s_buf + start, first);
    memcpy(out + first, s_buf, n - first);
    bool wrapped = s_total > LOG_BUF_SIZE;
    xSemaphoreGive(s_lock);

    // Once it has wrapped, the oldest line is missing its start.
    size_t skip = 0;
    if (wrapped) {
        char *nl = memchr(out, '\n', n);
        if (nl) skip = nl + 1 - out;
    }
    memmove(out, out + skip, n - skip);
    *len = n - skip;
    return out;
}
