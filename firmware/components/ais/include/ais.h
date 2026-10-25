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
    AIS_CLASS_ATON,     // aid to navigation: buoy, beacon, light (type 21)
    AIS_CLASS_SAR,      // search-and-rescue aircraft (type 9)
    AIS_CLASS_BASE,     // a station's clock (type 4 base station, type 11 UTC response): not a target
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
#define AIS_F_ATON      (1u << 10)  // aton_type, aton_flags
#define AIS_F_ALT       (1u << 11)  // alt_m
#define AIS_F_UTC       (1u << 12)  // utc
#define AIS_F_TEXT      (1u << 13)  // text (a safety broadcast, type 14)

// ais_report_t.aton_flags
#define AIS_ATON_VIRTUAL    0x01    // no physical mark there: only the AIS signal
#define AIS_ATON_OFF_POS    0x02    // a floating mark that has drifted off its charted position

// Aid-to-navigation types (ITU-R M.1371 table 74), ais_report_t.aton_type.
// 9-19 are fixed beacons, 20-30 floating buoys, of the same kinds in order.
#define AIS_ATON_BEACON_N       9
#define AIS_ATON_BUOY_N         20
#define AIS_ATON_LIGHT_VESSEL   31

// Weather and water readings from a station (type 8: the international
// meteorological/hydrological message, DAC 1 FI 31, or the US environmental
// message, DAC 367 FI 33). Bits in ais_met_t.fields mark what's there.
#define AIS_MET_POS         (1u << 0)
#define AIS_MET_NAME        (1u << 1)
#define AIS_MET_TIME        (1u << 2)   // day, hour, minute
#define AIS_MET_WIND        (1u << 3)   // wind_kn, wind_dir
#define AIS_MET_GUST        (1u << 4)   // gust_kn (gust_dir if < 360)
#define AIS_MET_AIR_TEMP    (1u << 5)
#define AIS_MET_DEW         (1u << 6)
#define AIS_MET_HUMIDITY    (1u << 7)
#define AIS_MET_PRESSURE    (1u << 8)   // pressure_hpa, pressure_trend
#define AIS_MET_VIS         (1u << 9)
#define AIS_MET_WATER_LEVEL (1u << 10)  // water_m, water_trend
#define AIS_MET_CURRENT     (1u << 11)
#define AIS_MET_WAVES       (1u << 12)  // wave_m (wave_s, wave_dir if known)
#define AIS_MET_SWELL       (1u << 13)
#define AIS_MET_WATER_TEMP  (1u << 14)
#define AIS_MET_SALINITY    (1u << 15)
#define AIS_MET_SEA_STATE   (1u << 16)

#define AIS_MET_SITE_IMO    0xff        // the international message: one station, no site number
#define AIS_MET_SITES       4           // sites one message can carry (the US message's reports, merged)

typedef enum { AIS_TREND_STEADY, AIS_TREND_FALLING, AIS_TREND_RISING, AIS_TREND_UNKNOWN } ais_trend_t;

typedef struct {
    uint8_t site;           // the US message's site id (0-127), or AIS_MET_SITE_IMO
    uint32_t fields;        // AIS_MET_*
    double lat, lon;
    char name[15];
    uint8_t day, hour, minute;  // UTC of the reading
    float wind_kn, gust_kn;
    uint16_t wind_dir, gust_dir;    // degrees true, where it blows from; gust_dir 360 = not given
    float air_c, dew_c;
    uint8_t humidity;       // %
    uint16_t pressure_hpa;
    uint8_t pressure_trend, water_trend;    // ais_trend_t
    float vis_nm;
    float water_m;          // above the station's datum
    float current_kn;
    uint16_t current_dir;   // degrees true, where it flows to (360 = not given)
    float wave_m, swell_m;
    uint8_t wave_s, swell_s;        // period; 0 = not given
    uint16_t wave_dir, swell_dir;   // 360 = not given
    float water_c, salinity;        // salinity in parts per thousand
    uint8_t sea_state;      // Beaufort 0-12
} ais_met_t;

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
    uint8_t aton_type;      // AIS_ATON_*: 0-31
    uint8_t aton_flags;
    uint16_t alt_m;         // SAR aircraft altitude, metres (4094 = that or higher)
    struct {
        uint16_t year;
        uint8_t month, day, hour, minute, second;
    } utc;                  // a base station's clock
    char text[162];         // safety broadcast (type 14): up to 161 characters
    uint8_t nmet;           // type 8 weather stations: how many of met[] are filled
    ais_met_t met[AIS_MET_SITES];
} ais_report_t;

// What a safety broadcast is (ais_safety_kind).
typedef enum {
    AIS_SAFETY_TEXT,        // a notice or free text
    AIS_SAFETY_TEST,        // an emergency beacon's test ("SART TEST", "MOB TEST", "EPIRB TEST")
    AIS_SAFETY_DISTRESS,    // an emergency beacon going off ("SART ACTIVE", ...), or "MAYDAY"
} ais_safety_t;

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
    uint32_t by_type[28];   // complete messages by type, 1-27 (decoded or not); [0]: too short to tell
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
const char *ais_aton_type_str(uint8_t type);
const char *ais_msg_type_str(uint8_t type);   // "Position report (Class A)", ...
bool ais_msg_type_used(uint8_t type);         // one ais_decode_payload() decodes

// A safety broadcast's kind, from its sender (emergency beacons have MMSIs
// 970..., 972..., 974...) and text.
ais_safety_t ais_safety_kind(uint32_t mmsi, const char *text);

#ifdef __cplusplus
}
#endif
