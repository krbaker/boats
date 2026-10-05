// Scores the AIS demodulator against a reference receiver, using a recording
// from the display's web page: the I/Q capture (capture.cu8) and the sentences
// heard meanwhile (capture.nmea: "<seconds> <N|S> <sentence>", N = network
// feed, S = the display's live SDR decoder).
//
//     build/ais_capture_score capture.cu8 capture.nmea [-v]
//
// The I/Q is decoded again here (offline, with whatever demodulator this was
// built with), and each packet the network feed (the reference) heard is looked
// for among the SDR's, by identical payload. For each miss it reports how
// strong a burst there was on that channel when the packet should have
// arrived: no burst means the antenna didn't hear it; a clear burst means the
// decoder could have done better. Packets only the SDR decoded are listed with
// whether the reference knows the vessel (if not, possibly a false decode).

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ais.h"
#include "ais_demod.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS        AIS_DEMOD_RATE
#define MAX_PKTS  20000
#define WIN       576                  // power windows: 2 ms
#define MATCH_LO  (-3.0)               // network time - SDR time, seconds
#define MATCH_HI  10.0

// ---------------------------------------------------------------------------
// Packets: complete messages (fragments joined), from any of three sources
// ---------------------------------------------------------------------------

enum { SRC_NET, SRC_LIVE, SRC_OFFLINE, NSRC };

typedef struct {
    double t;
    char ch;                // 'A' or 'B' (or '?')
    char payload[600];
    uint32_t mmsi;
    int type;
    int match;              // index of the matching packet in the other list, or -1
} pkt_t;

typedef struct {
    pkt_t *p;
    int n;
    // Fragment assembly (one message at a time per channel is plenty here).
    char part[2][600];
    int part_count[2], part_got[2];
} pktlist_t;

static pktlist_t s_lists[NSRC];

static int bits6(char c)
{
    int v = c - 48;
    return v > 40 ? v - 8 : v;
}

static uint32_t payload_uint(const char *p, int start, int len)
{
    uint32_t v = 0;
    for (int i = start; i < start + len; i++) {
        int c = i / 6;
        if (!p[c]) return 0;
        v = (v << 1) | ((bits6(p[c]) >> (5 - i % 6)) & 1);
    }
    return v;
}

static char norm_channel(char c)
{
    return c == '1' ? 'A' : c == '2' ? 'B' : c == 'A' || c == 'B' ? c : '?';
}

// Feeds one sentence; adds a packet when a message is complete.
static void add_sentence(pktlist_t *l, double t, const char *line)
{
    const char *s = strstr(line, "VDM,");
    if (!s) return;  // VDO (own ship) and anything else: not scored
    char f[7][600];
    int nf = 0, k = 0;
    for (const char *c = s + 4; *c && *c != '*' && nf < 7; c++) {
        if (*c == ',') {
            f[nf][k] = 0;
            nf++;
            k = 0;
        } else if (k < 599) {
            f[nf][k++] = *c;
        }
    }
    if (nf < 7) f[nf][k] = 0, nf++;
    if (nf < 5) return;  // count, number, sequence id, channel, payload
    int count = atoi(f[0]), num = atoi(f[1]);
    char ch = norm_channel(f[3][0]);
    int ci = ch == 'B';
    if (count < 1 || num < 1 || num > count) return;
    if (num == 1) {
        l->part[ci][0] = 0;
        l->part_got[ci] = 0;
        l->part_count[ci] = count;
    }
    if (l->part_count[ci] != count || l->part_got[ci] != num - 1) return;  // out of order: drop
    size_t have = strlen(l->part[ci]), add = strlen(f[4]);
    if (have + add >= sizeof(l->part[ci])) return;
    memcpy(l->part[ci] + have, f[4], add + 1);
    l->part_got[ci] = num;
    if (num != count || l->n >= MAX_PKTS) return;
    pkt_t *p = &l->p[l->n++];
    p->t = t;
    p->ch = ch;
    snprintf(p->payload, sizeof(p->payload), "%s", l->part[ci]);
    p->type = (int)payload_uint(p->payload, 0, 6);
    p->mmsi = payload_uint(p->payload, 8, 30);
    p->match = -1;
}

// ---------------------------------------------------------------------------
// Offline decode, plus channel power in 2 ms windows for judging misses
// ---------------------------------------------------------------------------

static long s_pos;  // complex samples fed so far

static void on_offline(void *ctx, const char *s)
{
    (void)ctx;
    add_sentence(&s_lists[SRC_OFFLINE], (double)s_pos / FS, s);
}

static float *s_pw[2];
static long s_nwin;

