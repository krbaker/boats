#include "wifi_mgr.h"

#include <string.h>

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "settings.h"

static const char *TAG = "wifi";

#define TEST_TIMEOUT_MS     20000
#define RECONNECT_DELAY_MS  3000
#define RECONNECT_DUAL_MS   10000   // ...with the access point up: each try scans, which takes the radio off its channel
#define AP_LEASE_MIN        2       // short, so the AP's clients soon pick up a change of DNS server
#define RECONNECT_STUCK_MS  60000   // no address and no join attempt this long: start over

// Internet probe: the same check Android makes. 204 = online; a redirect or
// any other answer = a captive portal's sign-in page; no answer = offline.
#define PROBE_URL           "http://connectivitycheck.gstatic.com/generate_204"
#define PROBE_TIMEOUT_MS    5000
#define PROBE_ONLINE_MS     60000   // between probes while online or offline
#define PROBE_PORTAL_MS     15000   // ...at a portal (to see the sign-in soon)

#define EV_GOT_IP       BIT0
#define EV_DISCONNECTED BIT1
#define EV_PROBE_NOW    BIT2
#define EV_LOST_IP      BIT3
#define EV_STA_CHANGED  BIT4    // the station got or lost its address: the AP may need updating (ap_update)
#define EV_AP_STARTED   BIT5    // the AP (re)started: its lwIP netif is new, with NAT off

typedef enum { CMD_SET_CREDS, CMD_AP, CMD_NEW_AP_PASS } cmd_type_t;

typedef struct {
    cmd_type_t type;
    char ssid[33];
    char pass[65];
    bool on;
} cmd_t;

static wifi_mgr_status_t s_status;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_ev;
static QueueHandle_t s_cmds;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static volatile bool s_auto_reconnect;
static char s_ssid[33], s_pass[65];     // the network the station joins
static bool s_saved;                    // a network is saved
static TickType_t s_last_join;          // the last esp_wifi_connect()
static TickType_t s_ap_deadline;
static uint8_t s_ap_channel = 6;
static uint32_t s_ap_dns;               // the DNS server the AP's DHCP hands out; 0 = the AP itself
static bool s_ap_stale;                 // the AP's config changed while it was off

#define LOCKED(stmt) do { xSemaphoreTake(s_lock, portMAX_DELAY); stmt; xSemaphoreGive(s_lock); } while (0)

// ---------------------------------------------------------------------------
// Captive-portal DNS for the access point: answer every A query with the AP's
// address so phones pop up the configuration page. Only asked while the
// station has no address: then the AP's DHCP names the station network's DNS
// server instead (ap_update).
// ---------------------------------------------------------------------------

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fromlen);
        if (n < 12 || !s_status.ap_active) continue;
        uint16_t qdcount = (buf[4] << 8) | buf[5];
        if ((buf[2] & 0x80) || qdcount != 1) continue; // only plain single queries

        // Find the end of the question (name + type + class).
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5;
        if (p > n) continue;
        uint16_t qtype = (buf[p - 4] << 8) | buf[p - 3];

        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(s_ap_netif, &ip);

        buf[2] = 0x84 | (buf[2] & 0x01); // response, authoritative, keep RD
        buf[3] = 0x80;                   // RA, no error
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0; // ANCOUNT
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        n = p;
        if (qtype == 1) {
            static const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(buf + n, ans, sizeof(ans));
            n += sizeof(ans);
            memcpy(buf + n, &ip.ip.addr, 4);
            n += 4;
        }
        sendto(sock, buf, n, 0, (struct sockaddr *)&from, fromlen);
    }
}

// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        xEventGroupSetBits(s_ev, EV_DISCONNECTED | EV_STA_CHANGED);
        LOCKED({
            s_status.sta_ip[0] = s_status.sta_netmask[0] = s_status.sta_gw[0] = '\0';
            s_status.net = WIFI_NET_OFFLINE;
            if (s_status.state == WIFI_MGR_STA_CONNECTED) s_status.state = WIFI_MGR_STA_CONNECTING;
        });
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // If the access point is up, the driver has moved it to this channel too.
        wifi_event_sta_connected_t *e = data;
        if (s_status.ap_active && e->channel != s_ap_channel) ESP_LOGI(TAG, "AP following to channel %d", e->channel);
        s_ap_channel = e->channel;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(s_ev, EV_AP_STARTED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        // DHCP couldn't renew: still joined, but with no address. wifi_task rejoins.
        ESP_LOGW(TAG, "lost IP");
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        xEventGroupSetBits(s_ev, EV_LOST_IP | EV_STA_CHANGED);
        LOCKED({
            s_status.sta_ip[0] = s_status.sta_netmask[0] = s_status.sta_gw[0] = '\0';
            s_status.net = WIFI_NET_OFFLINE;
            if (s_status.state == WIFI_MGR_STA_CONNECTED) s_status.state = WIFI_MGR_STA_CONNECTING;
        });
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        LOCKED({
            snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
            snprintf(s_status.sta_netmask, sizeof(s_status.sta_netmask), IPSTR, IP2STR(&e->ip_info.netmask));
            snprintf(s_status.sta_gw, sizeof(s_status.sta_gw), IPSTR, IP2STR(&e->ip_info.gw));
            s_status.net = WIFI_NET_UNKNOWN;
            s_status.state = WIFI_MGR_STA_CONNECTED;
        });
        ESP_LOGI(TAG, "got IP %s", s_status.sta_ip);
        xEventGroupClearBits(s_ev, EV_DISCONNECTED);
        xEventGroupSetBits(s_ev, EV_GOT_IP | EV_PROBE_NOW | EV_STA_CHANGED);
    }
}

// The password is made once and saved, so it stays the same until a new one is
// asked for (fresh).
static void make_ap_identity(bool fresh)
{
    // The P4 has no Wi-Fi of its own (the C6 co-processor does), so there is no
    // local soft-AP MAC: name the access point after the chip's base MAC.
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789"; // no look-alikes
    char pass[16];
    if (!fresh) settings_get_ap_pass(pass);
    if (fresh || strlen(pass) < 8) { // WPA2 needs 8 or more
        for (int i = 0; i < 10; i++) pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
        pass[10] = '\0';
        settings_set_ap_pass(pass);
    }
    LOCKED({
        snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "AIS-Display-%02X%02X", mac[4], mac[5]);
        strcpy(s_status.ap_pass, pass);
    });
}

static void apply_ap_config(void)
{
    wifi_config_t ap = {0};
    strcpy((char *)ap.ap.ssid, s_status.ap_ssid);
    ap.ap.ssid_len = strlen(s_status.ap_ssid);
    strcpy((char *)ap.ap.password, s_status.ap_pass);
    ap.ap.channel = s_ap_channel;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    s_ap_stale = false;

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    LOCKED(snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), IPSTR, IP2STR(&ip.ip)));
}

// The channel the strongest access point named ssid is on, or 0 if none is heard.
static uint8_t scan_channel(const char *ssid)
{
    wifi_scan_config_t sc = {.ssid = (uint8_t *)ssid};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_ERR_WIFI_STATE) { // the station is still busy with the last join
        vTaskDelay(pdMS_TO_TICKS(1000));
        err = esp_wifi_scan_start(&sc, true);
    }
    if (err != ESP_OK) return 0;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    uint8_t ch = 0;
    int8_t best = -128;
    wifi_ap_record_t rec;
    while (n--) {
        uint16_t one = 1;
        if (esp_wifi_scan_get_ap_records(&one, &rec) != ESP_OK || one == 0) break;
        if (rec.rssi > best) { best = rec.rssi; ch = rec.primary; }
    }
    esp_wifi_clear_ap_list();
    return ch;
}

// One try at joining s_ssid. With the access point up, it first moves to the
// network's channel: the radio can only be on one, and a join's
// authentication on another goes unanswered while the radio hops back to serve
// the AP (disconnect reason 2, AUTH_EXPIRE). The station then looks only there.
static void sta_join(void)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, s_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, s_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (s_status.ap_active) {
        uint8_t ch = scan_channel(s_ssid), cur = 0;
        wifi_second_chan_t sec;
        esp_wifi_get_channel(&cur, &sec);
        if (ch && ch != cur) {
            ESP_LOGI(TAG, "AP moving to channel %d, where \"%s\" is", ch, s_ssid);
            s_ap_channel = ch;
            apply_ap_config(); // restarts the AP: its clients rejoin on the new channel
        }
        sta.sta.channel = ch;
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) ESP_LOGW(TAG, "station config: %s", esp_err_to_name(err));
    s_last_join = xTaskGetTickCount();
    esp_wifi_connect();
}

static void sta_connect(const char *ssid, const char *pass)
{
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISCONNECTED | EV_LOST_IP);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, pass, sizeof(s_pass));
    LOCKED({
        strlcpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid));
        s_status.sta_ip[0] = '\0';
        s_status.state = WIFI_MGR_STA_CONNECTING;
    });
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    sta_join();
    s_auto_reconnect = true;
}

