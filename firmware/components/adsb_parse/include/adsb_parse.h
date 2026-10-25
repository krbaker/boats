// dump1090's aircraft.json (dump1090-fa, readsb / tar1090, dump1090-mutability):
// the aircraft it's tracking, read straight from the text without building a
// JSON tree, so a busy sky doesn't mean thousands of small allocations.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Which fields an aircraft reported (adsb_aircraft_t.fields).
#define ADSB_F_POS      (1u << 0)
#define ADSB_F_ALT      (1u << 1)   // alt_ft (barometric, else geometric)
#define ADSB_F_GROUND   (1u << 2)   // on the ground (alt_ft not set)
#define ADSB_F_GS       (1u << 3)
#define ADSB_F_TRACK    (1u << 4)
#define ADSB_F_VRATE    (1u << 5)
#define ADSB_F_SQUAWK   (1u << 6)

// The emergency/priority status an aircraft broadcasts ("emergency" field).
typedef enum {
    ADSB_EMERG_NONE, ADSB_EMERG_GENERAL, ADSB_EMERG_LIFEGUARD, ADSB_EMERG_MINFUEL, ADSB_EMERG_NORDO,
    ADSB_EMERG_UNLAWFUL, ADSB_EMERG_DOWNED, ADSB_EMERG_RESERVED,
} adsb_emerg_t;

// What an aircraft's emitter category says it is, for its icon.
typedef enum {
    ADSB_KIND_UNKNOWN,      // no category: a generic aeroplane
    ADSB_KIND_LIGHT,        // A1: under 15,500 lb (light single / twin)
    ADSB_KIND_SMALL,        // A2: 15,500 - 75,000 lb (business jet, regional turboprop)
    ADSB_KIND_LARGE,        // A3: 75,000 - 300,000 lb (airliner)
    ADSB_KIND_HIGH_VORTEX,  // A4: B757
    ADSB_KIND_HEAVY,        // A5: over 300,000 lb (wide-body, jumbo)
    ADSB_KIND_FAST,         // A6: high performance (military jet)
    ADSB_KIND_ROTOR,        // A7: helicopter
    ADSB_KIND_GLIDER,       // B1
    ADSB_KIND_BALLOON,      // B2: lighter than air
    ADSB_KIND_PARACHUTE,    // B3: skydiver
    ADSB_KIND_ULTRALIGHT,   // B4: ultralight, hang glider, paraglider
    ADSB_KIND_UAV,          // B6
    ADSB_KIND_SURFACE,      // C1-C7: airport vehicles and obstacles, not aircraft
} adsb_kind_t;

typedef struct {
    uint32_t addr;          // 24-bit ICAO address (or the non-ICAO address, see non_icao)
    bool non_icao;          // "~" in front of the hex: TIS-B / anonymous, not a real ICAO address
    char flight[9];         // call sign, trimmed; "" if none
    char type[5];           // ICAO type designator ("B738"), when the receiver has a database; "" if not
    char reg[12];           // registration, likewise
    uint8_t category;       // 0xA0-0xD7 ("A3" = 0xA3); 0 if not sent
    uint8_t emergency;      // adsb_emerg_t
    uint16_t squawk;        // as its four digits read in decimal, e.g. 7700
    uint32_t fields;        // ADSB_F_*
    double lat, lon;
    int32_t alt_ft;
    float gs_kn, track_deg;
    int16_t vrate_fpm;
    float seen_pos_s;       // age of the position when the file was written
    float seen_s;           // age of the last message of any kind
} adsb_aircraft_t;

// Called for each aircraft in the file, in order. Return false to stop.
typedef bool (*adsb_cb_t)(void *ctx, const adsb_aircraft_t *a);

// Reads json (len bytes, need not be NUL-terminated). *now gets the file's
// "now" (Unix seconds) if it has one before the aircraft, else 0. Returns how
// many aircraft were passed to cb, or -1 if this isn't an aircraft.json. A
// file cut short gives the aircraft before the cut.
int adsb_parse(const char *json, size_t len, adsb_cb_t cb, void *ctx, double *now);

adsb_kind_t adsb_kind(const adsb_aircraft_t *a);

// Squawking 7500 (hijack), 7600 (radio failure) or 7700 (emergency), or
// broadcasting one of those emergencies.
bool adsb_is_emergency(const adsb_aircraft_t *a);

// "Emergency", "Radio failure", "Hijack", ... for the squawk / emergency, or NULL if none.
const char *adsb_emergency_str(const adsb_aircraft_t *a);

// "Large (75,000-300,000 lb)" etc. for a category, "" if none.
const char *adsb_category_str(uint8_t category);
