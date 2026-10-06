// Retransmit: see ais_fwd.h and RETRANSMIT_PLAN.md.
//
// The decoder hands each sentence to ais_fwd_sentence(), which groups the
// fragments of a multi-part message and queues the whole message (lines with
// CR LF) in a ring buffer. One task owns every socket: it connects, sends
// each queued message to every destination that's ready, and retries the
// others with back-off. Nothing here blocks the decoder, and a destination
// that's down or slow never holds up the others: TCP sockets are
// non-blocking, and a message one can't take is dropped and counted there.

#include "ais_fwd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "wifi_mgr.h"

static const char *TAG = "fwd";

#define QUEUE_BYTES         8192
#define GROUP_MAX           512                     // one message, all fragments, with CR LF
#define PENDING_MAX         1024                    // TCP: accepted but not yet written
#define POLL_MS             200
#define CONNECT_TIMEOUT_US  (10LL * 1000000)
#define STUCK_US            (30LL * 1000000)        // TCP taking nothing for this long: reconnect
#define RESOLVE_EVERY_US    (10LL * 60 * 1000000)   // UDP: look the host name up again
#define BACKOFF_MIN_MS      5000
#define BACKOFF_MAX_MS      60000

typedef struct {
    fwd_dest_t cfg;
    int sock;                   // -1 when closed
    struct sockaddr_in addr;
    int64_t resolved_us;
    int64_t retry_at_us;
    uint32_t backoff_ms;
    int64_t connect_started_us;
    char pending[PENDING_MAX];  // TCP: the unsent end of a message
    int npending;
    int64_t pending_since_us;
    uint32_t minute_count;
    fwd_status_t st;            // copied out under s_lock
} dest_t;

static dest_t s_dest[FWD_DEST_MAX];
static SemaphoreHandle_t s_lock;
static RingbufHandle_t s_queue;
static EventGroupHandle_t s_events;
static volatile bool s_active;  // a destination is on: worth queueing
static uint32_t s_queue_dropped; // lines dropped because the queue was full

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

static void set_state(dest_t *d, fwd_state_t state, const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    d->st.state = state;
    if (err) strlcpy(d->st.error, err, sizeof(d->st.error));
    xSemaphoreGive(s_lock);
}

static void count(dest_t *d, uint32_t sent, uint32_t dropped)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    d->st.sent += sent;
    d->st.dropped += dropped;
    xSemaphoreGive(s_lock);
    d->minute_count += sent;
}

void ais_fwd_get_status(int dest, fwd_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_dest[dest].st;
    xSemaphoreGive(s_lock);
}