// The AP's config is loaded at start-up (wifi_mgr_start) and kept by the driver
// across mode changes, so turning the AP on starts it once. Setting the config
// again restarts it, so that's only done when it changed while the AP was off.
static void set_mode(wifi_mode_t mode)
{
    wifi_mode_t cur;
    esp_wifi_get_mode(&cur);
    if (cur != mode) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    }
    bool ap = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    if (ap && s_ap_stale) apply_ap_config();
    LOCKED(s_status.ap_active = ap);
}

// ---------------------------------------------------------------------------
// The internet probe
// ---------------------------------------------------------------------------

static wifi_net_t probe_once(void)
{
    esp_http_client_config_t cfg = {
        .url = PROBE_URL,
        .timeout_ms = PROBE_TIMEOUT_MS,
        .disable_auto_redirect = true, // a redirect is the answer, not something to follow
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return WIFI_NET_UNKNOWN;
    wifi_net_t r = WIFI_NET_OFFLINE;
    if (esp_http_client_perform(c) == ESP_OK) {
        r = esp_http_client_get_status_code(c) == 204 ? WIFI_NET_ONLINE : WIFI_NET_PORTAL;
    }
    esp_http_client_cleanup(c);
    return r;
}

static void probe_task(void *arg)
{
    for (;;) {
        uint32_t ms = s_status.net == WIFI_NET_PORTAL ? PROBE_PORTAL_MS : PROBE_ONLINE_MS;
        xEventGroupWaitBits(s_ev, EV_PROBE_NOW, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
        if (!wifi_mgr_has_ip()) continue;
        wifi_net_t r = probe_once();
        if (r == WIFI_NET_OFFLINE) { // one lost packet shouldn't flip the icon
            vTaskDelay(pdMS_TO_TICKS(3000));
            r = probe_once();
        }
        if (r == WIFI_NET_UNKNOWN || !wifi_mgr_has_ip()) continue;
        if (r != s_status.net) ESP_LOGI(TAG, "internet: %s", wifi_mgr_net_str(r));
        LOCKED(s_status.net = r);
    }
}

// ---------------------------------------------------------------------------
// Dual mode
// ---------------------------------------------------------------------------

// Fit the AP to the station. Its clients' DNS server: the station network's
// while the station has an address, so they reach the internet (or its
// captive portal) through NAT; else the AP itself, for the captive DNS that
// pops up the configuration page. And its subnet can't overlap the station's
// network, or replies to its clients could leave by the station. Its DHCP
// server is stopped for either to change.
static void ap_update(bool force)
{
    if (!s_status.ap_active) return;
    esp_netif_ip_info_t sta, ap;
    esp_netif_get_ip_info(s_sta_netif, &sta);
    esp_netif_get_ip_info(s_ap_netif, &ap);
    bool up = wifi_mgr_has_ip();
    bool clash = up && ((sta.ip.addr ^ ap.ip.addr) & sta.netmask.addr & ap.netmask.addr) == 0;
    esp_netif_dns_info_t dns = {0};
    if (!up || esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK ||
        dns.ip.type != ESP_IPADDR_TYPE_V4) {
        dns.ip.u_addr.ip4.addr = 0;
    }
    if (!force && !clash && dns.ip.u_addr.ip4.addr == s_ap_dns) return;

    esp_netif_dhcps_stop(s_ap_netif);
    if (clash) {
        bool dflt = ap.ip.addr == ESP_IP4TOADDR(192, 168, 4, 1);
        ap.ip.addr = ap.gw.addr = dflt ? ESP_IP4TOADDR(10, 48, 0, 1) : ESP_IP4TOADDR(192, 168, 4, 1);
        ap.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        ESP_LOGI(TAG, "AP moving to " IPSTR " (the station network overlaps)", IP2STR(&ap.ip));
        esp_netif_set_ip_info(s_ap_netif, &ap);
        LOCKED(snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), IPSTR, IP2STR(&ap.ip)));
    }
    s_ap_dns = dns.ip.u_addr.ip4.addr;
    uint8_t offer = s_ap_dns ? OFFER_DNS : 0;
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    if (s_ap_dns) esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    uint32_t lease = AP_LEASE_MIN;
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME, &lease, sizeof(lease));
    esp_netif_dhcps_start(s_ap_netif);
    if (s_ap_dns) ESP_LOGI(TAG, "AP clients: DNS " IPSTR ", out through the station", IP2STR(&dns.ip.u_addr.ip4));
    else ESP_LOGI(TAG, "AP clients: captive DNS (setup page)");
}

