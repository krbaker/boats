// Runs the AIS demodulator over a raw I/Q capture (.cu8, 288 kS/s, centred on
// 162.000 MHz, as recorded from the display's web page or by
// `rtl_sdr -f 162000000 -s 288000`) and prints each sentence with what the
// parser makes of it.
//
//     build/ais_demod_file capture.cu8 [ppm]
//
// Exits 0 if at least one packet decoded.

#include <stdio.h>
#include <stdlib.h>

#include "ais.h"
#include "ais_demod.h"

static ais_parser_t parser;
static long sample_pos;

static void on_sentence(void *ctx, const char *s)
{
    (void)ctx;
    ais_report_t r;
    ais_result_t res = ais_parse_line(&parser, s, 0, &r);
    printf("%7.3f s  %-80s", sample_pos / (double)AIS_DEMOD_RATE, s);
    if (res == AIS_RESULT_REPORT) printf("  type %2d MMSI %09u%s%s", r.msg_type, (unsigned)r.mmsi, r.name[0] ? " " : "", r.name);
    else if (res != AIS_RESULT_PENDING) printf("  (parser: %d)", res);
    printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s capture.cu8 [ppm]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    ais_parser_init(&parser);
    ais_demod_t *d = ais_demod_new(on_sentence, NULL);
    if (argc > 2) ais_demod_set_ppm(d, atof(argv[2]));
    static uint8_t buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ais_demod_feed(d, buf, n);
        sample_pos += n / 2;
    }
    fclose(f);
    ais_demod_stats_t st;
    ais_demod_get_stats(d, &st);
    printf("packets: A %u, B %u; bad CRC %u; noise A %.1f B %.1f dBFS; correction %+.0f Hz (%+.2f ppm)\n",
           (unsigned)st.packets[0], (unsigned)st.packets[1], (unsigned)st.bad_crc, st.noise_dbfs[0], st.noise_dbfs[1],
           st.freq_corr_hz, st.ppm);
    ais_demod_free(d);
    return st.packets[0] + st.packets[1] ? 0 : 1;
}
