// Persistent user settings (NVS-backed).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Which vessels the map labels or draws (set on the Vessels overlay).
typedef enum { FILTER_HIDE_LABEL, FILTER_HIDE_VESSEL } filter_mode_t;
typedef enum { ORDER_SPEED, ORDER_SIZE } draw_order_t;

typedef struct {
    uint16_t min_len_ft;     // 0 = no size filter
    uint8_t min_speed_kn;    // 0 = no speed filter
    uint8_t mode;            // filter_mode_t: what happens to vessels below the limits
    uint8_t order;           // draw_order_t: whose labels win when they overlap
    bool include_unknown;    // vessels that never reported size / speed pass those filters
    bool label_name;
    bool label_speed;
} vessel_filter_t;

// Where the dongle's sentences are retransmitted to (RETRANSMIT_PLAN.md).
#define FWD_DEST_MAX 3
typedef enum { FWD_UDP, FWD_TCP } fwd_proto_t;

typedef struct {
    bool enabled;
    uint8_t proto;           // fwd_proto_t
    uint16_t port;           // 0 = not set
    char host[64];           // IP address or host name
} fwd_dest_t;

typedef struct {
    char ais_host[64];
    uint16_t ais_port;
    char tide_station[16];   // NOAA CO-OPS station id, e.g. "9447130"
    char tz_name[48];        // IANA name shown in the UI, e.g. "America/Los_Angeles"
    char tz_posix[64];       // POSIX TZ string actually applied, e.g. "PST8PDT,M3.2.0,M11.1.0"
    double center_lat;
    double center_lon;
    uint8_t zoom_index;
    uint8_t brightness;      // percent
    vessel_filter_t filter;
    uint32_t map_created;    // chart set whose centre was last applied (see map_store)
    // AIS sources
    bool net_enabled;        // use the network (TCP) feed
    bool sdr_enabled;        // decode a plugged-in RTL-SDR dongle
    int16_t sdr_gain;        // tuner gain in tenths of a dB, or SDR_GAIN_AUTO / SDR_GAIN_BEST
    int16_t sdr_ppm_tenths;  // dongle crystal correction, tenths of a ppm
    bool sdr_bias_tee;       // power an LNA / active antenna through the coax
    fwd_dest_t fwd[FWD_DEST_MAX];
} settings_t;

#define SDR_GAIN_AUTO       (-1)  // the tuner's own AGC
#define SDR_GAIN_BEST       (-2)  // best fixed gain, chosen by measuring (see sdr_probe.c)

// Bits set on settings_events() when the matching group of settings changes.
#define SETTINGS_EV_AIS     BIT0
#define SETTINGS_EV_TIDE    BIT1
#define SETTINGS_EV_TZ      BIT2
#define SETTINGS_EV_CENTER  BIT3
#define SETTINGS_EV_MAP     BIT4  // a new chart set was uploaded
#define SETTINGS_EV_DISPLAY BIT5
#define SETTINGS_EV_SDR     BIT6
#define SETTINGS_EV_FWD     BIT7  // a retransmit destination changed

esp_err_t settings_init(void);

// Thread-safe copy of the current settings.
void settings_get(settings_t *out);

// Replace the settings, persist them and signal what changed.
esp_err_t settings_set(const settings_t *in);

// Raise SETTINGS_EV_MAP (map storage changed outside settings).
void settings_notify_map_changed(void);

// Each consumer gets its own event group so they don't steal each other's bits.
// Returns a group that receives SETTINGS_EV_* bits from now on.
EventGroupHandle_t settings_subscribe(void);

// Wi-Fi station credentials live separately (managed by wifi_mgr).
bool settings_get_wifi(char ssid[33], char pass[65]);
esp_err_t settings_set_wifi(const char *ssid, const char *pass);