// On, or if already on, a fresh WIFI_MGR_AP_S. With no network saved it stays on.
static void ap_on(void)
{
    s_ap_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_MGR_AP_S * 1000);
    LOCKED(s_status.ap_left_s = s_saved ? WIFI_MGR_AP_S : 0);
    if (s_status.ap_active) return;
    ESP_LOGI(TAG, "AP \"%s\" on, password \"%s\"", s_status.ap_ssid, s_status.ap_pass);
    set_mode(WIFI_MODE_APSTA); // NAT and DHCP are set up once it starts (wifi_task)
}

static void ap_off(void)
{
    if (!s_status.ap_active || !s_saved) return; // with no network saved it's the only way in
    ESP_LOGI(TAG, "AP off");
    set_mode(WIFI_MODE_STA);
    LOCKED(s_status.ap_left_s = 0);
}

// Wait for an address, re-trying the join whenever it fails, until timeout.
static bool wait_for_ip(uint32_t ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return false;
        EventBits_t bits = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_DISCONNECTED, pdFALSE, pdFALSE, deadline - now);
        if (bits & EV_GOT_IP) return true;
        if (bits & EV_DISCONNECTED) {
            xEventGroupClearBits(s_ev, EV_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(1000));
            sta_join();
        }
    }
}

static void test_credentials(const cmd_t *cmd)
{
    char old_ssid[33], old_pass[65];
    bool have_old = settings_get_wifi(old_ssid, old_pass);

    LOCKED({
        s_status.test = WIFI_TEST_RUNNING;
        strlcpy(s_status.test_ssid, cmd->ssid, sizeof(s_status.test_ssid));
    });
    sta_connect(cmd->ssid, cmd->pass);
    if (wait_for_ip(TEST_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "new network OK, saving");
        settings_set_wifi(cmd->ssid, cmd->pass);
        if (!s_saved) { // the first: the AP's time starts now
            s_saved = true;
            if (s_status.ap_active) ap_on();
        }
        LOCKED(s_status.test = WIFI_TEST_OK);
        return;
    }

    ESP_LOGW(TAG, "could not join \"%s\"", cmd->ssid);
    if (have_old && (strcmp(old_ssid, cmd->ssid) != 0 || strcmp(old_pass, cmd->pass) != 0)) {
        sta_connect(old_ssid, old_pass);
        if (wait_for_ip(TEST_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "reverted to \"%s\"", old_ssid);
            LOCKED(s_status.test = WIFI_TEST_FAILED_REVERTED);
            return;
        }
    }
    // Nothing works: the AP is the way back in. The station keeps trying the
    // saved network, if there is one.
    if (!have_old) {
        s_auto_reconnect = false;
        esp_wifi_disconnect();
        LOCKED(s_status.state = WIFI_MGR_NO_NETWORK);
    }
    ap_on();
    LOCKED(s_status.test = WIFI_TEST_FAILED);
}

static void wifi_task(void *arg)
{
    TickType_t last_reconnect = 0;
    for (;;) {
        cmd_t cmd;
        if (xQueueReceive(s_cmds, &cmd, pdMS_TO_TICKS(500))) {
            if (cmd.type == CMD_SET_CREDS) {
                test_credentials(&cmd);
            } else if (cmd.type == CMD_NEW_AP_PASS) {
                make_ap_identity(true);
                if (s_status.ap_active) apply_ap_config(); // restarts the AP: its clients drop off
                else s_ap_stale = true;
                ESP_LOGI(TAG, "AP \"%s\" new password \"%s\"", s_status.ap_ssid, s_status.ap_pass);
            } else {
                if (cmd.on) ap_on();
                else ap_off();
            }
            continue;
        }
        if (s_status.ap_active && s_saved) {
            int32_t left = (int32_t)(s_ap_deadline - xTaskGetTickCount());
            if (left <= 0) ap_off();
            else LOCKED(s_status.ap_left_s = (left + configTICK_RATE_HZ - 1) / configTICK_RATE_HZ);
        }
        // Each AP start (on, or a restart for a new channel or password) makes
        // its netif anew, with NAT off: turn it on, and fit the AP's DHCP.
        if ((xEventGroupClearBits(s_ev, EV_AP_STARTED) & EV_AP_STARTED) && s_status.ap_active) {
            esp_err_t err = esp_netif_napt_enable(s_ap_netif);
            if (err == ESP_OK) ESP_LOGI(TAG, "AP up: NAT on");
            else ESP_LOGE(TAG, "NAT failed: %s", esp_err_to_name(err));
            ap_update(true);
        }
        if (xEventGroupClearBits(s_ev, EV_STA_CHANGED) & EV_STA_CHANGED) ap_update(false);
        // Keep retrying the station connection while it's down. A join that
        // fails without a disconnect event, or an address that DHCP couldn't
        // renew, would otherwise leave it down for good: start over.
        TickType_t now = xTaskGetTickCount();
        EventBits_t bits = xEventGroupGetBits(s_ev);
        if (s_auto_reconnect && s_status.state == WIFI_MGR_STA_CONNECTING) {
            if ((bits & EV_LOST_IP) || now - s_last_join > pdMS_TO_TICKS(RECONNECT_STUCK_MS)) {
                ESP_LOGW(TAG, "no address: rejoining");
                xEventGroupClearBits(s_ev, EV_LOST_IP);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500)); // let its disconnect event land first
                xEventGroupClearBits(s_ev, EV_DISCONNECTED);
                sta_join();
                last_reconnect = xTaskGetTickCount();
            } else if ((bits & EV_DISCONNECTED) &&
                       now - last_reconnect > pdMS_TO_TICKS(s_status.ap_active ? RECONNECT_DUAL_MS : RECONNECT_DELAY_MS)) {
                xEventGroupClearBits(s_ev, EV_DISCONNECTED);
                sta_join();
                last_reconnect = xTaskGetTickCount();
            }
        }
        if (s_status.state == WIFI_MGR_STA_CONNECTED) {
            wifi_ap_record_t info;
            if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) LOCKED(s_status.rssi = info.rssi);
        }
    }
}

