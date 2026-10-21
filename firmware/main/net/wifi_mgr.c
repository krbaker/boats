#include "wifi_mgr.h"

#include <string.h>

#include "esp_event.h"
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
#define AP_LINGER_MS        60000   // keep the AP up after a successful change so the page can show the result
#define RECONNECT_DELAY_MS  3000

// Internet probe: the same check Android makes. 204 = online; a redirect or
// any other answer = a captive portal's sign-in page; no answer = offline.
#define PROBE_URL           "http://connectivitycheck.gstatic.com/generate_204"
#define PROBE_TIMEOUT_MS    5000
#define PROBE_ONLINE_MS     60000   // between probes while online or offline
#define PROBE_PORTAL_MS     15000   // ...at a portal (signing in some other way)
#define PROBE_ASSIST_MS     5000    // ...while the login hotspot is on

#define EV_GOT_IP       BIT0
#define EV_DISCONNECTED BIT1
#define EV_AP_UP        BIT2
#define EV_PROBE_NOW    BIT3
#define EV_AP_STARTED   BIT4    // the AP (re)started: its lwIP netif is new, with NAT off

typedef enum { CMD_SET_CREDS, CMD_START_AP, CMD_PORTAL_ASSIST, CMD_NEW_AP_PASS } cmd_type_t;

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
static TickType_t s_assist_deadline;
static uint8_t s_ap_channel = 6;

#define LOCKED(stmt) do { xSemaphoreTake(s_lock, portMAX_DELAY); stmt; xSemaphoreGive(s_lock); } while (0)

// ---------------------------------------------------------------------------
// DNS on the AP. Setup mode: answer every A query with the AP's address so
// phones pop up the configuration page. Login hotspot: relay its clients'
// queries to the station network's DNS server, so name lookups don't depend
// on that server being reachable (or NATed properly) from the AP's subnet.
// ---------------------------------------------------------------------------

#define DNS_RELAY_SLOTS 16

typedef struct {
    bool used;
    uint16_t id, client_id;
    struct sockaddr_in client;
    TickType_t sent;
} relay_slot_t;

static relay_slot_t s_relay[DNS_RELAY_SLOTS];
static uint16_t s_relay_next;

static void dns_answer_captive(int sock, uint8_t *buf, int n, const struct sockaddr_in *from)
{
    uint16_t qdcount = (buf[4] << 8) | buf[5];
    if (qdcount != 1) return; // only plain single queries

    // Find the end of the question (name + type + class).
    int p = 12;
    while (p < n && buf[p]) p += buf[p] + 1;
    p += 5;
    if (p > n) return;
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
    sendto(sock, buf, n, 0, (const struct sockaddr *)from, sizeof(*from));
}

static void dns_relay_query(int up, uint8_t *buf, int n, const struct sockaddr_in *from)
{
    // Only for the hotspot's own clients: not an open resolver for the station side.
    esp_netif_ip_info_t ap;
    esp_netif_get_ip_info(s_ap_netif, &ap);
    if ((from->sin_addr.s_addr ^ ap.ip.addr) & ap.netmask.addr) return;

    esp_netif_dns_info_t dns = {0};
    esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
        ESP_LOGW(TAG, "DNS relay: station network has no DNS server");
        return;
    }

    // A free slot, else the oldest.
    relay_slot_t *s = &s_relay[0];
    for (int i = 0; i < DNS_RELAY_SLOTS; i++) {
        if (!s_relay[i].used) { s = &s_relay[i]; break; }
        if ((int32_t)(s_relay[i].sent - s->sent) < 0) s = &s_relay[i];
    }
    s->used = true;
    s->client = *from;
    s->client_id = (buf[0] << 8) | buf[1];
    s->id = s_relay_next++;
    s->sent = xTaskGetTickCount();
    buf[0] = s->id >> 8;
    buf[1] = s->id & 0xFF;

    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = dns.ip.u_addr.ip4.addr};
    sendto(up, buf, n, 0, (struct sockaddr *)&to, sizeof(to));
}

