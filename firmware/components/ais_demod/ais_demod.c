// AIS demodulator: see ais_demod.h for the signal path.

#include "ais_demod.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEC        6                          // 288 kS/s -> 48 kS/s
#define FS_OUT     (AIS_DEMOD_RATE / DEC)
#define SPS        5                          // samples per bit at 48 kS/s (9600 bit/s)
#define LPF_TAPS   64
#ifndef LPF_CUT_HZ
#define LPF_CUT_HZ 12000.0f
#endif
#ifndef MF_SPAN
#define MF_SPAN    1                          // matched filter length in bits
#endif
#define MF_TAPS    (MF_SPAN * SPS)
#define GMSK_BT    0.4f
#define LUT_BITS   10
#define LUT_N      (1 << LUT_BITS)
#define MAX_BYTES  160                        // 5 slots is 1280 bits incl. stuffing
#define MIN_BITS   72                         // shortest plausible frame (incl. CRC)
#define CH_OFFSET  25000.0f

// Carrier offset (DC) tracking: a slow average normally, and for a short
// window after a packet starts, the exact mean of the last four bits (the
// training sequence 0101... is ++--++-- after NRZI, so that window averages
// to the offset).
#define DC_SLOW       (1.0f / 256)
#define DC_FAST_WIN   (4 * SPS)
#ifndef DC_FAST_LEN
#define DC_FAST_LEN   (16 * SPS)
#endif
// Packet detection: fast power vs the noise floor.
#define PWR_FAST      0.1f
#define NOISE_DOWN    0.002f
#define NOISE_UP      0.00005f
#define DETECT_ON     4.0f                    // 6 dB above the floor
#define DETECT_OFF    2.0f
// Coherent branch: carrier phase from the symbols within COH_D bits either side.
#ifndef COH_D
#define COH_D         2
#endif
#define COH_W         (2 * COH_D + 1)
// The coherent branch gets a narrower channel filter (at 48 kS/s) than the FM one.
#ifndef COH_CUT_HZ
#define COH_CUT_HZ    7000.0f
#endif
#ifndef COH_TAPS
#define COH_TAPS      21
#endif
#define RECENT        4                       // packets remembered per channel, to drop duplicates
#define DUP_WINDOW    (30 * FS_OUT / 1000)    // 30 ms
// Frequency tracking: fraction of each good packet's offset applied.
#define TRACK_GAIN    0.3f
#define TRACK_MAX_HZ  10000.0f

// One HDLC frame decoder, fed bits after NRZI decoding.
typedef struct {
    uint8_t shreg;              // last 8 bits
    bool in_frame;
    int nbits, ones;
    uint8_t buf[MAX_BYTES];
    float raw_sum;              // discriminator before DC removal, for the carrier offset
    float pwr_sum;
    int nsamp;
} hdlc_t;

// One sampling phase (of SPS) of one channel, with two demodulators:
// FM discriminator, and coherent. Both decode independently; duplicates are
// dropped in frame_end.
typedef struct {
    // FM: sign of the frequency, NRZI.
    bool prev_sym;
    hdlc_t fm;
    // Coherent: the samples, derotated by -90 degrees per bit (and by the
    // carrier offset), are +-1 times the carrier phase; that phase comes from
    // the average of z^2 over the window (squaring removes the data).
    float rot_i, rot_q;
    float zi[COH_W], zq[COH_W];
    int zpos, zn;
    float ui, uq;               // carrier phase reference (modulo 180 degrees), kept continuous
    int8_t c1, c2;              // last two symbol decisions
    hdlc_t coh;
} slicer_t;

typedef struct {
    uint8_t bytes[MAX_BYTES];
    int len;
    uint32_t t;
} recent_t;

typedef struct {
    char name;
    float f_ch;                 // channel offset from the centre
    uint32_t phase, inc;
    float hist_i[2 * LPF_TAPS], hist_q[2 * LPF_TAPS];
    int hpos, dec_n;
    float prev_i, prev_q;
    float mf_hist[2 * MF_TAPS];
    int mfpos;
    float coh_i[2 * COH_TAPS], coh_q[2 * COH_TAPS];
    int cohpos;
    float fast_win[DC_FAST_WIN];
    float fast_sum;
    int fast_pos, fast_left;
    float dc;
    float pwr_fast, noise;
    bool active;
    uint32_t n48;
    slicer_t sl[SPS];
    // The slicers and both demodulators usually all decode a strong packet:
    // emit it once.
    recent_t recent[RECENT];
    int recent_pos;
} chan_t;

