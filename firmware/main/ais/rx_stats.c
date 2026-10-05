#include "rx_stats.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define BUCKET_S  10
#define BUCKETS   6

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_total[RX_NSRC][2];
static uint32_t s_bucket[BUCKETS][RX_NSRC][2];
static uint32_t s_bucket_epoch[BUCKETS];  // which 10 s period each bucket holds

static uint32_t epoch_now(void)
{
    return (uint32_t)(esp_timer_get_time() / (BUCKET_S * 1000000LL));
}

void rx_stats_count(rx_source_t src, char channel)
{
    int ch = channel == 'A' || channel == '1' ? 0 : channel == 'B' || channel == '2' ? 1 : -1;
    if (ch < 0 || src >= RX_NSRC) return;
    uint32_t e = epoch_now();
    int b = e % BUCKETS;
    taskENTER_CRITICAL(&s_mux);
    if (s_bucket_epoch[b] != e) {
        memset(s_bucket[b], 0, sizeof(s_bucket[b]));
        s_bucket_epoch[b] = e;
    }
    s_bucket[b][src][ch]++;
    s_total[src][ch]++;
    taskEXIT_CRITICAL(&s_mux);
}

void rx_stats_get(rx_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t e = epoch_now();
    taskENTER_CRITICAL(&s_mux);
    memcpy(out->total, s_total, sizeof(s_total));
    for (int b = 0; b < BUCKETS; b++) {
        if (e - s_bucket_epoch[b] >= BUCKETS) continue;  // older than a minute
        for (int s = 0; s < RX_NSRC; s++) {
            for (int c = 0; c < 2; c++) out->last_min[s][c] += s_bucket[b][s][c];
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

char rx_sentence_channel(const char *line)
{
    const char *p = strstr(line, "VDM,");
    if (!p) p = strstr(line, "VDO,");
    if (!p) return 0;
    for (int commas = 0; *p && commas < 4; p++) {
        if (*p == ',') commas++;
    }
    return *p != ',' ? *p : 0;
}
