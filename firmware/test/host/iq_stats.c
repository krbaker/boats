// Signal health of a raw I/Q capture (.cu8, 288 kS/s, centred on 162.000 MHz,
// as recorded from the display's web page), as JSON on stdout: sample levels
// and clipping, DC offset, the averaged spectrum, a 100 ms power timeline, and
// the bursts on each AIS channel (A at -25 kHz, B at +25 kHz). Power is in dB
// relative to a full-scale complex tone (|I + jQ| = 1 after scaling to +-1).
//
//     build/iq_stats capture.cu8 > iq_stats.json
//
// Used by tools/sdr_test.py; a burst with no decoded packet near it is what
// the decoder (or the gain) missed.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS          288000
#define NFFT        1024                // 281 Hz bins, 3.6 ms frames
#define BLOCK       (FS / 10)           // timeline rows: 100 ms
#define CH_HZ       25000.0
#define CH_HALF_HZ  6000.0              // AIS: 9.6 kbit/s GMSK, most energy within +-6 kHz
#define BURST_DB    10.0                // above the channel's noise floor
#define BURST_MIN   4                   // frames (~14 ms): a 26.7 ms AIS slot spans ~7
#define MAX_FRAMES  (600 * FS / NFFT + 16)

typedef struct {
    double re, im;
} cpx_t;

static void fft(cpx_t *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            cpx_t t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = -2 * M_PI / len;
        cpx_t wl = {cos(a), sin(a)};
        for (int i = 0; i < n; i += len) {
            cpx_t w = {1, 0};
            for (int k = 0; k < len / 2; k++) {
                cpx_t u = x[i + k], v = x[i + k + len / 2];
                cpx_t t = {v.re * w.re - v.im * w.im, v.re * w.im + v.im * w.re};
                x[i + k] = (cpx_t){u.re + t.re, u.im + t.im};
                x[i + k + len / 2] = (cpx_t){u.re - t.re, u.im - t.im};
                w = (cpx_t){w.re * wl.re - w.im * wl.im, w.re * wl.im + w.im * wl.re};
            }
        }
    }
}

