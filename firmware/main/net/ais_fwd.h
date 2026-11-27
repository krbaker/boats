// Retransmits the dongle's AIS sentences to up to FWD_DEST_MAX destinations,
// over UDP or TCP (RETRANSMIT_PLAN.md), and serves them to up to
// SRV_CLIENTS_MAX TCP clients (BOARDS_PLAN.md).

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

#define SRV_CLIENTS_MAX 4

typedef struct {
    char addr[16];              // the client's IP address
    int64_t connected_us;       // esp_timer time it connected
    uint32_t sent;
    uint32_t dropped;
} srv_client_status_t;

// The TCP server. Its states are fwd_state_t's: OFF, WAITING_NETWORK,
// SENDING (listening, with or without clients) and ERROR.
typedef struct {
    fwd_state_t state;
    uint16_t port;
    int nclients;
    srv_client_status_t client[SRV_CLIENTS_MAX]; // by slot: a free slot has connected_us 0
    uint32_t sent;              // sentences sent, counted once per client
    uint32_t dropped;           // ...and not sent: a client too slow to take them
    uint32_t sent_last_min;
    uint32_t refused;           // connections turned away: all slots in use
    char error[48];
} srv_status_t;

void ais_fwd_start(void);

// A sentence from the SDR decoder, without CR LF. Never blocks: it's queued,
// or dropped and counted if the queue is full. Call from one task only.
void ais_fwd_sentence(const char *sentence);

void ais_fwd_get_status(int dest, fwd_status_t *out);
const char *ais_fwd_state_str(fwd_state_t s);

void ais_fwd_get_server_status(srv_status_t *out);
const char *ais_fwd_server_state_str(fwd_state_t s); // "listening" for SENDING