struct ais_demod {
    ais_demod_sentence_fn fn;
    void *ctx;
    float lpf[LPF_TAPS];
    float coh_lpf[COH_TAPS];
    float mf[MF_TAPS];
    float lut_cos[LUT_N], lut_sin[LUT_N];
    chan_t ch[2];
    float ppm, corr_hz;
    bool tracking;
    uint8_t carry;
    bool has_carry;
    int seq;
    ais_demod_stats_t stats;
};

// ---------------------------------------------------------------------------
// Set-up
// ---------------------------------------------------------------------------

// Low-pass: Hamming-windowed sinc, unity gain at DC.
static void design_lpf(float *h, int n, float cut_hz, float fs)
{
    float sum = 0;
    for (int i = 0; i < n; i++) {
        float t = i - (n - 1) / 2.0f;
        float x = 2 * cut_hz / fs;
        float s = t == 0 ? x : sinf((float)M_PI * x * t) / ((float)M_PI * t);
        float w = 0.54f - 0.46f * cosf(2 * (float)M_PI * i / (n - 1));
        h[i] = s * w;
        sum += h[i];
    }
    for (int i = 0; i < n; i++) h[i] /= sum;
}

static void design_filters(ais_demod_t *d)
{
    design_lpf(d->lpf, LPF_TAPS, LPF_CUT_HZ, AIS_DEMOD_RATE);
    design_lpf(d->coh_lpf, COH_TAPS, COH_CUT_HZ, FS_OUT);
    float sum;

    // Matched filter: the GMSK frequency pulse (a 1-bit rectangle through a
    // Gaussian filter of bandwidth BT), unity gain at DC.
    const float k = 2 * (float)M_PI * GMSK_BT / sqrtf(logf(2));
    sum = 0;
    for (int i = 0; i < MF_TAPS; i++) {
        float t = (i - (MF_TAPS - 1) / 2.0f) / SPS;  // in bits
        d->mf[i] = erff(k * (t + 0.5f) / sqrtf(2)) - erff(k * (t - 0.5f) / sqrtf(2));
        sum += d->mf[i];
    }
    for (int i = 0; i < MF_TAPS; i++) d->mf[i] /= sum;
}

static void update_nco(ais_demod_t *d)
{
    float ppm_hz = d->ppm * (AIS_DEMOD_CENTER_HZ / 1e6f);
    for (int c = 0; c < 2; c++) {
        // Where the channel actually lands in the samples, shifted down to 0 Hz.
        float f = d->ch[c].f_ch - ppm_hz + d->corr_hz;
        d->ch[c].inc = (uint32_t)(int32_t)llroundf(-f / AIS_DEMOD_RATE * 4294967296.0f);
    }
    d->stats.freq_corr_hz = d->corr_hz;
    d->stats.ppm = d->ppm - d->corr_hz / (AIS_DEMOD_CENTER_HZ / 1e6f);
}

size_t ais_demod_size(void)
{
    return sizeof(ais_demod_t);
}

ais_demod_t *ais_demod_new(ais_demod_sentence_fn fn, void *ctx)
{
    void *mem = malloc(sizeof(ais_demod_t));
    return mem ? ais_demod_init(mem, fn, ctx) : NULL;
}

ais_demod_t *ais_demod_init(void *mem, ais_demod_sentence_fn fn, void *ctx)
{
    ais_demod_t *d = mem;
    memset(d, 0, sizeof(*d));
    d->fn = fn;
    d->ctx = ctx;
    d->tracking = true;
    design_filters(d);
    for (int i = 0; i < LUT_N; i++) {
        d->lut_cos[i] = cosf(2 * (float)M_PI * i / LUT_N);
        d->lut_sin[i] = sinf(2 * (float)M_PI * i / LUT_N);
    }
    d->ch[0].name = 'A';
    d->ch[0].f_ch = -CH_OFFSET;
    d->ch[1].name = 'B';
    d->ch[1].f_ch = CH_OFFSET;
    for (int c = 0; c < 2; c++) {
        d->ch[c].noise = 1e-4f;
        d->stats.level_dbfs[c] = -100;
        for (int i = 0; i < SPS; i++) {
            d->ch[c].sl[i].rot_i = d->ch[c].sl[i].ui = 1;
            d->ch[c].sl[i].c1 = d->ch[c].sl[i].c2 = 1;
        }
    }
    update_nco(d);
    return d;
}

