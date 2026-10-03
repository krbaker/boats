// Wi-Fi station / access-point management.
//
// Station credentials are only committed to NVS after they've been proven to
// work; a failed change falls back to the previous network, then to AP mode.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_MGR_STA_CONNECTING,
    WIFI_MGR_STA_CONNECTED,
    WIFI_MGR_AP,            // access point only (setup mode)
} wifi_mgr_state_t;

typedef enum {
    WIFI_TEST_NONE,
    WIFI_TEST_RUNNING,
    WIFI_TEST_OK,
    WIFI_TEST_FAILED_REVERTED,  // went back to the previous network
    WIFI_TEST_FAILED_AP,        // previous network also failed (or none): AP mode
} wifi_test_result_t;

typedef struct {
    wifi_mgr_state_t state;
    bool ap_active;         // AP interface up (AP mode, or keeping it up during/after a test)
    char sta_ssid[33];
    char sta_ip[16];
    int8_t rssi;
    char ap_ssid[33];
    char ap_pass[16];
    char ap_ip[16];
    wifi_test_result_t test;
    char test_ssid[33];
} wifi_mgr_status_t;

void wifi_mgr_start(void);
bool wifi_mgr_has_ip(void);
void wifi_mgr_get_status(wifi_mgr_status_t *out);

// Try new station credentials asynchronously (see wifi_mgr_status_t.test).
void wifi_mgr_set_credentials(const char *ssid, const char *pass);

// Drop the station connection and become an access point with a fresh random
// password. Saved station credentials are kept.
void wifi_mgr_start_ap(void);

const char *wifi_mgr_state_str(wifi_mgr_state_t s);
