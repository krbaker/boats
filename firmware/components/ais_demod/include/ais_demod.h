// AIS demodulator: turns RTL-SDR I/Q samples (unsigned 8-bit, 288 kS/s,
// centred between the two AIS channels on 162.000 MHz) into !AIVDM sentences
// for the AIS parser. Portable C with no ESP-IDF dependencies, so it builds
// and is tested on the host (test/host/test_ais_demod.c).
//
// Per channel: NCO down to 0 Hz -> low-pass + decimate to 48 kS/s (5 samples
// per bit), then for each of the five sampling phases, two demodulators that
// decode independently (duplicates are dropped):
//   - FM: discriminator -> short matched filter -> carrier offset (DC)
//     removal -> sign -> NRZI.
//   - Coherent: narrower filter -> derotate -90 degrees per bit (GMSK with
//     h = 0.5 is offset-QPSK in disguise) -> carrier phase from the squared
//     symbols either side -> sign -> differential/NRZI decoding.
// Then HDLC -> CRC-16 -> six-bit armour. The coherent branch is about 3 dB
// more sensitive; the FM branch copes better with packets far off frequency.
// The carrier offset measured on each good packet slowly retunes both NCOs, so
// a dongle with a poor crystal locks on by itself.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIS_DEMOD_RATE      288000      // input sample rate (complex samples/s)
#define AIS_DEMOD_CENTER_HZ 162000000   // tuned frequency the channels are relative to

// Called with each sentence, without CR/LF. A long message comes as several
// calls in a row, one per fragment.
typedef void (*ais_demod_sentence_fn)(void *ctx, const char *sentence);

typedef struct {
    uint32_t packets[2];        // good packets, channel A and B
    uint32_t bad_crc;           // flag-delimited frames of plausible length that failed the CRC
    float level_dbfs[2];        // signal level of the last good packet per channel
    float noise_dbfs[2];        // noise floor estimate per channel
    float freq_corr_hz;         // automatic correction currently applied
    float ppm;                  // total tuning correction (manual + automatic), in ppm
} ais_demod_stats_t;

typedef struct ais_demod ais_demod_t;

// Allocates a demodulator (about 18 KB). Returns NULL if out of memory.
ais_demod_t *ais_demod_new(ais_demod_sentence_fn fn, void *ctx);
void ais_demod_free(ais_demod_t *d);
// Or set one up in memory the caller provides (ais_demod_size() bytes), for
// example fast internal RAM. Don't call ais_demod_free on it.
size_t ais_demod_size(void);
ais_demod_t *ais_demod_init(void *mem, ais_demod_sentence_fn fn, void *ctx);

// Feeds interleaved I/Q bytes; n_bytes should be even (an odd last byte is
// held over to the next call).
void ais_demod_feed(ais_demod_t *d, const uint8_t *iq, size_t n_bytes);

// Manual frequency correction for the dongle's crystal, in ppm (positive when
// the dongle tunes high). Automatic tracking continues on top of it.
void ais_demod_set_ppm(ais_demod_t *d, float ppm);
// Turns automatic frequency tracking on (the default) or off.
void ais_demod_set_tracking(ais_demod_t *d, int on);

void ais_demod_get_stats(const ais_demod_t *d, ais_demod_stats_t *out);

#ifdef __cplusplus
}
#endif