void ais_demod_free(ais_demod_t *d)
{
    free(d);
}

void ais_demod_set_ppm(ais_demod_t *d, float ppm)
{
    d->ppm = ppm;
    update_nco(d);
}

void ais_demod_set_tracking(ais_demod_t *d, int on)
{
    d->tracking = on;
    if (!on) d->corr_hz = 0;
    update_nco(d);
}

void ais_demod_get_stats(const ais_demod_t *d, ais_demod_stats_t *out)
{
    *out = d->stats;
    for (int c = 0; c < 2; c++) out->noise_dbfs[c] = 10 * log10f(d->ch[c].noise + 1e-12f);
}

// ---------------------------------------------------------------------------
// Output: six-bit armour into !AIVDM sentences
// ---------------------------------------------------------------------------

static void emit(ais_demod_t *d, char channel, const uint8_t *bytes, int nbytes)
{
    char armour[MAX_BYTES * 8 / 6 + 2];
    int nbits = nbytes * 8, nchars = (nbits + 5) / 6;
    for (int i = 0; i < nchars; i++) {
        int v = 0;
        for (int b = 0; b < 6; b++) {
            int k = i * 6 + b;
            // Bytes were assembled first-received bit first (LSB), but AIS
            // fields run most significant bit first within each byte.
            int bit = k < nbits ? (bytes[k >> 3] >> (7 - (k & 7))) & 1 : 0;
            v = (v << 1) | bit;
        }
        armour[i] = v < 40 ? v + 48 : v + 56;
    }
    int fill = nchars * 6 - nbits;

    const int per = 60;
    int frags = (nchars + per - 1) / per;
    char seq[4] = "";
    if (frags > 1) {
        snprintf(seq, sizeof(seq), "%d", d->seq);
        d->seq = (d->seq + 1) % 10;
    }
    for (int f = 0; f < frags; f++) {
        char s[100];
        int len = nchars - f * per < per ? nchars - f * per : per;
        int n = snprintf(s, sizeof(s), "!AIVDM,%d,%d,%s,%c,%.*s,%d", frags, f + 1, seq, channel, len,
                         armour + f * per, f == frags - 1 ? fill : 0);
        uint8_t ck = 0;
        for (int i = 1; i < n; i++) ck ^= (uint8_t)s[i];
        snprintf(s + n, sizeof(s) - n, "*%02X", ck);
        d->fn(d->ctx, s);
    }
}

// ---------------------------------------------------------------------------
// Bits: NRZI -> HDLC -> CRC
// ---------------------------------------------------------------------------

static uint16_t crc16_x25(const uint8_t *p, int n)
{
    uint16_t crc = 0xffff;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = crc & 1 ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}

static bool is_duplicate(chan_t *ch, const uint8_t *buf, int nbytes)
{
    for (int i = 0; i < RECENT; i++) {
        recent_t *r = &ch->recent[i];
        if (r->len == nbytes && ch->n48 - r->t < DUP_WINDOW && !memcmp(r->bytes, buf, nbytes)) return true;
    }
    recent_t *r = &ch->recent[ch->recent_pos];
    ch->recent_pos = (ch->recent_pos + 1) % RECENT;
    memcpy(r->bytes, buf, nbytes);
    r->len = nbytes;
    r->t = ch->n48;
    return false;
}

static void frame_end(ais_demod_t *d, chan_t *ch, hdlc_t *h)
{
    // The flag's leading 0 and five 1s were taken as data before the flag
    // showed itself.
    int n = h->nbits - 6;
    if (n < MIN_BITS || n % 8) return;
    int nbytes = n / 8;
    if (crc16_x25(h->buf, nbytes) != 0xf0b8) {
        d->stats.bad_crc++;
        return;
    }
    if (is_duplicate(ch, h->buf, nbytes)) return;

    int c = ch == &d->ch[0] ? 0 : 1;
    d->stats.packets[c]++;
    if (h->nsamp) {
        d->stats.level_dbfs[c] = 10 * log10f(h->pwr_sum / h->nsamp + 1e-12f);
        float off_hz = h->raw_sum / h->nsamp * FS_OUT / (2 * (float)M_PI);
        if (d->tracking) {
            d->corr_hz += TRACK_GAIN * off_hz;
            if (d->corr_hz > TRACK_MAX_HZ) d->corr_hz = TRACK_MAX_HZ;
            if (d->corr_hz < -TRACK_MAX_HZ) d->corr_hz = -TRACK_MAX_HZ;
            update_nco(d);
        }
    }
    emit(d, ch->name, h->buf, nbytes - 2);
}