void wifi_mgr_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_ev = xEventGroupCreate();
    s_cmds = xQueueCreate(4, sizeof(cmd_t));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "ais-display");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_wifi_event, NULL));

    char ssid[33], pass[65];
    make_ap_identity(false);
    s_saved = settings_get_wifi(ssid, pass);
    // Load the AP's config now, before Wi-Fi starts, so turning the AP on later
    // starts it once instead of starting and restarting it.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    apply_ap_config();
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_saved ? WIFI_MODE_STA : WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // The station MAC (the C6's): what a captive portal knows the display by.
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        LOCKED(snprintf(s_status.sta_mac, sizeof(s_status.sta_mac), MACSTR, MAC2STR(mac)));
    }
    if (s_saved) {
        sta_connect(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no saved network");
        LOCKED(s_status.state = WIFI_MGR_NO_NETWORK);
        ap_on();
    }

    // Credentials from the card's config.json: tested like ones from the web page.
    if (settings_take_card_wifi(ssid, pass)) wifi_mgr_set_credentials(ssid, pass);
    memset(pass, 0, sizeof(pass));

    xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 4, NULL); // internal: it saves to flash
    // These two only wait on the network: their stacks in PSRAM, as internal RAM is short.
    xTaskCreateWithCaps(dns_task, "dns", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(probe_task, "net_probe", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
}

bool wifi_mgr_has_ip(void)
{
    return xEventGroupGetBits(s_ev) & EV_GOT_IP;
}

void wifi_mgr_get_status(wifi_mgr_status_t *out)
{
    LOCKED(*out = s_status);
}

void wifi_mgr_set_credentials(const char *ssid, const char *pass)
{
    cmd_t cmd = {.type = CMD_SET_CREDS};
    strlcpy(cmd.ssid, ssid, sizeof(cmd.ssid));
    strlcpy(cmd.pass, pass, sizeof(cmd.pass));
    LOCKED(s_status.test = WIFI_TEST_RUNNING);
    xQueueSend(s_cmds, &cmd, 0);
}

void wifi_mgr_ap(bool on)
{
    cmd_t cmd = {.type = CMD_AP, .on = on};
    xQueueSend(s_cmds, &cmd, 0);
}

void wifi_mgr_new_ap_password(void)
{
    cmd_t cmd = {.type = CMD_NEW_AP_PASS};
    xQueueSend(s_cmds, &cmd, 0);
}

const char *wifi_mgr_net_str(wifi_net_t n)
{
    switch (n) {
    case WIFI_NET_UNKNOWN: return "checking";
    case WIFI_NET_ONLINE: return "online";
    case WIFI_NET_OFFLINE: return "offline";
    case WIFI_NET_PORTAL: return "captive portal";
    }
    return "?";
}

const char *wifi_mgr_state_str(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_STA_CONNECTING: return "connecting";
    case WIFI_MGR_STA_CONNECTED: return "connected";
    case WIFI_MGR_NO_NETWORK: return "no network";
    }
    return "?";
}