static void dns_relay_reply(int sock, uint8_t *buf, int n)
{
    uint16_t id = (buf[0] << 8) | buf[1];
    for (int i = 0; i < DNS_RELAY_SLOTS; i++) {
        relay_slot_t *s = &s_relay[i];
        if (!s->used || s->id != id) continue;
        s->used = false;
        buf[0] = s->client_id >> 8;
        buf[1] = s->client_id & 0xFF;
        sendto(sock, buf, n, 0, (struct sockaddr *)&s->client, sizeof(s->client));
        return;
    }
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); // to the upstream server, from an ephemeral port
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || up < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }
    s_relay_next = esp_random();
    static uint8_t buf[1500]; // relayed answers can exceed 512 bytes (EDNS0)
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        FD_SET(up, &rd);
        if (select((sock > up ? sock : up) + 1, &rd, NULL, NULL, NULL) <= 0) continue;

        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        if (FD_ISSET(up, &rd)) {
            int n = recvfrom(up, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
            if (n >= 12 && (buf[2] & 0x80) && s_status.portal_assist) dns_relay_reply(sock, buf, n);
        }
        if (FD_ISSET(sock, &rd)) {
            fromlen = sizeof(from);
            int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fromlen);
            if (n < 12 || (buf[2] & 0x80)) continue; // queries only
            if (s_status.portal_assist) dns_relay_query(up, buf, n, &from);
            else if (s_status.ap_active) dns_answer_captive(sock, buf, n, &from);
        }
    }
}

// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        xEventGroupSetBits(s_ev, EV_DISCONNECTED);
        LOCKED({
            s_status.sta_ip[0] = s_status.sta_netmask[0] = s_status.sta_gw[0] = '\0';
            s_status.net = WIFI_NET_OFFLINE;
            if (s_status.state == WIFI_MGR_STA_CONNECTED) s_status.state = WIFI_MGR_STA_CONNECTING;
        });
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        xEventGroupSetBits(s_ev, EV_AP_UP | EV_AP_STARTED);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) {
        xEventGroupClearBits(s_ev, EV_AP_UP);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        LOCKED({
            snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
            snprintf(s_status.sta_netmask, sizeof(s_status.sta_netmask), IPSTR, IP2STR(&e->ip_info.netmask));
            snprintf(s_status.sta_gw, sizeof(s_status.sta_gw), IPSTR, IP2STR(&e->ip_info.gw));
            s_status.net = WIFI_NET_UNKNOWN;
            if (s_status.state != WIFI_MGR_AP) s_status.state = WIFI_MGR_STA_CONNECTED;
        });
        ESP_LOGI(TAG, "got IP %s", s_status.sta_ip);
        xEventGroupClearBits(s_ev, EV_DISCONNECTED);
        xEventGroupSetBits(s_ev, EV_GOT_IP | EV_PROBE_NOW);
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

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    LOCKED(snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), IPSTR, IP2STR(&ip.ip)));
}

// The channel the strongest access point named ssid is on, or 0 if none is heard.
static uint8_t scan_channel(const char *ssid)
{
    wifi_scan_config_t sc = {.ssid = (uint8_t *)ssid};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;
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

static void sta_connect(const char *ssid, const char *pass)
{
    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    s_auto_reconnect = false;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISCONNECTED);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    LOCKED({
        strlcpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid));
        s_status.sta_ip[0] = '\0';
        if (s_status.state != WIFI_MGR_AP) s_status.state = WIFI_MGR_STA_CONNECTING;
    });
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    esp_wifi_connect();
    s_auto_reconnect = true;
}

static void set_mode(wifi_mode_t mode)
{
    wifi_mode_t cur;
    esp_wifi_get_mode(&cur);
    if (cur != mode) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    }
    bool ap = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    if (ap) apply_ap_config();
    LOCKED(s_status.ap_active = ap);
}

// ---------------------------------------------------------------------------
// Captive portals: the internet probe and the login hotspot
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
        uint32_t ms = s_status.portal_assist ? PROBE_ASSIST_MS
                      : s_status.net == WIFI_NET_PORTAL ? PROBE_PORTAL_MS : PROBE_ONLINE_MS;
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

// Move the AP to another address (its DHCP server has to be stopped for it).
// Its clients always get the AP itself as their DNS server (see dns_task).
static void set_ap_ip(const esp_netif_ip_info_t *ip)
{
    esp_netif_dhcps_stop(s_ap_netif);
    esp_netif_set_ip_info(s_ap_netif, ip);
    esp_netif_dhcps_start(s_ap_netif);
    LOCKED(snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), IPSTR, IP2STR(&ip->ip)));
}

static void portal_assist_stop(bool to_sta)
{
    if (!s_status.portal_assist) return;
    ESP_LOGI(TAG, "login hotspot off");
    esp_netif_napt_disable(s_ap_netif);
    LOCKED({
        s_status.portal_assist = false;
        s_status.portal_assist_left_s = 0;
    });
    if (to_sta) set_mode(WIFI_MODE_STA);
}

