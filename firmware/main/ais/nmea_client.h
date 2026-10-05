// TCP client for the NMEA 0183 AIS feed.

#pragma once

#include <stdint.h>

#include "ais.h"

typedef enum {
    NMEA_STATE_OFF,             // turned off in the settings
    NMEA_STATE_NOT_CONFIGURED,
    NMEA_STATE_WAITING_NETWORK,
    NMEA_STATE_CONNECTING,
    NMEA_STATE_CONNECTED,
    NMEA_STATE_DISCONNECTED,
} nmea_state_t;

typedef struct {
    nmea_state_t state;
    char host[64];
    uint16_t port;
    ais_stats_t stats;
    int64_t last_rx_us;     // esp_timer time of the last line received (0 = never)
    char last_error[48];
} nmea_status_t;

void nmea_client_start(void);
void nmea_client_get_status(nmea_status_t *out);
const char *nmea_state_str(nmea_state_t s);
