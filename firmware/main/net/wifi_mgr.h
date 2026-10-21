// Wi-Fi station / access-point management.
//
// Station credentials are only committed to NVS after they've been proven to
// work; a failed change falls back to the previous network, then to AP mode.
//
// Captive portals: an HTTP probe tells whether the station network reaches the
// internet. If it's held at a sign-in page, the "login hotspot" brings the AP
// up alongside the station and routes (NATs) it out through the station, so a
// phone joined to the AP can sign in on the display's behalf: the portal only
// ever sees the display.

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

typedef enum {
    WIFI_NET_UNKNOWN,       // no probe yet on this connection
    WIFI_NET_ONLINE,
    WIFI_NET_OFFLINE,       // no address, or the probe got no answer
    WIFI_NET_PORTAL,        // the probe was redirected or answered: a sign-in page
} wifi_net_t;

typedef struct {
    wifi_mgr_state_t state;
    bool ap_active;         // AP interface up (AP mode, or keeping it up during/after a test)
    char sta_ssid[33];
    char sta_ip[16];
    char sta_netmask[16];
    char sta_gw[16];
    char sta_mac[18];
    int8_t rssi;
    wifi_net_t net;
    bool portal_assist;     // login hotspot (AP + NAT) on
    uint16_t portal_assist_left_s;
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

// Drop the station connection and become an access point (with its saved
// password). Saved station credentials are kept.
void wifi_mgr_start_ap(void);

// Make and save a new random access-point password. If the access point is up
// it restarts with it, disconnecting anyone joined.
void wifi_mgr_new_ap_password(void);

// Start / stop the login hotspot. It only starts while the station is
// connected, and stops by itself after WIFI_MGR_PORTAL_ASSIST_S.
#define WIFI_MGR_PORTAL_ASSIST_S (10 * 60)
void wifi_mgr_portal_assist(bool on);

const char *wifi_mgr_state_str(wifi_mgr_state_t s);
const char *wifi_mgr_net_str(wifi_net_t n);
