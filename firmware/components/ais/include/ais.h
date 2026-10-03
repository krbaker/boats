// AIS NMEA 0183 parsing and decoding.
//
// Pure C with no ESP-IDF dependencies so it can be unit-tested on the host.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AIS_CLASS_UNKNOWN = 0,
    AIS_CLASS_A,
    AIS_CLASS_B,
} ais_class_t;

// Bits in ais_report_t.fields marking which values are present.
#define AIS_F_POS       (1u << 0)
#define AIS_F_SOG       (1u << 1)
#define AIS_F_COG       (1u << 2)
#define AIS_F_HEADING   (1u << 3)
#define AIS_F_NAVSTAT   (1u << 4)
#define AIS_F_NAME      (1u << 5)
#define AIS_F_CALLSIGN  (1u << 6)
#define AIS_F_SHIPTYPE  (1u << 7)
#define AIS_F_DIM       (1u << 8)
#define AIS_F_DEST      (1u << 9)

typedef struct {
    uint32_t mmsi;
    uint8_t msg_type;
    ais_class_t cls;
    uint32_t fields;        // AIS_F_* bits

    double lat, lon;        // degrees
    float sog_kn;           // knots
    float cog_deg;          // degrees true
    uint16_t heading;       // degrees true
    uint8_t nav_status;     // 0-15
    uint8_t ship_type;      // ITU 0-99
    uint16_t dim_a, dim_b;  // metres from reference point to bow / stern
    uint8_t dim_c, dim_d;   // metres to port / starboard
    char name[21];
    char callsign[8];
    char destination[21];
} ais_report_t;

typedef enum {
    AIS_RESULT_REPORT = 0,  // *out filled with a decoded report
    AIS_RESULT_PENDING,     // fragment stored, waiting for the rest
    AIS_RESULT_IGNORED,     // not an AIS sentence (or unknown format)
    AIS_RESULT_BAD_CHECKSUM,
    AIS_RESULT_UNSUPPORTED, // valid AIS, but a message type we don't decode
} ais_result_t;

typedef struct {
    uint32_t lines;
    uint32_t reports;
    uint32_t ignored;
    uint32_t bad_checksum;
    uint32_t unsupported;
} ais_stats_t;

#define AIS_MAX_FRAGMENTS   9
#define AIS_FRAG_SLOTS      8
#define AIS_FRAG_TIMEOUT_MS 2000
#define AIS_MAX_PAYLOAD     (AIS_MAX_FRAGMENTS * 64)

typedef struct {
    bool in_use;
    int seq_id;             // -1 when the sentence had no sequence id
    char channel;
    uint8_t count;
    uint16_t received;      // bitmask of fragment numbers received
    uint32_t started_ms;
    int fill_bits;          // from the last fragment
    char part[AIS_MAX_FRAGMENTS][64];
} ais_frag_slot_t;

typedef struct {
    ais_frag_slot_t slots[AIS_FRAG_SLOTS];
    ais_stats_t stats;
} ais_parser_t;

void ais_parser_init(ais_parser_t *p);

// Feed one line of text (with or without a trailing CR/LF). Anything before
// the "!xxVDM"/"!xxVDO" sentence start (timestamps, NMEA 4 TAG blocks, ...)
// and anything after the checksum is ignored.
ais_result_t ais_parse_line(ais_parser_t *p, const char *line, uint32_t now_ms, ais_report_t *out);

// Decode an already-assembled armoured payload. Returns AIS_RESULT_REPORT or
// AIS_RESULT_UNSUPPORTED.
ais_result_t ais_decode_payload(const char *payload, int fill_bits, ais_report_t *out);

// Human-readable strings for display.
const char *ais_ship_type_str(uint8_t type);
const char *ais_nav_status_str(uint8_t status);
const char *ais_class_str(ais_class_t cls);

#ifdef __cplusplus
}
#endif
