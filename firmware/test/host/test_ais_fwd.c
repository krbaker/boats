// Host test for the retransmitter (main/net/ais_fwd.c): runs the real code,
// with the FreeRTOS stand-ins in fwd_stubs/, against UDP and TCP listeners on
// 127.0.0.1. Takes about 15 s: it waits out the TCP reconnect back-off.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "ais_fwd.h"
#include "wifi_mgr.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// What ais_fwd.c needs from the rest of the firmware
// ---------------------------------------------------------------------------

static settings_t s_cfg;
static pthread_mutex_t s_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static EventGroupHandle_t s_group;
static volatile bool s_has_ip = true;

void settings_get(settings_t *out)
{
    pthread_mutex_lock(&s_cfg_lock);
    *out = s_cfg;
    pthread_mutex_unlock(&s_cfg_lock);
}

EventGroupHandle_t settings_subscribe(void)
{
    s_group = xEventGroupCreate();
    return s_group;
}

bool wifi_mgr_has_ip(void)
{
    return s_has_ip;
}

static void set_dest(int i, bool on, fwd_proto_t proto, const char *host, uint16_t port)
{
    pthread_mutex_lock(&s_cfg_lock);
    s_cfg.fwd[i] = (fwd_dest_t){.enabled = on, .proto = proto, .port = port};
    snprintf(s_cfg.fwd[i].host, sizeof(s_cfg.fwd[i].host), "%s", host);
    pthread_mutex_unlock(&s_cfg_lock);
    if (s_group) xEventGroupSetBits(s_group, SETTINGS_EV_FWD);
}

// ---------------------------------------------------------------------------
// Sockets on the receiving end
// ---------------------------------------------------------------------------

