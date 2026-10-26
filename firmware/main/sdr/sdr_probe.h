// RTL-SDR receiver: see sdr_probe.c and RTL_SDR_PLAN.md.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ais.h"
#include "ais_demod.h"
#include "map_store.h"

#define SDR_CAPTURE_PATH  MAP_ROOT "/sdr/capture.cu8"
// Sentences heard during the recording, one per line:
// "<seconds since the I/Q file started> <N network | S SDR decoder> <sentence>".
#define SDR_CAPTURE_NMEA_PATH MAP_ROOT "/sdr/capture.nmea"
#define SDR_CAPTURE_MAX_S 600

// SDR_OFF: no SDR support running; SDR_DISABLED: dongle present but turned off
// in the settings.
typedef enum { SDR_OFF, SDR_NONE, SDR_TESTING, SDR_READY, SDR_CAPTURING, SDR_DISABLED } sdr_state_t;

typedef struct {
    sdr_state_t state;
    char tuner[104];
    uint32_t center_hz, rate;
    // The capture running or last run; cap_file_bytes is the finished file on
    // the card (0 if none).
    int cap_seconds;
    uint64_t cap_bytes, cap_total, cap_dropped, cap_file_bytes;
    char cap_error[80];
    uint32_t cap_log_lines;     // sentences logged so far
    // Decoder
    float cpu_pct;              // time in the demodulator, % of one core
    uint64_t dropped_bytes;     // samples the decoder fell too far behind to take
    ais_demod_stats_t demod;
    ais_stats_t parser;         // what the decoded sentences were (message types)
    // Gain
    int16_t gain;               // applied now, tenths of a dB (SDR_GAIN_AUTO: tuner AGC)
    bool gain_best;             // chosen by the "best fixed gain" sweep
    int gain_steps_down;        // steps below that choice, backing off from clipping
    int sweep_step, sweep_steps;  // sweep progress (sweep_step 0: not sweeping)
} sdr_status_t;

// Starts USB host on the USB-A port. An RTL2832U dongle with an R820T tuner plugged
// in is tuned to the AIS channels and decoded continuously into the vessel
// table, and can record I/Q captures; other RTL2832U dongles just get a
// throughput test. Only called with CONFIG_AIS_SDR_PROBE.
void sdr_probe_start(void);

const char *sdr_state_str(sdr_state_t st);
void sdr_get_status(sdr_status_t *out);
// Records `seconds` of I/Q to SDR_CAPTURE_PATH, in the background. On failure
// err says why.
bool sdr_capture_start(int seconds, char *err, size_t errlen);
// Adds a sentence to the recording's NMEA log (does nothing unless recording).
// source: 'N' network feed, 'S' SDR decoder.
void sdr_capture_log(char source, const char *sentence);