static void decode_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    long nwin_max = bytes / 2 / WIN + 1;
    s_pw[0] = calloc(nwin_max, sizeof(float));
    s_pw[1] = calloc(nwin_max, sizeof(float));

    ais_demod_t *d = ais_demod_new(on_offline, NULL);
    static uint8_t buf[4096];
    // Channel power: mix +-25 kHz down, 12 kHz boxcar, average over the window.
    // Rotating phasors (renormalised every window) instead of cos/sin per sample.
    double ri[2] = {1, 1}, rq[2] = {0, 0}, acc_i[2] = {0}, acc_q[2] = {0}, wsum[2] = {0};
    double si_step[2], co_step[2];
    for (int c = 0; c < 2; c++) {
        double w = (c ? -1 : 1) * 2 * M_PI * 25000.0 / FS;
        co_step[c] = cos(w);
        si_step[c] = sin(w);
    }
    int box = 0, wn = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ais_demod_feed(d, buf, n);
        for (size_t i = 0; i + 1 < n; i += 2) {
            double x = (buf[i] - 127.5) / 127.5, y = (buf[i + 1] - 127.5) / 127.5;
            for (int c = 0; c < 2; c++) {
                acc_i[c] += x * ri[c] - y * rq[c];
                acc_q[c] += x * rq[c] + y * ri[c];
                double ni = ri[c] * co_step[c] - rq[c] * si_step[c];
                rq[c] = ri[c] * si_step[c] + rq[c] * co_step[c];
                ri[c] = ni;
            }
            if (++box == 24) {
                for (int c = 0; c < 2; c++) {
                    wsum[c] += (acc_i[c] * acc_i[c] + acc_q[c] * acc_q[c]) / (24.0 * 24.0);
                    acc_i[c] = acc_q[c] = 0;
                }
                box = 0;
                if (++wn == WIN / 24) {
                    for (int c = 0; c < 2; c++) {
                        s_pw[c][s_nwin] = (float)(wsum[c] / wn);
                        wsum[c] = 0;
                        double m = 1 / sqrt(ri[c] * ri[c] + rq[c] * rq[c]);
                        ri[c] *= m;
                        rq[c] *= m;
                    }
                    s_nwin++;
                    wn = 0;
                }
            }
            s_pos++;
        }
    }
    fclose(f);
    ais_demod_stats_t st;
    ais_demod_get_stats(d, &st);
    printf("I/Q: %.1f s; offline decoder: %u packets (A %u, B %u), tuning correction %+.2f ppm\n", (double)s_pos / FS,
           (unsigned)(st.packets[0] + st.packets[1]), (unsigned)st.packets[0], (unsigned)st.packets[1], st.ppm);
    ais_demod_free(d);
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float s_noise[2];

static void noise_floors(void)
{
    float *tmp = malloc(s_nwin * sizeof(float));
    for (int c = 0; c < 2; c++) {
        memcpy(tmp, s_pw[c], s_nwin * sizeof(float));
        qsort(tmp, s_nwin, sizeof(float), cmp_float);
        s_noise[c] = tmp[s_nwin / 2];
    }
    free(tmp);
}