static void portal_assist_start(void)
{
    if (s_status.portal_assist) { // again: a fresh ten minutes
        s_assist_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_MGR_PORTAL_ASSIST_S * 1000);
        return;
    }
    if (s_status.state != WIFI_MGR_STA_CONNECTED) {
        ESP_LOGW(TAG, "login hotspot needs a station connection");
        return;
    }
    ESP_LOGI(TAG, "login hotspot on");
    set_mode(WIFI_MODE_APSTA);
    if (!(xEventGroupWaitBits(s_ev, EV_AP_UP, pdFALSE, pdFALSE, pdMS_TO_TICKS(5000)) & EV_AP_UP)) {
        ESP_LOGE(TAG, "AP didn't start");
        set_mode(WIFI_MODE_STA);
        return;
    }

    esp_netif_ip_info_t sta, ap;
    esp_netif_get_ip_info(s_sta_netif, &sta);
    esp_netif_get_ip_info(s_ap_netif, &ap);
    // The AP's subnet can't overlap the network it's routed into.
    bool clash = ((sta.ip.addr ^ ap.ip.addr) & sta.netmask.addr & ap.netmask.addr) == 0;
    if (clash) {
        bool dflt = ap.ip.addr == ESP_IP4TOADDR(192, 168, 4, 1);
        ap.ip.addr = ap.gw.addr = dflt ? ESP_IP4TOADDR(10, 48, 0, 1) : ESP_IP4TOADDR(192, 168, 4, 1);
        ap.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
        ESP_LOGI(TAG, "AP moved to " IPSTR " (station network overlaps)", IP2STR(&ap.ip));
        set_ap_ip(&ap);
    }
    esp_netif_dns_info_t dns = {0};
    esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    ESP_LOGI(TAG, "hotspot " IPSTR " -> station " IPSTR " gw " IPSTR ", DNS relayed to " IPSTR,
             IP2STR(&ap.ip), IP2STR(&sta.ip), IP2STR(&sta.gw), IP2STR(&dns.ip.u_addr.ip4));

    // Forwarded traffic leaves by the default netif; bringing the AP up can
    // leave that pointing at the AP, which bounces everything back to it.
    esp_netif_t *was = esp_netif_get_default_netif();
    if (was != s_sta_netif)
        ESP_LOGW(TAG, "default route was %s, moving it to the station",
                 was ? esp_netif_get_desc(was) : "none");
    esp_netif_set_default_netif(s_sta_netif);

    LOCKED(s_status.portal_assist = true); // set first: portal_assist_stop() checks it
    esp_err_t err = esp_netif_napt_enable(s_ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NAT failed: %s", esp_err_to_name(err));
        portal_assist_stop(true);
        return;
    }
    s_assist_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(WIFI_MGR_PORTAL_ASSIST_S * 1000);
    LOCKED(s_status.portal_assist_left_s = WIFI_MGR_PORTAL_ASSIST_S);
    xEventGroupSetBits(s_ev, EV_PROBE_NOW);
}

static void enter_ap_mode(void)
{
    ESP_LOGI(TAG, "entering AP mode");
    portal_assist_stop(false);
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    make_ap_identity(false);
    LOCKED({
        s_status.state = WIFI_MGR_AP;
        s_status.sta_ip[0] = s_status.sta_netmask[0] = s_status.sta_gw[0] = '\0';
        s_status.net = WIFI_NET_OFFLINE;
    });
    set_mode(WIFI_MODE_AP);
    ESP_LOGI(TAG, "AP \"%s\" password \"%s\"", s_status.ap_ssid, s_status.ap_pass);
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
            esp_wifi_connect();
        }
    }
}

