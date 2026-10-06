// Retransmits the dongle's AIS sentences to up to FWD_DEST_MAX destinations,
// over UDP or TCP (RETRANSMIT_PLAN.md).

#pragma once

#include <stdint.h>

#include "settings.h"

typedef enum {
    FWD_STATE_OFF,
    FWD_STATE_NOT_CONFIGURED,
    FWD_STATE_WAITING_NETWORK,
    FWD_STATE_CONNECTING,       // TCP, or looking up the host name
    FWD_STATE_SENDING,
    FWD_STATE_ERROR,            // retrying; error says why
} fwd_state_t;

typedef struct {
    fwd_state_t state;
    uint32_t sent;              // sentences sent
    uint32_t dropped;           // sentences that couldn't be sent (not connected, queue full)
    uint32_t sent_last_min;     // sentences sent in the last full minute
    int64_t connected_us;       // TCP: esp_timer time the connection was made (0 = not connected)
    char error[48];             // the last problem ("" if none since the settings changed)
} fwd_status_t;

void ais_fwd_start(void);

// A sentence from the SDR decoder, without CR LF. Never blocks: it's queued,
// or dropped and counted if the queue is full. Call from one task only.
void ais_fwd_sentence(const char *sentence);

void ais_fwd_get_status(int dest, fwd_status_t *out);
const char *ais_fwd_state_str(fwd_state_t s);
