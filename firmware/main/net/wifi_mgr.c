#include "wifi_mgr.h"

#include <string.h>

#include "esp_event.h"
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

#define EV_GOT_IP       BIT0
#define EV_DISCONNECTED BIT1

typedef enum { CMD_SET_CREDS, CMD_START_AP } cmd_type_t;

typedef struct {
    cmd_type_t type;
    char ssid[33];
    char pass[65];
} cmd_t;

static wifi_mgr_status_t s_status;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_ev;
static QueueHandle_t s_cmds;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static volatile bool s_auto_reconnect;

#define LOCKED(stmt) do { xSemaphoreTake(s_lock, portMAX_DELAY); stmt; xSemaphoreGive(s_lock); } while (0)

// ---------------------------------------------------------------------------
// Captive-portal DNS: answer every A query with the AP's address so phones
// pop up the configuration page.
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
        xEventGroupSetBits(s_ev, EV_DISCONNECTED);
        LOCKED({
            s_status.sta_ip[0] = '\0';
            if (s_status.state == WIFI_MGR_STA_CONNECTED) s_status.state = WIFI_MGR_STA_CONNECTING;
        });
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        LOCKED({
            snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
            if (s_status.state != WIFI_MGR_AP) s_status.state = WIFI_MGR_STA_CONNECTED;
        });
        ESP_LOGI(TAG, "got IP %s", s_status.sta_ip);
        xEventGroupClearBits(s_ev, EV_DISCONNECTED);
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

static void make_ap_identity(void)
{
    // The P4 has no Wi-Fi of its own (the C6 co-processor does), so there is no
    // local soft-AP MAC: name the access point after the chip's base MAC.
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789"; // no look-alikes
    char pass[11];
    for (int i = 0; i < 10; i++) pass[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    pass[10] = '\0';
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
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    LOCKED(snprintf(s_status.ap_ip, sizeof(s_status.ap_ip), IPSTR, IP2STR(&ip.ip)));
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

static void enter_ap_mode(void)
{
    ESP_LOGI(TAG, "entering AP mode");
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    make_ap_identity();
    LOCKED({
        s_status.state = WIFI_MGR_AP;
        s_status.sta_ip[0] = '\0';
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
    char old_ssid[33], old_pass[65];
    bool have_old = settings_get_wifi(old_ssid, old_pass);
    bool was_ap = s_status.state == WIFI_MGR_AP;

    LOCKED({
        s_status.test = WIFI_TEST_RUNNING;
        strlcpy(s_status.test_ssid, cmd->ssid, sizeof(s_status.test_ssid));
        if (was_ap) s_status.state = WIFI_MGR_STA_CONNECTING;
    });
    // Keep the AP up while testing so a phone configuring us stays connected.
    if (was_ap) set_mode(WIFI_MODE_APSTA);

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
            } else {
                enter_ap_mode();
            }
            continue;
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
    make_ap_identity();
    if (settings_get_wifi(ssid, pass)) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        sta_connect(ssid, pass);
    } else {
        ESP_LOGI(TAG, "no saved network");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_start());
        enter_ap_mode();
    }

    xTaskCreate(wifi_task, "wifi_mgr", 4096, NULL, 4, NULL);
    xTaskCreate(dns_task, "dns", 3072, NULL, 3, NULL);
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

const char *wifi_mgr_state_str(wifi_mgr_state_t s)
{
    switch (s) {
    case WIFI_MGR_STA_CONNECTING: return "connecting";
    case WIFI_MGR_STA_CONNECTED: return "connected";
    case WIFI_MGR_AP: return "access point";
    }
    return "?";
}