static void hdlc_bit(ais_demod_t *d, chan_t *ch, hdlc_t *h, int bit, float raw, float pwr)
{
    if (h->in_frame) {
        h->raw_sum += raw;
        h->pwr_sum += pwr;
        h->nsamp++;
    }
    h->shreg = (h->shreg << 1) | bit;
    if (h->shreg == 0x7e) {  // flag: ends one frame, starts the next
        if (h->in_frame) frame_end(d, ch, h);
        h->in_frame = true;
        h->nbits = h->ones = 0;
        h->raw_sum = h->pwr_sum = 0;
        h->nsamp = 0;
        memset(h->buf, 0, sizeof(h->buf));
        return;
    }
    if ((h->shreg & 0x7f) == 0x7f) {  // seven 1s: abort
        h->in_frame = false;
        return;
    }
    if (!h->in_frame) return;
    if (bit) {
        if (++h->ones > 5) return;  // a sixth 1 can only be part of a flag
    } else {
        int stuffed = h->ones == 5;
        h->ones = 0;
        if (stuffed) return;
    }
    if (h->nbits >= MAX_BYTES * 8) {
        h->in_frame = false;
        return;
    }
    if (bit) h->buf[h->nbits >> 3] |= 1 << (h->nbits & 7);
    h->nbits++;
}

static void coherent_sample(ais_demod_t *d, chan_t *ch, slicer_t *s, float yi, float yq, float raw, float pwr)
{
    float zi = yi * s->rot_i - yq * s->rot_q, zq = yi * s->rot_q + yq * s->rot_i;
    // Next bit: rotate a further -90 degrees and by the carrier offset (dc is
    // radians per sample).
    uint32_t idx = (uint32_t)(int32_t)(-ch->dc * SPS * (LUT_N / (2 * (float)M_PI))) & (LUT_N - 1);
    float wi = d->lut_sin[idx], wq = -d->lut_cos[idx];  // e^{ja} * -j
    float ri = s->rot_i * wi - s->rot_q * wq, rq = s->rot_i * wq + s->rot_q * wi;
    float rn = 1 / sqrtf(ri * ri + rq * rq);
    s->rot_i = ri * rn;
    s->rot_q = rq * rn;

    s->zi[s->zpos] = zi;
    s->zq[s->zpos] = zq;
    s->zpos = (s->zpos + 1) % COH_W;
    if (s->zn < COH_W) {
        s->zn++;
        return;
    }
    float si = 0, sq = 0;
    for (int i = 0; i < COH_W; i++) {
        si += s->zi[i] * s->zi[i] - s->zq[i] * s->zq[i];
        sq += 2 * s->zi[i] * s->zq[i];
    }
    // Half the angle of the sum: the carrier phase, modulo 180 degrees.
    float m = sqrtf(si * si + sq * sq);
    if (m > 1e-12f) {
        float hi = si / m + 1, hq = sq / m;
        float hm = sqrtf(hi * hi + hq * hq);
        float ui = hm > 1e-3f ? hi / hm : 0, uq = hm > 1e-3f ? hq / hm : 1;
        if (ui * s->ui + uq * s->uq < 0) ui = -ui, uq = -uq;  // no 180-degree jumps
        s->ui = ui;
        s->uq = uq;
    }
    int k = (s->zpos + COH_D) % COH_W;  // the middle of the window
    int8_t c = s->zi[k] * s->ui + s->zq[k] * s->uq > 0 ? 1 : -1;
    // c_k = c_{k-1} * (frequency sign of bit k), and NRZI gives a 1 when the
    // frequency sign doesn't change: that is when c_k == c_{k-2}.
    int bit = c == s->c2;
    s->c2 = s->c1;
    s->c1 = c;
    hdlc_bit(d, ch, &s->coh, bit, raw, pwr);
}

static void slicer_sample(ais_demod_t *d, chan_t *ch, slicer_t *s, float x, float raw, float pwr, float yi, float yq)
{
    bool sym = x > 0;
    int bit = sym == s->prev_sym;  // NRZI: no change is a 1
    s->prev_sym = sym;
    hdlc_bit(d, ch, &s->fm, bit, raw, pwr);
    coherent_sample(d, ch, s, yi, yq, raw, pwr);
}

