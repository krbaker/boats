// Host-test stand-in for main/config/settings.h
#pragma once
#include <stdint.h>
typedef struct {
    double center_lat, center_lon;
    uint8_t zoom_index;
} settings_t;
void settings_get(settings_t *out);
int settings_set(const settings_t *in);
void settings_notify_map_changed(void);
