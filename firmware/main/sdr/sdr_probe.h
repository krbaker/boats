// RTL-SDR bring-up: see sdr_probe.c and RTL_SDR_PLAN.md (steps 1-2).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "map_store.h"

#define SDR_CAPTURE_PATH  MAP_ROOT "/sdr/capture.cu8"
#define SDR_CAPTURE_MAX_S 600

typedef enum { SDR_OFF, SDR_NONE, SDR_TESTING, SDR_READY, SDR_CAPTURING } sdr_state_t;

typedef struct {
    sdr_state_t state;
    char tuner[40];
    uint32_t center_hz, rate;
    // The capture running or last run; cap_file_bytes is the finished file on
    // the card (0 if none).
    int cap_seconds;
    uint64_t cap_bytes, cap_total, cap_dropped, cap_file_bytes;
    char cap_error[80];
} sdr_status_t;

// Starts USB host on the USB-A port and tests any RTL2832U dongle plugged in,
// logging the results. An R820T dongle then stays tuned to the AIS channels,
// ready to record captures. Only called with CONFIG_AIS_SDR_PROBE.
void sdr_probe_start(void);

const char *sdr_state_str(sdr_state_t st);
void sdr_get_status(sdr_status_t *out);
// Records `seconds` of I/Q to SDR_CAPTURE_PATH, in the background. On failure
// err says why.
bool sdr_capture_start(int seconds, char *err, size_t errlen);