// ---------------------------------------------------------------------------
// Samples
// ---------------------------------------------------------------------------

// One 48 kS/s sample of one channel.
static void chan_sample(ais_demod_t *d, chan_t *ch, float yi, float yq)
{
    float pwr = yi * yi + yq * yq;
    ch->pwr_fast += (pwr - ch->pwr_fast) * PWR_FAST;
    ch->noise += (pwr - ch->noise) * (pwr < ch->noise ? NOISE_DOWN : NOISE_UP);
    if (!ch->active && ch->pwr_fast > ch->noise * DETECT_ON) {
        ch->active = true;
        ch->fast_left = DC_FAST_LEN;
    } else if (ch->active && ch->pwr_fast < ch->noise * DETECT_OFF) {
        ch->active = false;
    }

    // FM discriminator: phase change since the last sample.
    float re = yi * ch->prev_i + yq * ch->prev_q;
    float im = yq * ch->prev_i - yi * ch->prev_q;
    ch->prev_i = yi;
    ch->prev_q = yq;
    float disc = atan2f(im, re);

    ch->mf_hist[ch->mfpos] = ch->mf_hist[ch->mfpos + MF_TAPS] = disc;
    ch->mfpos = (ch->mfpos + 1) % MF_TAPS;
    const float *h = &ch->mf_hist[ch->mfpos];
    float m = 0;
    for (int i = 0; i < MF_TAPS; i++) m += h[i] * d->mf[i];

    ch->fast_sum += m - ch->fast_win[ch->fast_pos];
    ch->fast_win[ch->fast_pos] = m;
    ch->fast_pos = (ch->fast_pos + 1) % DC_FAST_WIN;
    if (ch->fast_left > 0) {
        ch->fast_left--;
        ch->dc = ch->fast_sum / DC_FAST_WIN;
    } else {
        ch->dc += (m - ch->dc) * DC_SLOW;
    }

    ch->coh_i[ch->cohpos] = ch->coh_i[ch->cohpos + COH_TAPS] = yi;
    ch->coh_q[ch->cohpos] = ch->coh_q[ch->cohpos + COH_TAPS] = yq;
    ch->cohpos = (ch->cohpos + 1) % COH_TAPS;
    const float *ci = &ch->coh_i[ch->cohpos], *cq = &ch->coh_q[ch->cohpos];
    float zi = 0, zq = 0;
    for (int i = 0; i < COH_TAPS; i++) {
        zi += ci[i] * d->coh_lpf[i];
        zq += cq[i] * d->coh_lpf[i];
    }

    slicer_sample(d, ch, &ch->sl[ch->n48 % SPS], m - ch->dc, m, pwr, zi, zq);
    ch->n48++;
}

static void sample(ais_demod_t *d, float x, float y)
{
    for (int c = 0; c < 2; c++) {
        chan_t *ch = &d->ch[c];
        uint32_t idx = ch->phase >> (32 - LUT_BITS);
        float co = d->lut_cos[idx], si = d->lut_sin[idx];
        ch->phase += ch->inc;
        float mi = x * co - y * si, mq = x * si + y * co;
        ch->hist_i[ch->hpos] = ch->hist_i[ch->hpos + LPF_TAPS] = mi;
        ch->hist_q[ch->hpos] = ch->hist_q[ch->hpos + LPF_TAPS] = mq;
        ch->hpos = (ch->hpos + 1) % LPF_TAPS;
        if (++ch->dec_n < DEC) continue;
        ch->dec_n = 0;
        const float *hi = &ch->hist_i[ch->hpos], *hq = &ch->hist_q[ch->hpos];
        float yi = 0, yq = 0;
        for (int i = 0; i < LPF_TAPS; i++) {
            yi += hi[i] * d->lpf[i];
            yq += hq[i] * d->lpf[i];
        }
        chan_sample(d, ch, yi, yq);
    }
}

void ais_demod_feed(ais_demod_t *d, const uint8_t *iq, size_t n)
{
    const float k = 1 / 127.5f;
    if (d->has_carry && n) {
        sample(d, (d->carry - 127.5f) * k, (iq[0] - 127.5f) * k);
        iq++;
        n--;
        d->has_carry = false;
    }
    for (; n >= 2; n -= 2, iq += 2) sample(d, (iq[0] - 127.5f) * k, (iq[1] - 127.5f) * k);
    if (n) {
        d->carry = iq[0];
        d->has_carry = true;
    }
}
