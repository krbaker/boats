// Host-side tests for the AIS demodulator, using synthetic AIS packets: a
// known payload is framed (training sequence, HDLC flags, CRC, bit stuffing),
// NRZI encoded, GMSK modulated (BT 0.4, 9600 bit/s, +-2400 Hz) onto channel A
// or B, given noise and a frequency offset, and quantised to 8-bit I/Q like
// the RTL-SDR's output.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ais.h"
#include "ais_demod.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// Random numbers (deterministic)
// ---------------------------------------------------------------------------

static uint64_t rng = 0x2545f4914f6cdd1dULL;

static double urand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return ((rng >> 11) + 0.5) / 9007199254740992.0;
}

static double grand(void)
{
    return sqrt(-2 * log(urand())) * cos(2 * M_PI * urand());
}

// ---------------------------------------------------------------------------
// Modulator
// ---------------------------------------------------------------------------

#define FS      AIS_DEMOD_RATE
#define BAUD    9600
#define SPB     (FS / BAUD)    // 30 samples per bit
#define MAXBITS 2000

// Armoured payload -> bytes, AIS field order (MSB first).
static int dearmour(const char *p, uint8_t *out)
{
    int nbits = 0;
    memset(out, 0, 128);
    for (; *p; p++) {
        int v = *p - 48;
        if (v > 40) v -= 8;
        for (int b = 5; b >= 0; b--, nbits++) {
            if ((v >> b) & 1) out[nbits >> 3] |= 0x80 >> (nbits & 7);
        }
    }
    return nbits / 8;
}

static uint16_t crc16_x25(const uint8_t *p, int n)
{
    uint16_t crc = 0xffff;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}

// Frame bits as sent, before NRZI: ramp, training, flag, stuffed data + CRC, flag, ramp.
static int frame_bits(const char *payload, uint8_t *bits)
{
    uint8_t bytes[130];
    int nbytes = dearmour(payload, bytes);
    uint16_t crc = ~crc16_x25(bytes, nbytes);
    bytes[nbytes++] = crc & 0xff;
    bytes[nbytes++] = crc >> 8;

    int n = 0;
    for (int i = 0; i < 8; i++) bits[n++] = 0;               // ramp up
    for (int i = 0; i < 24; i++) bits[n++] = i & 1;          // training 0101...
    static const uint8_t flag[8] = {0, 1, 1, 1, 1, 1, 1, 0};
    memcpy(bits + n, flag, 8);
    n += 8;
    int ones = 0;
    for (int i = 0; i < nbytes; i++) {
        for (int b = 0; b < 8; b++) {  // each byte least significant bit first
            int bit = (bytes[i] >> b) & 1;
            bits[n++] = bit;
            ones = bit ? ones + 1 : 0;
            if (ones == 5) {
                bits[n++] = 0;
                ones = 0;
            }
        }
    }
    memcpy(bits + n, flag, 8);
    n += 8;
    for (int i = 0; i < 8; i++) bits[n++] = 0;               // buffer / ramp down
    return n;
}

typedef struct {
    double phase;
    double t;            // samples so far (for the carrier)
} modstate_t;

