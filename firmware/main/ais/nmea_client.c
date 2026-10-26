#include "nmea_client.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "rx_stats.h"
#include "sdr_probe.h"
#include "settings.h"
#include "vessel_db.h"
#include "wifi_mgr.h"

static const char *TAG = "nmea";

#define NMEA_LINE_MAX            256
#define STALE_TIMEOUT_US    (120LL * 1000000)   // reconnect if the feed goes quiet
#define CONNECT_TIMEOUT_MS  5000
#define BACKOFF_MAX_MS      30000

static nmea_status_t s_status;
static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static ais_parser_t s_parser;

static void set_state(nmea_state_t st, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = st;
    if (err) strlcpy(s_status.last_error, err, sizeof(s_status.last_error));
    xSemaphoreGive(s_lock);
}

// Wait up to ms, waking early if the AIS settings change. Returns true on change.
static bool wait_or_settings_change(uint32_t ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, SETTINGS_EV_AIS, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
    return bits & SETTINGS_EV_AIS;
}

static int connect_to(const char *host, uint16_t port)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || !res) {
        set_state(NMEA_STATE_DISCONNECTED, "DNS lookup failed");
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        freeaddrinfo(res);
        set_state(NMEA_STATE_DISCONNECTED, "socket() failed");
        return -1;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    int r = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) {
        close(sock);
        set_state(NMEA_STATE_DISCONNECTED, "connect failed");
        return -1;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    struct timeval tv = {.tv_sec = CONNECT_TIMEOUT_MS / 1000};
    if (select(sock + 1, NULL, &wfds, NULL, &tv) <= 0) {
        close(sock);
        set_state(NMEA_STATE_DISCONNECTED, "connect timed out");
        return -1;
    }
    int so_err = 0;
    socklen_t len = sizeof(so_err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &len);
    if (so_err) {
        close(sock);
        set_state(NMEA_STATE_DISCONNECTED, so_err == ECONNREFUSED ? "connection refused" : "connect failed");
        return -1;
    }

    int keepalive = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    return sock;
}

static void handle_line(const char *line)
{
    ais_report_t report;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ais_result_t res = ais_parse_line(&s_parser, line, now_ms, &report);
    if (res != AIS_RESULT_IGNORED) sdr_capture_log('N', line);
    if (res == AIS_RESULT_REPORT || res == AIS_RESULT_UNSUPPORTED) rx_stats_count(RX_NET, rx_sentence_channel(line));
    if (res == AIS_RESULT_REPORT) vessel_db_update(&report, RX_NET);
}

// Read lines until the connection drops or the settings change.
static void read_loop(int sock)
{
    static char line[NMEA_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    char buf[512];
    int64_t last_rx = esp_timer_get_time();
    int64_t last_expire = 0;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {.tv_sec = 1};
        int r = select(sock + 1, &rfds, NULL, NULL, &tv);

        int64_t now = esp_timer_get_time();
        if (now - last_expire > 1000000) {
            vessel_db_expire();
            last_expire = now;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.stats = s_parser.stats;
            xSemaphoreGive(s_lock);
        }
        if (xEventGroupGetBits(s_events) & SETTINGS_EV_AIS) {
            ESP_LOGI(TAG, "AIS server changed, reconnecting");
            return;
        }
        if (r < 0) {
            set_state(NMEA_STATE_DISCONNECTED, "socket error");
            return;
        }
        if (r == 0) {
            if (now - last_rx > STALE_TIMEOUT_US) {
                set_state(NMEA_STATE_DISCONNECTED, "no data for 2 minutes");
                return;
            }
            continue;
        }

        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            set_state(NMEA_STATE_DISCONNECTED, n == 0 ? "server closed connection" : "receive error");
            return;
        }
        last_rx = now;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.last_rx_us = now;
        xSemaphoreGive(s_lock);

        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (len > 0 && !overflow) {
                    line[len] = '\0';
                    handle_line(line);
                }
                len = 0;
                overflow = false;
            } else if (len < NMEA_LINE_MAX - 1) {
                line[len++] = c;
            } else {
                overflow = true; // drop over-long lines entirely
            }
        }
    }
}

static void nmea_task(void *arg)
{
    uint32_t backoff_ms = 1000;
    for (;;) {
        settings_t cfg;
        settings_get(&cfg);
        xEventGroupClearBits(s_events, SETTINGS_EV_AIS);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_status.host, cfg.ais_host, sizeof(s_status.host));
        s_status.port = cfg.ais_port;
        xSemaphoreGive(s_lock);

        if (!cfg.net_enabled) {
            set_state(NMEA_STATE_OFF, "");
            vessel_db_expire();
            wait_or_settings_change(1000);
            continue;
        }
        if (!cfg.ais_host[0] || !cfg.ais_port) {
            set_state(NMEA_STATE_NOT_CONFIGURED, "");
            vessel_db_expire();
            wait_or_settings_change(1000);
            continue;
        }
        if (!wifi_mgr_has_ip()) {
            set_state(NMEA_STATE_WAITING_NETWORK, "");
            vessel_db_expire();
            wait_or_settings_change(1000);
            continue;
        }

        set_state(NMEA_STATE_CONNECTING, "");
        ESP_LOGI(TAG, "connecting to %s:%u", cfg.ais_host, cfg.ais_port);
        int sock = connect_to(cfg.ais_host, cfg.ais_port);
        if (sock >= 0) {
            ESP_LOGI(TAG, "connected");
            set_state(NMEA_STATE_CONNECTED, "");
            backoff_ms = 1000;
            read_loop(sock);
            close(sock);
            if (s_status.state == NMEA_STATE_CONNECTED) set_state(NMEA_STATE_DISCONNECTED, "");
        }
        ESP_LOGW(TAG, "disconnected: %s; retry in %lu ms", s_status.last_error, (unsigned long)backoff_ms);
        if (wait_or_settings_change(backoff_ms)) {
            backoff_ms = 1000;
        } else {
            backoff_ms = backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2;
        }
        vessel_db_expire();
    }
}

void nmea_client_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = settings_subscribe();
    ais_parser_init(&s_parser);
    // The stack in PSRAM: it waits on the network, and internal RAM is short.
    xTaskCreatePinnedToCoreWithCaps(nmea_task, "nmea", 7168, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM);
}

void nmea_client_get_status(nmea_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

const char *nmea_state_str(nmea_state_t s)
{
    switch (s) {
    case NMEA_STATE_OFF: return "off";
    case NMEA_STATE_NOT_CONFIGURED: return "not configured";
    case NMEA_STATE_WAITING_NETWORK: return "waiting for network";
    case NMEA_STATE_CONNECTING: return "connecting";
    case NMEA_STATE_CONNECTED: return "connected";
    case NMEA_STATE_DISCONNECTED: return "disconnected";
    }
    return "?";
}
