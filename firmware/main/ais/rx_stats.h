// Packet counts per source (network feed, local RTL-SDR) and channel, to
// compare the two receivers.

#pragma once

#include <stdint.h>

typedef enum { RX_NET, RX_SDR, RX_NSRC } rx_source_t;

typedef struct {
    uint32_t total[RX_NSRC][2];     // since boot, channel A and B
    uint32_t last_min[RX_NSRC][2];  // in the last minute (to the nearest 10 s)
} rx_stats_t;

// Count one complete AIS message heard on channel ('A'/'1' or 'B'/'2'; others
// aren't counted).
void rx_stats_count(rx_source_t src, char channel);
void rx_stats_get(rx_stats_t *out);

// The channel letter of an !xxVDM/!xxVDO sentence (its fifth field), or 0.
char rx_sentence_channel(const char *line);