const char *ais_fwd_state_str(fwd_state_t s)
{
    switch (s) {
    case FWD_STATE_OFF: return "off";
    case FWD_STATE_NOT_CONFIGURED: return "not configured";
    case FWD_STATE_WAITING_NETWORK: return "waiting for network";
    case FWD_STATE_CONNECTING: return "connecting";
    case FWD_STATE_SENDING: return "sending";
    case FWD_STATE_ERROR: return "error";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Queueing (the decoder's task)
// ---------------------------------------------------------------------------

static int count_lines(const char *p, size_t len)
{
    int n = 0;
    for (size_t i = 0; i < len; i++) n += p[i] == '\n';
    return n;
}

void ais_fwd_sentence(const char *sentence)
{
    static char group[GROUP_MAX];
    static int len, lines;
    if (!s_active || !s_queue) return;

    // "!AIVDM,<count>,<number>,...": collect fragments until the last one.
    int total = 1, num = 1;
    if (sscanf(sentence, "!%*5[A-Z],%d,%d,", &total, &num) != 2 || total < 1 || num < 1 || num > total) total = num = 1;
    if (num == 1) len = lines = 0;
    if (num != lines + 1) { // a fragment went missing: drop the message
        len = lines = 0;
        return;
    }
    int n = snprintf(group + len, sizeof(group) - len, "%s\r\n", sentence);
    if (n < 0 || n >= (int)sizeof(group) - len) {
        len = lines = 0;
        return;
    }
    len += n;
    lines++;
    if (num < total) return;

    if (xRingbufferSend(s_queue, group, len, 0) != pdTRUE) {
        __atomic_add_fetch(&s_queue_dropped, lines, __ATOMIC_RELAXED);
    }
    len = lines = 0;
}

// ---------------------------------------------------------------------------
// Connections (the forwarder task)
// ---------------------------------------------------------------------------

static void close_dest(dest_t *d)
{
    if (d->sock >= 0) close(d->sock);
    d->sock = -1;
    d->npending = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    d->st.connected_us = 0;
    xSemaphoreGive(s_lock);
}

// Close, and try again after the back-off, which doubles each time up to a minute.
static void retry_later(dest_t *d, const char *why)
{
    const char *proto = d->cfg.proto == FWD_TCP ? "TCP" : "UDP";
    ESP_LOGW(TAG, "%s %s:%u: %s; retrying in %lu s", proto, d->cfg.host, d->cfg.port, why,
             (unsigned long)(d->backoff_ms / 1000));
    close_dest(d);
    set_state(d, FWD_STATE_ERROR, why);
    d->retry_at_us = esp_timer_get_time() + (int64_t)d->backoff_ms * 1000;
    d->backoff_ms = d->backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : d->backoff_ms * 2;
}

static bool resolve(dest_t *d)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = d->cfg.proto == FWD_TCP ? SOCK_STREAM : SOCK_DGRAM};
    struct addrinfo *res = NULL;
    char port[8];
    snprintf(port, sizeof(port), "%u", d->cfg.port);
    if (getaddrinfo(d->cfg.host, port, &hints, &res) != 0 || !res) return false;
    memcpy(&d->addr, res->ai_addr, sizeof(d->addr));
    freeaddrinfo(res);
    d->resolved_us = esp_timer_get_time();
    return true;
}

static void open_dest(dest_t *d, int64_t now)
{
    set_state(d, FWD_STATE_CONNECTING, NULL);
    if (!resolve(d)) return retry_later(d, "host name not found");
    bool tcp = d->cfg.proto == FWD_TCP;
    d->sock = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (d->sock < 0) return retry_later(d, "no socket available");
    fcntl(d->sock, F_SETFL, fcntl(d->sock, F_GETFL, 0) | O_NONBLOCK);
    if (!tcp) {
        ESP_LOGI(TAG, "UDP %s:%u: sending", d->cfg.host, d->cfg.port);
        set_state(d, FWD_STATE_SENDING, "");
        d->backoff_ms = BACKOFF_MIN_MS;
        return;
    }
    int keepalive = 1;
    setsockopt(d->sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    if (connect(d->sock, (struct sockaddr *)&d->addr, sizeof(d->addr)) < 0 && errno != EINPROGRESS) {
        return retry_later(d, "connect failed");
    }
    d->connect_started_us = now;
}

// TCP: has the connection been made, refused, or timed out?
static void check_connect(dest_t *d, int64_t now)
{
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(d->sock, &wfds);
    struct timeval tv = {0};
    if (select(d->sock + 1, NULL, &wfds, NULL, &tv) <= 0) {
        if (now - d->connect_started_us > CONNECT_TIMEOUT_US) retry_later(d, "connect timed out");
        return;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(d->sock, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err) return retry_later(d, err == ECONNREFUSED ? "connection refused" : "connect failed");
    ESP_LOGI(TAG, "TCP %s:%u: connected", d->cfg.host, d->cfg.port);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    d->st.state = FWD_STATE_SENDING;
    d->st.error[0] = '\0';
    d->st.connected_us = now;
    xSemaphoreGive(s_lock);
    d->backoff_ms = BACKOFF_MIN_MS;
}

// TCP: write what's left of the last message. False if the connection failed.
static bool flush_pending(dest_t *d, int64_t now)
{
    if (!d->npending) return true;
    int n = send(d->sock, d->pending, d->npending, MSG_DONTWAIT);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            retry_later(d, "connection lost");
            return false;
        }
        if (now - d->pending_since_us > STUCK_US) {
            retry_later(d, "not accepting data");
            return false;
        }
        return true;
    }
    memmove(d->pending, d->pending + n, d->npending - n);
    d->npending -= n;
    d->pending_since_us = now;
    return true;
}

// TCP: notice the far end closing even when there's nothing to send.
static bool check_closed(dest_t *d)
{
    char buf[64];
    int n = recv(d->sock, buf, sizeof(buf), MSG_DONTWAIT); // anything sent to us is ignored
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        retry_later(d, "closed by the other end");
        return false;
    }
    return true;
}

// On, with an address and port to send to.
static bool usable(const dest_t *d)
{
    return d->cfg.enabled && d->cfg.host[0] && d->cfg.port;
}

// Bring a destination towards "sending", as far as it can go without waiting.
static void service(dest_t *d, bool has_ip, int64_t now)
{
    if (!usable(d)) return;
    if (!has_ip) {
        if (d->sock >= 0) close_dest(d);
        set_state(d, FWD_STATE_WAITING_NETWORK, NULL);
        d->retry_at_us = 0;
        return;
    }
    if (d->sock < 0) {
        if (now >= d->retry_at_us) open_dest(d, now);
        return;
    }
    if (d->cfg.proto == FWD_UDP) {
        if (now - d->resolved_us > RESOLVE_EVERY_US) resolve(d); // keeps the old address if it fails
        return;
    }
    if (d->st.state == FWD_STATE_CONNECTING) return check_connect(d, now);
    if (flush_pending(d, now)) check_closed(d);
}