static double db(double p)
{
    return 10 * log10(p > 1e-20 ? p : 1e-20);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s capture.cu8\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }

    static double win[NFFT], psd[NFFT];
    double wpow = 0;
    for (int i = 0; i < NFFT; i++) {
        win[i] = 0.5 - 0.5 * cos(2 * M_PI * i / NFFT); // Hann
        wpow += win[i] * win[i];
    }
    double *chp[2] = {malloc(MAX_FRAMES * sizeof(double)), malloc(MAX_FRAMES * sizeof(double))};
    long nframes = 0;

    static uint64_t hist_i[256], hist_q[256];
    double sum_i = 0, sum_q = 0, sum_p = 0;
    uint64_t n = 0, clip = 0, dead = 0;

    // Timeline rows: total power, clipping and the busiest frame on each channel.
    printf("{\n  \"timeline\": [");
    double blk_p = 0, blk_peak[2] = {-200, -200};
    uint64_t blk_n = 0, blk_clip = 0;
    long rows = 0;

    static uint8_t buf[NFFT * 2];
    static cpx_t x[NFFT];
    int lo[2], hi[2];
    for (int c = 0; c < 2; c++) {
        double centre = c ? CH_HZ : -CH_HZ;
        lo[c] = (int)lround((centre - CH_HALF_HZ) / FS * NFFT);
        hi[c] = (int)lround((centre + CH_HALF_HZ) / FS * NFFT);
    }
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) == sizeof(buf)) {
        int frame_dead = 1;
        for (int i = 0; i < NFFT; i++) {
            uint8_t bi = buf[2 * i], bq = buf[2 * i + 1];
            hist_i[bi]++;
            hist_q[bq]++;
            if (bi == 0 || bi == 255 || bq == 0 || bq == 255) clip++, blk_clip++;
            if (bi != buf[0] || bq != buf[1]) frame_dead = 0;
            double vi = (bi - 127.5) / 127.5, vq = (bq - 127.5) / 127.5;
            sum_i += vi;
            sum_q += vq;
            double p = vi * vi + vq * vq;
            sum_p += p;
            blk_p += p;
            x[i] = (cpx_t){vi * win[i], vq * win[i]};
        }
        dead += frame_dead;
        n += NFFT;
        blk_n += NFFT;
        fft(x, NFFT);
        double norm = 1.0 / (wpow * NFFT);  // per-bin power; summing all bins gives the mean sample power
        for (int k = 0; k < NFFT; k++) psd[k] += (x[k].re * x[k].re + x[k].im * x[k].im) * norm;
        for (int c = 0; c < 2; c++) {
            double s = 0;
            for (int k = lo[c]; k <= hi[c]; k++) {
                cpx_t v = x[(k + NFFT) % NFFT];
                s += (v.re * v.re + v.im * v.im) * norm;
            }
            if (nframes < MAX_FRAMES) chp[c][nframes] = db(s);
            if (db(s) > blk_peak[c]) blk_peak[c] = db(s);
        }
        nframes++;

        if (blk_n >= BLOCK) {
            printf("%s\n    {\"t\": %.1f, \"power_db\": %.2f, \"clip_pct\": %.4f, \"peak_a_db\": %.2f, \"peak_b_db\": %.2f}",
                   rows ? "," : "", rows * 0.1, db(blk_p / blk_n), 100.0 * blk_clip / blk_n, blk_peak[0], blk_peak[1]);
            rows++;
            blk_p = 0;
            blk_n = blk_clip = 0;
            blk_peak[0] = blk_peak[1] = -200;
        }
    }
    fclose(f);
    printf("\n  ],\n");
    if (!n) {
        fprintf(stderr, "%s: no samples\n", argv[1]);
        return 1;
    }
    if (nframes > MAX_FRAMES) nframes = MAX_FRAMES;

    printf("  \"samples\": %llu,\n  \"seconds\": %.3f,\n", (unsigned long long)n, (double)n / FS);
    printf("  \"dc_i\": %.5f,\n  \"dc_q\": %.5f,\n", sum_i / n, sum_q / n);
    printf("  \"power_db\": %.2f,\n  \"clip_pct\": %.5f,\n", db(sum_p / n), 100.0 * clip / n);
    printf("  \"dead_frames\": %llu,\n", (unsigned long long)dead);
    int min_i = 0, max_i = 255, min_q = 0, max_q = 255;
    while (!hist_i[min_i]) min_i++;
    while (!hist_i[max_i]) max_i--;
    while (!hist_q[min_q]) min_q++;
    while (!hist_q[max_q]) max_q--;
    printf("  \"range_i\": [%d, %d],\n  \"range_q\": [%d, %d],\n", min_i, max_i, min_q, max_q);
    // Effective bits: how many of the 8 the signal actually swings through.
    printf("  \"bits_used_i\": %.2f,\n  \"bits_used_q\": %.2f,\n", log2(max_i - min_i + 1.0), log2(max_q - min_q + 1.0));
    printf("  \"hist_i\": [");
    for (int i = 0; i < 256; i++) printf("%s%llu", i ? "," : "", (unsigned long long)hist_i[i]);
    printf("],\n  \"hist_q\": [");
    for (int i = 0; i < 256; i++) printf("%s%llu", i ? "," : "", (unsigned long long)hist_q[i]);
    printf("],\n");

    // Spectrum, -FS/2 .. +FS/2, and the median as the floor spurs stand out from.
    static double spec[NFFT], sorted[NFFT];
    for (int k = 0; k < NFFT; k++) spec[k] = db(psd[(k + NFFT / 2) % NFFT] / nframes);
    memcpy(sorted, spec, sizeof(spec));
    qsort(sorted, NFFT, sizeof(double), cmp_double);
    double floor_db = sorted[NFFT / 2];
    printf("  \"spectrum_bin_hz\": %.3f,\n  \"spectrum_floor_db\": %.2f,\n  \"spectrum_db\": [", (double)FS / NFFT,
           floor_db);
    for (int k = 0; k < NFFT; k++) printf("%s%.2f", k ? "," : "", spec[k]);
    printf("],\n  \"spurs\": [");
    int nspur = 0;
    for (int k = 1; k < NFFT - 1; k++) {
        if (spec[k] > floor_db + 10 && spec[k] >= spec[k - 1] && spec[k] >= spec[k + 1]) {
            printf("%s{\"hz\": %.0f, \"db_above_floor\": %.1f}", nspur++ ? ", " : "", (k - NFFT / 2) * (double)FS / NFFT,
                   spec[k] - floor_db);
        }
    }
    printf("],\n  \"dc_bin_db_above_floor\": %.1f,\n", spec[NFFT / 2] - floor_db);

    // Each channel: noise floor (20th percentile of frame power) and bursts.
    double *tmp = malloc(nframes * sizeof(double));
    printf("  \"channels\": {");
    for (int c = 0; c < 2; c++) {
        memcpy(tmp, chp[c], nframes * sizeof(double));
        qsort(tmp, nframes, sizeof(double), cmp_double);
        double nf = tmp[nframes / 5], med = tmp[nframes / 2];
        printf("%s\n    \"%c\": {\"offset_hz\": %.0f, \"noise_db\": %.2f, \"median_db\": %.2f, \"bursts\": [", c ? "," : "",
               c ? 'B' : 'A', c ? CH_HZ : -CH_HZ, nf, med);
        int nb = 0;
        for (long i = 0; i < nframes;) {
            if (chp[c][i] < nf + BURST_DB) {
                i++;
                continue;
            }
            long j = i;
            double peak = -200, sum = 0;
            while (j < nframes && chp[c][j] >= nf + BURST_DB) {
                if (chp[c][j] > peak) peak = chp[c][j];
                sum += pow(10, chp[c][j] / 10);
                j++;
            }
            if (j - i >= BURST_MIN) {
                printf("%s\n      {\"t\": %.4f, \"dur_ms\": %.1f, \"peak_db\": %.2f, \"snr_db\": %.2f, \"mean_db\": %.2f}",
                       nb++ ? "," : "", (double)i * NFFT / FS, (j - i) * 1000.0 * NFFT / FS, peak, peak - nf,
                       db(sum / (j - i)));
            }
            i = j;
        }
        printf("\n    ]}");
    }
    printf("\n  },\n  \"frames\": %ld\n}\n", nframes);
    free(tmp);
    free(chp[0]);
    free(chp[1]);
    return 0;
}
