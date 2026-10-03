// Decode AIS NMEA lines from stdin and print one tab-separated line per report:
//   mmsi type class fields lat lon sog cog heading shiptype a b c d name callsign destination
// Also handy for checking what the display will make of a captured feed.

#include <stdio.h>
#include <string.h>

#include "ais.h"

int main(void)
{
    ais_parser_t p;
    ais_parser_init(&p);
    char line[512];
    uint32_t t = 0;
    while (fgets(line, sizeof(line), stdin)) {
        ais_report_t r;
        if (ais_parse_line(&p, line, t += 10, &r) != AIS_RESULT_REPORT) continue;
        printf("%u\t%u\t%s\t%u\t%.6f\t%.6f\t%.1f\t%.1f\t%u\t%u\t%u\t%u\t%u\t%u\t%s\t%s\t%s\n", r.mmsi, r.msg_type,
               ais_class_str(r.cls), r.fields, r.lat, r.lon, r.sog_kn, r.cog_deg, r.heading, r.ship_type, r.dim_a,
               r.dim_b, r.dim_c, r.dim_d, r.name, r.callsign, r.destination);
    }
    fprintf(stderr, "lines %u reports %u ignored %u bad_checksum %u unsupported %u\n", p.stats.lines, p.stats.reports,
            p.stats.ignored, p.stats.bad_checksum, p.stats.unsupported);
    return 0;
}