static void deliver(dest_t *d, const char *msg, size_t len, int lines, int64_t now)
{
    if (!usable(d)) return;
    if (d->st.state != FWD_STATE_SENDING) return count(d, 0, lines);

    if (d->cfg.proto == FWD_UDP) {
        // One datagram per sentence.
        int sent = 0;
        for (const char *p = msg, *end = msg + len; p < end;) {
            const char *nl = memchr(p, '\n', end - p);
            size_t n = nl ? (size_t)(nl - p + 1) : (size_t)(end - p);
            if (sendto(d->sock, p, n, 0, (struct sockaddr *)&d->addr, sizeof(d->addr)) == (int)n) sent++;
            p += n;
        }
        if (sent < lines) set_state(d, FWD_STATE_SENDING, "some datagrams couldn't be sent");
        return count(d, sent, lines - sent);
    }

    if (!flush_pending(d, now)) return count(d, 0, lines);
    if (d->npending) return count(d, 0, lines); // still busy with the last one
    int n = send(d->sock, msg, len, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return count(d, 0, lines);
        retry_later(d, "connection lost");
        return count(d, 0, lines);
    }
    if ((size_t)n < len) { // the rest goes out on the next pass, before anything else
        memcpy(d->pending, msg + n, len - n);
        d->npending = len - n;
        d->pending_since_us = now;
    }
    count(d, lines, 0);
}

// Picks up changed settings. A destination whose settings changed starts over.
static void load_settings(void)
{
    settings_t cfg;
    settings_get(&cfg);
    bool active = false;
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        dest_t *d = &s_dest[i];
        const fwd_dest_t *c = &cfg.fwd[i];
        active |= c->enabled && c->host[0] && c->port;
        if (d->cfg.enabled == c->enabled && d->cfg.proto == c->proto && d->cfg.port == c->port &&
            strcmp(d->cfg.host, c->host) == 0) {
            continue;
        }
        close_dest(d);
        d->cfg = *c;
        d->retry_at_us = 0;
        d->backoff_ms = BACKOFF_MIN_MS;
        d->resolved_us = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        d->st = (fwd_status_t){.state = !c->enabled ? FWD_STATE_OFF
                                        : (!c->host[0] || !c->port) ? FWD_STATE_NOT_CONFIGURED
                                                                    : FWD_STATE_CONNECTING};
        xSemaphoreGive(s_lock);
    }
    s_active = active;
}

static void fwd_task(void *arg)
{
    int64_t minute_start = esp_timer_get_time();
    load_settings();
    for (;;) {
        if (xEventGroupClearBits(s_events, SETTINGS_EV_FWD) & SETTINGS_EV_FWD) load_settings();
        int64_t now = esp_timer_get_time();
        bool has_ip = wifi_mgr_has_ip();
        for (int i = 0; i < FWD_DEST_MAX; i++) service(&s_dest[i], has_ip, now);

        uint32_t qdrop = __atomic_exchange_n(&s_queue_dropped, 0, __ATOMIC_RELAXED);
        if (qdrop) {
            for (int i = 0; i < FWD_DEST_MAX; i++) {
                if (usable(&s_dest[i])) count(&s_dest[i], 0, qdrop);
            }
        }

        size_t len;
        char *msg = xRingbufferReceive(s_queue, &len, pdMS_TO_TICKS(POLL_MS));
        if (msg) {
            int lines = count_lines(msg, len);
            now = esp_timer_get_time();
            for (int i = 0; i < FWD_DEST_MAX; i++) deliver(&s_dest[i], msg, len, lines, now);
            vRingbufferReturnItem(s_queue, msg);
        }

        if (now - minute_start >= 60LL * 1000000) {
            minute_start = now;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < FWD_DEST_MAX; i++) {
                s_dest[i].st.sent_last_min = s_dest[i].minute_count;
                s_dest[i].minute_count = 0;
            }
            xSemaphoreGive(s_lock);
        }
    }
}

void ais_fwd_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_events = settings_subscribe();
    for (int i = 0; i < FWD_DEST_MAX; i++) s_dest[i].sock = -1;
    s_queue = xRingbufferCreate(QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_queue) {
        ESP_LOGE(TAG, "no memory for the retransmit queue");
        return;
    }
    xTaskCreate(fwd_task, "ais_fwd", 5120, NULL, 3, NULL);
}