static void test_credentials(const cmd_t *cmd)
{
    portal_assist_stop(true); // its routing and DNS belong to the network we're leaving
    char old_ssid[33], old_pass[65];
    bool have_old = settings_get_wifi(old_ssid, old_pass);
    bool was_ap = s_status.state == WIFI_MGR_AP;

    LOCKED({
        s_status.test = WIFI_TEST_RUNNING;
        strlcpy(s_status.test_ssid, cmd->ssid, sizeof(s_status.test_ssid));
        if (was_ap) s_status.state = WIFI_MGR_STA_CONNECTING;
    });
    // Keep the AP up while testing so a phone configuring us stays connected.
    // Its radio can only be on one channel: put the AP on the network's
    // channel first, or the join's authentication can go unanswered while the
    // radio hops back to serve the AP (disconnect reason 2, AUTH_EXPIRE).
    if (was_ap) {
        set_mode(WIFI_MODE_APSTA);
        uint8_t ch = scan_channel(cmd->ssid);
        if (ch && ch != s_ap_channel) {
            ESP_LOGI(TAG, "AP moving to channel %d, where \"%s\" is", ch, cmd->ssid);
            s_ap_channel = ch;
            apply_ap_config(); // restarts the AP: the phone rejoins on the new channel
        }
    }

    sta_connect(cmd->ssid, cmd->pass);
    if (wait_for_ip(TEST_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "new network OK, saving");
        settings_set_wifi(cmd->ssid, cmd->pass);
        LOCKED(s_status.test = WIFI_TEST_OK);
        if (was_ap) {
            vTaskDelay(pdMS_TO_TICKS(AP_LINGER_MS));
            set_mode(WIFI_MODE_STA);
        }
        return;
    }

    ESP_LOGW(TAG, "could not join \"%s\"", cmd->ssid);
    if (have_old && (strcmp(old_ssid, cmd->ssid) != 0 || strcmp(old_pass, cmd->pass) != 0)) {
        sta_connect(old_ssid, old_pass);
        if (wait_for_ip(TEST_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "reverted to \"%s\"", old_ssid);
            LOCKED(s_status.test = WIFI_TEST_FAILED_REVERTED);
            if (was_ap) set_mode(WIFI_MODE_STA);
            return;
        }
    }
    enter_ap_mode();
    LOCKED(s_status.test = WIFI_TEST_FAILED_AP);
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
                ESP_LOGI(TAG, "AP \"%s\" new password \"%s\"", s_status.ap_ssid, s_status.ap_pass);
            } else if (cmd.type == CMD_PORTAL_ASSIST) {
                if (cmd.on) portal_assist_start();
                else portal_assist_stop(true);
            } else {
                enter_ap_mode();
            }
            continue;
        }
        if (s_status.portal_assist) {
            int32_t left = (int32_t)(s_assist_deadline - xTaskGetTickCount());
            if (left <= 0) portal_assist_stop(true);
            else LOCKED(s_status.portal_assist_left_s = (left + configTICK_RATE_HZ - 1) / configTICK_RATE_HZ);
        }
        // The AP restarts after its config is set (and on a new password), and
        // each start re-adds its netif with NAT off: turn it back on.
        if ((xEventGroupClearBits(s_ev, EV_AP_STARTED) & EV_AP_STARTED) && s_status.portal_assist) {
            esp_err_t err = esp_netif_napt_enable(s_ap_netif);
            if (err == ESP_OK) ESP_LOGI(TAG, "AP started: NAT on");
            else ESP_LOGE(TAG, "AP restarted, NAT failed: %s", esp_err_to_name(err));
        }
        // Keep retrying the station connection while it's down.
        if (s_auto_reconnect && s_status.state == WIFI_MGR_STA_CONNECTING &&
            (xEventGroupGetBits(s_ev) & EV_DISCONNECTED) &&
            xTaskGetTickCount() - last_reconnect > pdMS_TO_TICKS(RECONNECT_DELAY_MS)) {
            xEventGroupClearBits(s_ev, EV_DISCONNECTED);
            last_reconnect = xTaskGetTickCount();
            esp_wifi_connect();
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

    char ssid[33], pass[65];
    make_ap_identity(false);
    bool have = settings_get_wifi(ssid, pass);
    ESP_ERROR_CHECK(esp_wifi_set_mode(have ? WIFI_MODE_STA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_start());
    // The station MAC (the C6's): what a captive portal knows the display by.
    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        LOCKED(snprintf(s_status.sta_mac, sizeof(s_status.sta_mac), MACSTR, MAC2STR(mac)));
    }
    if (have) {
        sta_connect(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no saved network");
        enter_ap_mode();
    }

    xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 4, NULL);
    xTaskCreate(dns_task, "dns", 4096, NULL, 3, NULL);
    xTaskCreate(probe_task, "net_probe", 6144, NULL, 3, NULL);
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

void wifi_mgr_start_ap(void)
{
    cmd_t cmd = {.type = CMD_START_AP};
    xQueueSend(s_cmds, &cmd, 0);
}

void wifi_mgr_new_ap_password(void)
{
    cmd_t cmd = {.type = CMD_NEW_AP_PASS};
    xQueueSend(s_cmds, &cmd, 0);
}

void wifi_mgr_portal_assist(bool on)
{
    cmd_t cmd = {.type = CMD_PORTAL_ASSIST, .on = on};
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
    case WIFI_MGR_AP: return "access point";
    }
    return "?";
}