// Strongest 30 ms stretch (a packet is ~27 ms) on the channel within +-span
// of t, in dB above the channel's noise floor.
static float burst_db(char ch, double t, double span)
{
    int c = ch == 'B';
    long lo = (long)((t - span) * FS / WIN), hi = (long)((t + span) * FS / WIN);
    if (lo < 0) lo = 0;
    if (hi > s_nwin - 15) hi = s_nwin - 15;
    float best = 0;
    for (long w = lo; w <= hi; w++) {
        float sum = 0;
        for (int k = 0; k < 15; k++) sum += s_pw[c][w + k];
        if (sum / 15 > best) best = sum / 15;
    }
    return best > 0 ? 10 * log10f(best / s_noise[c]) : -99;
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

// Matches reference packets to SDR packets: same payload, network time minus
// SDR time in [lo, hi]. Returns the number matched; fills .match both ways.
static int match(pktlist_t *ref, pktlist_t *sdr, double lo, double hi)
{
    int m = 0;
    for (int i = 0; i < sdr->n; i++) sdr->p[i].match = -1;
    for (int i = 0; i < ref->n; i++) {
        ref->p[i].match = -1;
        for (int j = 0; j < sdr->n; j++) {
            double dt = ref->p[i].t - sdr->p[j].t;
            if (sdr->p[j].match < 0 && dt >= lo && dt <= hi && !strcmp(ref->p[i].payload, sdr->p[j].payload)) {
                ref->p[i].match = j;
                sdr->p[j].match = i;
                m++;
                break;
            }
        }
    }
    return m;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static bool ref_knows(uint32_t mmsi)
{
    for (int i = 0; i < s_lists[SRC_NET].n; i++) {
        if (s_lists[SRC_NET].p[i].mmsi == mmsi) return true;
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s capture.cu8 capture.nmea [-v]\n", argv[0]);
        return 2;
    }
    bool verbose = argc > 3 && !strcmp(argv[3], "-v");
    for (int s = 0; s < NSRC; s++) s_lists[s].p = calloc(MAX_PKTS, sizeof(pkt_t));

    FILE *f = fopen(argv[2], "r");
    if (!f) {
        perror(argv[2]);
        return 2;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        double t;
        char src;
        int off = 0;
        if (sscanf(line, "%lf %c %n", &t, &src, &off) < 2 || !off) continue;
        line[strcspn(line, "\r\n")] = 0;
        add_sentence(&s_lists[src == 'S' ? SRC_LIVE : SRC_NET], t, line + off);
    }
    fclose(f);

    decode_file(argv[1]);
    noise_floors();

    pktlist_t *ref = &s_lists[SRC_NET], *off = &s_lists[SRC_OFFLINE], *live = &s_lists[SRC_LIVE];
    double dur = (double)s_pos / FS;
    int ref_in = 0;
    for (int i = 0; i < ref->n; i++) ref_in += ref->p[i].t <= dur + MATCH_HI;

    // The network feed's delay, from the matches.
    match(ref, off, MATCH_LO, MATCH_HI);
    double lat[MAX_PKTS];
    int nl = 0;
    for (int i = 0; i < ref->n; i++) {
        if (ref->p[i].match >= 0) lat[nl++] = ref->p[i].t - off->p[ref->p[i].match].t;
    }
    double delay = 0, spread = 0;
    if (nl) {
        qsort(lat, nl, sizeof(double), cmp_double);
        delay = lat[nl / 2];
        spread = (lat[nl * 9 / 10] - lat[nl / 10]) / 2;
    }
    // Where to look for a missed packet's burst: the delay's 10-90% spread.
    double span = spread + 0.1 > 0.25 ? spread + 0.1 : 0.25;

    printf("reference (network): %d packets; SDR live: %d\n", ref->n, live->n);
    printf("network feed delay %.2f s (median of %d matches, 10-90%% within +-%.2f s)\n\n", delay, nl, spread);
    int m_live = match(ref, live, MATCH_LO, MATCH_HI);
    int m_off = match(ref, off, MATCH_LO, MATCH_HI);  // leaves .match for the offline decode
    int chan_agree = 0;
    for (int i = 0; i < ref->n; i++) chan_agree += ref->p[i].match >= 0 && ref->p[i].ch == off->p[ref->p[i].match].ch;
    printf("reference packets the SDR also decoded:  live %d/%d (%.0f%%),  offline %d/%d (%.0f%%)\n", m_live, ref_in,
           100.0 * m_live / (ref_in ? ref_in : 1), m_off, ref_in, 100.0 * m_off / (ref_in ? ref_in : 1));
    printf("channel letters agree on %d of %d matches\n\n", chan_agree, m_off);

    // Misses by how strong a burst there was when the packet should have come.
    static const float edges[] = {3, 6, 9, 12, 15};
    int bins[6] = {0}, nmiss = 0;
    if (verbose) printf("missed reference packets (offline decode):\n");
    for (int i = 0; i < ref->n; i++) {
        pkt_t *p = &ref->p[i];
        if (p->match >= 0 || p->t > dur + MATCH_HI) continue;
        float b = burst_db(p->ch, p->t - delay, span);
        int k = 0;
        while (k < 5 && b >= edges[k]) k++;
        bins[k]++;
        nmiss++;
        if (verbose) {
            printf("  %7.2f s  ch %c  type %2d  MMSI %09u  burst %+5.1f dB\n", p->t - delay, p->ch, p->type,
                   (unsigned)p->mmsi, b);
        }
    }
    printf("misses (%d) by the strongest burst on the channel within +-%.2f s of the expected time:\n", nmiss, span);
    printf("   < 3 dB (nothing heard)  %d\n", bins[0]);
    for (int k = 1; k < 6; k++) {
        if (k < 5) printf("   %2.0f-%2.0f dB               %d\n", edges[k - 1], edges[k], bins[k]);
        else printf("   >= %2.0f dB (decoder?)     %d\n", edges[4], bins[k]);
    }

    printf("\nSDR-only packets (offline decode, not heard by the reference):\n");
    int only = 0, unknown = 0;
    for (int j = 0; j < off->n; j++) {
        pkt_t *p = &off->p[j];
        if (p->match >= 0) continue;
        bool known = ref_knows(p->mmsi);
        only++;
        unknown += !known;
        if (verbose || !known) {
            printf("  %7.2f s  ch %c  type %2d  MMSI %09u  %s\n", p->t, p->ch, p->type, (unsigned)p->mmsi,
                   known ? "vessel known to the reference" : "UNKNOWN VESSEL: possible false decode");
        }
    }
    printf("  %d packets, %d from vessels the reference never heard\n", only, unknown);
    return 0;
}