// Appends one packet at `snr_db` (in a 10 kHz bandwidth) to iq (u8 pairs),
// on channel offset ch_hz plus freq_off_hz. Returns samples written.
// Before and after it: `gap` samples of noise only.
static int modulate(const char *payload, double ch_hz, double freq_off_hz, double snr_db, int gap, uint8_t *iq,
                    double amp)
{
    uint8_t bits[MAXBITS];
    int nb = frame_bits(payload, bits);

    // NRZI: a 0 toggles the level, a 1 keeps it.
    static double nrz[MAXBITS * SPB];
    int level = 1;
    for (int i = 0; i < nb; i++) {
        if (!bits[i]) level = -level;
        for (int k = 0; k < SPB; k++) nrz[i * SPB + k] = level;
    }
    int ns = nb * SPB;

    // Gaussian filter for BT 0.4, unity gain.
    const int gl = 3 * SPB;
    double g[3 * SPB], gs = 0;
    double sigma = sqrt(log(2)) / (2 * M_PI * 0.4) * SPB;
    for (int i = 0; i < gl; i++) {
        double t = i - (gl - 1) / 2.0;
        g[i] = exp(-t * t / (2 * sigma * sigma));
        gs += g[i];
    }

    double noise_var = amp * amp / pow(10, snr_db / 10) * (FS / 10000.0);  // per complex sample
    double ns_sd = sqrt(noise_var / 2);
    int out = 0;
    double phase = 0;
    int total = gap + ns + gap;
    for (int n = 0; n < total; n++) {
        double si = 0, sq = 0;
        int k = n - gap;
        if (k >= 0 && k < ns) {
            double f = 0;
            for (int j = 0; j < gl; j++) {
                int idx = k - j + gl / 2;
                if (idx >= 0 && idx < ns) f += nrz[idx] * g[j];
            }
            f = f / gs * 2400.0;
            phase += 2 * M_PI * (f + ch_hz + freq_off_hz) / FS;
            si = amp * cos(phase);
            sq = amp * sin(phase);
        }
        double vi = si + ns_sd * grand(), vq = sq + ns_sd * grand();
        int qi = (int)lround(vi * 127.5 + 127.5), qq = (int)lround(vq * 127.5 + 127.5);
        iq[out++] = qi < 0 ? 0 : qi > 255 ? 255 : qi;
        iq[out++] = qq < 0 ? 0 : qq > 255 ? 255 : qq;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

#define MAX_SENT 64
static char sentences[MAX_SENT][100];
static int nsent;

static void collect(void *ctx, const char *s)
{
    (void)ctx;
    if (nsent < MAX_SENT) snprintf(sentences[nsent], sizeof(sentences[0]), "%s", s);
    nsent++;
}

static uint8_t iqbuf[2 * 400000];

// Sends `count` packets of `payload` and returns how many decoded intact.
static int run(ais_demod_t *d, const char *payload, char channel, double off_hz, double snr_db, int count)
{
    int good = 0;
    double ch_hz = channel == 'A' ? -25000 : 25000;
    for (int i = 0; i < count; i++) {
        nsent = 0;
        int n = modulate(payload, ch_hz, off_hz, snr_db, 3000, iqbuf, 0.3);
        ais_demod_feed(d, iqbuf, 2 * n);
        for (int s = 0; s < nsent && s < MAX_SENT; s++) {
            char want[100];
            snprintf(want, sizeof(want), "!AIVDM,1,1,,%c,%s,", channel, payload);
            if (!strncmp(sentences[s], want, strlen(want))) good++;
        }
    }
    return good;
}

static const char *k_type1 = "15NRQ1PP00rsR@BHE;Ibh?wF0L1W";   // decoded from a real capture
static const char *k_type18 = "B5NJ;PP005l4ot5Isbl03wsUkP06";

static void test_clean(void)
{
    ais_demod_t *d = ais_demod_new(collect, NULL);
    CHECK(run(d, k_type1, 'A', 0, 40, 1) == 1);
    CHECK(run(d, k_type1, 'B', 0, 40, 1) == 1);
    CHECK(run(d, k_type18, 'B', 0, 40, 1) == 1);

    // And the sentence is one the parser accepts, checksum included.
    nsent = 0;
    int n = modulate(k_type1, -25000, 0, 40, 3000, iqbuf, 0.3);
    ais_demod_feed(d, iqbuf, 2 * n);
    CHECK(nsent == 1);
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    CHECK(nsent >= 1 && ais_parse_line(&p, sentences[0], 0, &r) == AIS_RESULT_REPORT);
    CHECK(r.mmsi == 367567110);
    ais_demod_free(d);
}

static void test_multipart(void)
{
    // Type 5 (424 bits): more than 60 armour characters, so two sentences.
    const char *t5 = "55?MbV02;H;s<HtKR20EHE:0@T4@Dn2222222216L961O5Gf0NSQEp6ClRp888888888880";
    ais_demod_t *d = ais_demod_new(collect, NULL);
    nsent = 0;
    int n = modulate(t5, 25000, 0, 40, 3000, iqbuf, 0.3);
    ais_demod_feed(d, iqbuf, 2 * n);
    CHECK(nsent == 2);
    ais_parser_t p;
    ais_report_t r;
    ais_parser_init(&p);
    ais_result_t res = AIS_RESULT_PENDING;
    for (int i = 0; i < nsent && i < MAX_SENT; i++) res = ais_parse_line(&p, sentences[i], 0, &r);
    CHECK(res == AIS_RESULT_REPORT);
    CHECK(r.mmsi == 351759000);
    CHECK(!strcmp(r.name, "EVER DIADEM"));
    ais_demod_free(d);
}

// Decode rate against SNR (in a 10 kHz bandwidth) and frequency offset;
// prints a table. Measured performance, with some margin: every packet at
// >= 14 dB, >= 90% at 12 dB (about 80% at 10 dB), and >= 80% at 12 dB for
// packets up to 5 kHz off frequency once tracking has locked.
static void test_sweep(void)
{
    const int count = 40;
    printf("SNR (dB)   A ok   B ok\n");
    for (int snr = 4; snr <= 16; snr += 2) {
        ais_demod_t *d = ais_demod_new(collect, NULL);
        ais_demod_set_tracking(d, 0);
        int a = run(d, k_type1, 'A', 0, snr, count), b = run(d, k_type1, 'B', 0, snr, count);
        printf("%6d   %4d/%d %4d/%d\n", snr, a, count, b, count);
        if (snr >= 14) CHECK(a == count && b == count);
        if (snr == 12) CHECK(a >= count * 9 / 10 && b >= count * 9 / 10);
        ais_demod_free(d);
    }
    printf("offset (Hz)  first 5   next %d (12 dB, tracking on)\n", count);
    static const int offs[] = {-5000, -2000, -500, 500, 2000, 5000};
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        ais_demod_t *d = ais_demod_new(collect, NULL);
        int first = run(d, k_type1, i & 1 ? 'A' : 'B', offs[i], 12, 5);
        int rest = run(d, k_type1, i & 1 ? 'A' : 'B', offs[i], 12, count);
        ais_demod_stats_t st;
        ais_demod_get_stats(d, &st);
        printf("%9d    %d/5      %d/%d  (correction %+.0f Hz)\n", offs[i], first, rest, count, st.freq_corr_hz);
        CHECK(rest >= count * 8 / 10);
        CHECK(fabs(st.freq_corr_hz - offs[i]) < 100);
        ais_demod_free(d);
    }
}

int main(int argc, char **argv)
{
    // --dump FILE: write a few clean packets (A and B) as a .cu8 file for
    // inspecting the modulator with other tools.
    if (argc == 3 && !strcmp(argv[1], "--dump")) {
        FILE *f = fopen(argv[2], "wb");
        if (!f) return 2;
        for (int i = 0; i < 4; i++) {
            int n = modulate(k_type1, i & 1 ? 25000 : -25000, 0, 30, 6000, iqbuf, 0.3);
            fwrite(iqbuf, 2, n, f);
        }
        fclose(f);
        return 0;
    }
    test_clean();
    test_multipart();
    test_sweep();
    if (failures) {
        printf("%d demodulator check(s) failed\n", failures);
        return 1;
    }
    printf("demodulator tests passed\n");
    return 0;
}
