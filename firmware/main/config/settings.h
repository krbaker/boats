// Persistent user settings (NVS-backed).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

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
} settings_t;

// Bits set on settings_events() when the matching group of settings changes.
#define SETTINGS_EV_AIS     BIT0
#define SETTINGS_EV_TIDE    BIT1
#define SETTINGS_EV_TZ      BIT2
#define SETTINGS_EV_CENTER  BIT3
#define SETTINGS_EV_MAP     BIT4  // a new chart set was uploaded
#define SETTINGS_EV_DISPLAY BIT5

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