static int listen_on(int type, uint16_t *port)
{
    int s = socket(AF_INET, type, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    bind(s, (struct sockaddr *)&a, sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &len);
    *port = ntohs(a.sin_port);
    if (type == SOCK_STREAM) listen(s, 4);
    return s;
}

static bool readable(int s, int ms)
{
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    struct timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
    return select(s + 1, &r, NULL, NULL, &tv) > 0;
}

// One datagram, or "" if none arrives in ms.
static const char *udp_recv(int s, int ms)
{
    static char buf[512];
    buf[0] = '\0';
    if (!readable(s, ms)) return buf;
    int n = recv(s, buf, sizeof(buf) - 1, 0);
    buf[n > 0 ? n : 0] = '\0';
    return buf;
}

static int tcp_accept(int l, int ms)
{
    return readable(l, ms) ? accept(l, NULL, NULL) : -1;
}

// Reads from a TCP connection until `want` bytes or ms pass.
static const char *tcp_read(int s, size_t want, int ms)
{
    static char buf[1024];
    size_t got = 0;
    while (got < want && readable(s, ms)) {
        int n = recv(s, buf + got, sizeof(buf) - 1 - got, 0);
        if (n <= 0) break;
        got += n;
    }
    buf[got] = '\0';
    return buf;
}

static bool wait_state(int dest, fwd_state_t want, int ms)
{
    for (int t = 0; t < ms; t += 20) {
        fwd_status_t st;
        ais_fwd_get_status(dest, &st);
        if (st.state == want) return true;
        usleep(20000);
    }
    return false;
}

static fwd_status_t status(int dest)
{
    fwd_status_t st;
    ais_fwd_get_status(dest, &st);
    return st;
}

// ---------------------------------------------------------------------------

#define S1  "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4A"
#define T5A "!AIVDM,2,1,1,A,55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp8,0*1C"
#define T5B "!AIVDM,2,2,1,A,88888888880,2*25"

int main(void)
{
    signal(SIGPIPE, SIG_IGN); // Linux raises it on a write to a closed socket; lwIP doesn't
    uint16_t up, tp, up2, closed_port;
    int udp = listen_on(SOCK_DGRAM, &up);
    int tl = listen_on(SOCK_STREAM, &tp);
    int udp2 = listen_on(SOCK_DGRAM, &up2);
    int nobody = listen_on(SOCK_STREAM, &closed_port);
    close(nobody); // a port with nothing listening

    set_dest(0, true, FWD_UDP, "127.0.0.1", up);
    set_dest(1, true, FWD_TCP, "localhost", tp);
    ais_fwd_start();

    // Both come up; the third is off.
    int tc = tcp_accept(tl, 2000);
    CHECK(tc >= 0);
    CHECK(wait_state(0, FWD_STATE_SENDING, 2000));
    CHECK(wait_state(1, FWD_STATE_SENDING, 2000));
    CHECK(status(2).state == FWD_STATE_OFF);
    CHECK(status(1).connected_us > 0);

    // A single sentence and a two-part message: one datagram per sentence over
    // UDP, the lines in order over TCP.
    ais_fwd_sentence(S1);
    ais_fwd_sentence(T5A);
    ais_fwd_sentence(T5B);
    CHECK(strcmp(udp_recv(udp, 1000), S1 "\r\n") == 0);
    CHECK(strcmp(udp_recv(udp, 1000), T5A "\r\n") == 0);
    CHECK(strcmp(udp_recv(udp, 1000), T5B "\r\n") == 0);
    const char *all = S1 "\r\n" T5A "\r\n" T5B "\r\n";
    CHECK(strcmp(tcp_read(tc, strlen(all), 1000), all) == 0);

    // The second part of a message whose first part went missing isn't sent.
    ais_fwd_sentence(T5B);
    ais_fwd_sentence(S1);
    CHECK(strcmp(udp_recv(udp, 1000), S1 "\r\n") == 0);
    CHECK(strcmp(tcp_read(tc, strlen(S1) + 2, 1000), S1 "\r\n") == 0);
    usleep(100000);
    CHECK(status(0).sent == 4 && status(0).dropped == 0);
    CHECK(status(1).sent == 4 && status(1).dropped == 0);

    // The TCP receiver goes away: noticed, UDP carries on, and TCP reconnects
    // after the 5 s back-off.
    close(tc);
    CHECK(wait_state(1, FWD_STATE_ERROR, 2000));
    CHECK(strcmp(status(1).error, "closed by the other end") == 0);
    ais_fwd_sentence(S1);
    CHECK(strcmp(udp_recv(udp, 1000), S1 "\r\n") == 0);
    usleep(100000);
    CHECK(status(1).dropped == 1);
    tc = tcp_accept(tl, 8000);
    CHECK(tc >= 0);
    CHECK(wait_state(1, FWD_STATE_SENDING, 2000));
    CHECK(status(1).error[0] == '\0');
    ais_fwd_sentence(S1);
    CHECK(strcmp(udp_recv(udp, 1000), S1 "\r\n") == 0);
    CHECK(strcmp(tcp_read(tc, strlen(S1) + 2, 1000), S1 "\r\n") == 0);

    // Wi-Fi drops: both wait, sentences are dropped; back again, both resume
    // straight away.
    s_has_ip = false;
    CHECK(wait_state(0, FWD_STATE_WAITING_NETWORK, 2000));
    CHECK(wait_state(1, FWD_STATE_WAITING_NETWORK, 2000));
    uint32_t d0 = status(0).dropped;
    ais_fwd_sentence(S1);
    usleep(400000);
    CHECK(status(0).dropped == d0 + 1);
    s_has_ip = true;
    close(tc);
    tc = tcp_accept(tl, 2000);
    CHECK(tc >= 0);
    CHECK(wait_state(0, FWD_STATE_SENDING, 2000));
    CHECK(wait_state(1, FWD_STATE_SENDING, 2000));
    ais_fwd_sentence(S1);
    CHECK(strcmp(udp_recv(udp, 1000), S1 "\r\n") == 0);
    CHECK(strcmp(tcp_read(tc, strlen(S1) + 2, 1000), S1 "\r\n") == 0);

    // A settings change moves destination 1 to another port, live.
    set_dest(0, true, FWD_UDP, "127.0.0.1", up2);
    usleep(500000);
    CHECK(wait_state(0, FWD_STATE_SENDING, 2000));
    ais_fwd_sentence(S1);
    CHECK(strcmp(udp_recv(udp2, 1000), S1 "\r\n") == 0);
    CHECK(strcmp(udp_recv(udp, 300), "") == 0);
    CHECK(strcmp(tcp_read(tc, strlen(S1) + 2, 1000), S1 "\r\n") == 0);

    // Turned on without an address; then TCP to a port nobody listens on.
    set_dest(2, true, FWD_UDP, "", 0);
    CHECK(wait_state(2, FWD_STATE_NOT_CONFIGURED, 2000));
    set_dest(2, true, FWD_TCP, "127.0.0.1", closed_port);
    CHECK(wait_state(2, FWD_STATE_ERROR, 2000));
    CHECK(strcmp(status(2).error, "connection refused") == 0);
    // ...which doesn't hold up the others.
    ais_fwd_sentence(S1);
    CHECK(strcmp(udp_recv(udp2, 1000), S1 "\r\n") == 0);
    CHECK(strcmp(tcp_read(tc, strlen(S1) + 2, 1000), S1 "\r\n") == 0);

    // Turned off.
    set_dest(1, false, FWD_TCP, "localhost", tp);
    CHECK(wait_state(1, FWD_STATE_OFF, 2000));
    CHECK(strcmp(tcp_read(tc, 1, 1000), "") == 0); // the connection was closed

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all retransmit tests passed\n");
    return 0;
}
