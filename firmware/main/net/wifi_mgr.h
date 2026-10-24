// Wi-Fi: two modes.
//
// Client: the station joins the saved network, and keeps rejoining it.
// Dual: the access point runs alongside the station, so a phone can join it
// and open the configuration page. Its traffic is routed (NATed) out through
// the station, so the phone can also sign in to the station network's captive
// portal on the display's behalf: the portal only ever sees the display. It
// turns off by itself after WIFI_MGR_AP_S, except while no network is saved
// (first start-up).
//
// The radio is on one channel for both, so in dual mode the access point moves
// to the channel of the network the station is joining.
//
// Station credentials are only committed to NVS after they've been proven to
// work; a failed change falls back to the previous network.
//
// An HTTP probe tells whether the station network reaches the internet or is
// held at a captive portal's sign-in page (for the status icon).

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    WIFI_MGR_STA_CONNECTING,
    WIFI_MGR_STA_CONNECTED,
    WIFI_MGR_NO_NETWORK,    // none saved: the station is idle (and the access point on)
} wifi_mgr_state_t;

typedef enum {
    WIFI_TEST_NONE,
    WIFI_TEST_RUNNING,
    WIFI_TEST_OK,
    WIFI_TEST_FAILED_REVERTED,  // went back to the previous network
    WIFI_TEST_FAILED,           // previous network also failed (or none): the access point is on
} wifi_test_result_t;

typedef enum {
    WIFI_NET_UNKNOWN,       // no probe yet on this connection
    WIFI_NET_ONLINE,
    WIFI_NET_OFFLINE,       // no address, or the probe got no answer
    WIFI_NET_PORTAL,        // the probe was redirected or answered: a sign-in page
} wifi_net_t;

typedef struct {
    wifi_mgr_state_t state;
    bool ap_active;         // dual mode: the access point is up
    uint16_t ap_left_s;     // until it turns off by itself; 0 = it stays on (no network saved)
    char sta_ssid[33];
    char sta_ip[16];
    char sta_netmask[16];
    char sta_gw[16];
    char sta_mac[18];
    int8_t rssi;
    wifi_net_t net;
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

// Dual mode on (for WIFI_MGR_AP_S, again from the start if already on) or off.
#define WIFI_MGR_AP_S (10 * 60)
void wifi_mgr_ap(bool on);

// Make and save a new random access-point password. If the access point is up
// it restarts with it, disconnecting anyone joined.
void wifi_mgr_new_ap_password(void);

const char *wifi_mgr_state_str(wifi_mgr_state_t s);
const char *wifi_mgr_net_str(wifi_net_t n);
