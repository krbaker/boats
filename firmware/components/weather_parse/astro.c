// Sun and moon: see astro.h.

#include "astro.h"

#include <math.h>
#include <stdlib.h>

#define DEG (3.14159265358979323846 / 180.0)
#define RISE_SET_ALT (-0.833)   // the sun's upper edge on the horizon, with refraction
#define DELTA_T_S 69.0          // TT - UTC around 2026, for the moon phase times

static double norm360(double a)
{
    a = fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}

static double julian_day(time_t t)
{
    return (double)t / 86400.0 + 2440587.5;
}

static time_t from_julian_day(double jd)
{
    return (time_t)llround((jd - 2440587.5) * 86400.0);
}

// The sun's apparent ecliptic longitude, and its right ascension and
// declination, in degrees (Meeus ch. 25, low precision).
static void sun_position(double jd, double *lon, double *ra, double *dec)
{
    double T = (jd - 2451545.0) / 36525.0;
    double L0 = norm360(280.46646 + T * (36000.76983 + T * 0.0003032));
    double M = 357.52911 + T * (35999.05029 - 0.0001537 * T);
    double C = sin(M * DEG) * (1.914602 - T * (0.004817 + 0.000014 * T)) + sin(2 * M * DEG) * (0.019993 - 0.000101 * T) +
               sin(3 * M * DEG) * 0.000289;
    double omega = 125.04 - 1934.136 * T;
    double lambda = L0 + C - 0.00569 - 0.00478 * sin(omega * DEG);
    double eps0 = 23.0 + (26.0 + (21.448 - T * (46.815 + T * (0.00059 - T * 0.001813))) / 60.0) / 60.0;
    double eps = eps0 + 0.00256 * cos(omega * DEG);
    *lon = norm360(lambda);
    *ra = norm360(atan2(cos(eps * DEG) * sin(lambda * DEG), cos(lambda * DEG)) / DEG);
    *dec = asin(sin(eps * DEG) * sin(lambda * DEG)) / DEG;
}

double sun_elevation(double lat, double lon, time_t t)
{
    double jd = julian_day(t), sl, ra, dec;
    sun_position(jd, &sl, &ra, &dec);
    double T = (jd - 2451545.0) / 36525.0;
    double gmst = norm360(280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T);
    double ha = (gmst + lon - ra) * DEG;
    return asin(sin(lat * DEG) * sin(dec * DEG) + cos(lat * DEG) * cos(dec * DEG) * cos(ha)) / DEG;
}

bool sun_is_up(double lat, double lon, time_t t)
{
    return sun_elevation(lat, lon, t) > RISE_SET_ALT;
}

bool sun_event(double lat, double lon, time_t t0, time_t t1, bool rise, time_t *out)
{
    // Step in 10 minutes looking for the crossing, then halve down to a second.
    const time_t step = 600;
    double prev = sun_elevation(lat, lon, t0) - RISE_SET_ALT;
    for (time_t a = t0; a < t1; a += step) {
        time_t b = a + step < t1 ? a + step : t1;
        double next = sun_elevation(lat, lon, b) - RISE_SET_ALT;
        if ((rise && prev <= 0 && next > 0) || (!rise && prev > 0 && next <= 0)) {
            time_t lo = a, hi = b;
            while (hi - lo > 1) {
                time_t mid = lo + (hi - lo) / 2;
                double m = sun_elevation(lat, lon, mid) - RISE_SET_ALT;
                if ((m > 0) == rise) hi = mid;
                else lo = mid;
            }
            *out = hi;
            return true;
        }
        prev = next;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Moon
// ---------------------------------------------------------------------------

void moon_at(time_t t, moon_t *out)
{
    double jd = julian_day(t + (time_t)DELTA_T_S);
    double T = (jd - 2451545.0) / 36525.0;
    // Mean elongation, the sun's and the moon's anomalies, argument of latitude (Meeus ch. 47).
    double D = norm360(297.8501921 + 445267.1114034 * T - 0.0018819 * T * T);
    double M = norm360(357.5291092 + 35999.0502909 * T - 0.0001536 * T * T);
    double Mp = norm360(134.9633964 + 477198.8675055 * T + 0.0087414 * T * T);
    double F = norm360(93.2720950 + 483202.0175233 * T - 0.0036539 * T * T);
    double Lp = norm360(218.3164477 + 481267.88123421 * T - 0.0015786 * T * T);
    double E = 1.0 - 0.002516 * T - 0.0000074 * T * T;
    // The largest periodic terms of the moon's longitude (1e-6 degrees).
    static const struct { signed char d, m, mp, f; int l; } terms[] = {
        {0, 0, 1, 0, 6288774}, {2, 0, -1, 0, 1274027}, {2, 0, 0, 0, 658314}, {0, 0, 2, 0, 213618},
        {0, 1, 0, 0, -185116}, {0, 0, 0, 2, -114332}, {2, 0, -2, 0, 58793},  {2, -1, -1, 0, 57066},
        {2, 0, 1, 0, 53322},   {2, -1, 0, 0, 45758},  {0, 1, -1, 0, -40923}, {1, 0, 0, 0, -34720},
        {0, 1, 1, 0, -30383},  {2, 0, 0, -2, 15327},  {0, 0, 1, 2, -12528},  {0, 0, 1, -2, 10980},
        {4, 0, -1, 0, 10675},  {0, 0, 3, 0, 10034},   {4, 0, -2, 0, 8548},   {2, 1, -1, 0, -7888},
        {2, 1, 0, 0, -6766},   {1, 0, -1, 0, -5163},  {1, 1, 0, 0, 4987},    {2, -1, 1, 0, 4036},
    };
    double sum = 0;
    for (unsigned i = 0; i < sizeof(terms) / sizeof(terms[0]); i++) {
        double arg = terms[i].d * D + terms[i].m * M + terms[i].mp * Mp + terms[i].f * F;
        double e = abs(terms[i].m) == 1 ? E : abs(terms[i].m) == 2 ? E * E : 1.0;
        sum += terms[i].l * e * sin(arg * DEG);
    }
    double moon_lon = norm360(Lp + sum / 1e6);
    double sun_lon, ra, dec;
    sun_position(jd, &sun_lon, &ra, &dec);

    // Phase angle (Meeus 48.4), and the lit fraction from it.
    double i = 180.0 - D - 6.289 * sin(Mp * DEG) + 2.100 * sin(M * DEG) - 1.274 * sin((2 * D - Mp) * DEG) -
               0.658 * sin(2 * D * DEG) - 0.214 * sin(2 * Mp * DEG) - 0.110 * sin(D * DEG);
    out->illum = (1.0 + cos(i * DEG)) / 2.0;
    out->elongation = norm360(moon_lon - sun_lon);
    out->waxing = out->elongation < 180.0;

    // Eight phases; the four principal ones cover about a day (+/- 6 degrees).
    double el = out->elongation;
    if (el < 6 || el >= 354) out->phase = MOON_NEW;
    else if (el < 84) out->phase = MOON_WAXING_CRESCENT;
    else if (el < 96) out->phase = MOON_FIRST_QUARTER;
    else if (el < 174) out->phase = MOON_WAXING_GIBBOUS;
    else if (el < 186) out->phase = MOON_FULL;
    else if (el < 264) out->phase = MOON_WANING_GIBBOUS;
    else if (el < 276) out->phase = MOON_LAST_QUARTER;
    else out->phase = MOON_WANING_CRESCENT;
}

const char *moon_phase_name(moon_phase_t p)
{
    static const char *names[] = {"New moon", "Waxing crescent", "First quarter", "Waxing gibbous",
                                  "Full moon", "Waning gibbous", "Last quarter",  "Waning crescent"};
    return (unsigned)p < 8 ? names[p] : "?";
}

// The new (or full, k + 0.5) moon numbered k from January 2000 (Meeus ch. 49).
static double phase_jde(double k, bool full)
{
    double T = k / 1236.85;
    double jde = 2451550.09766 + 29.530588861 * k + 0.00015437 * T * T - 0.000000150 * T * T * T;
    double E = 1.0 - 0.002516 * T - 0.0000074 * T * T;
    double M = (2.5534 + 29.10535670 * k - 0.0000014 * T * T) * DEG;
    double Mp = (201.5643 + 385.81693528 * k + 0.0107582 * T * T) * DEG;
    double F = (160.7108 + 390.67050284 * k - 0.0016118 * T * T) * DEG;
    double om = (124.7746 - 1.56375588 * k + 0.0020672 * T * T) * DEG;
    double c = full ? -0.40614 * sin(Mp) + 0.17302 * E * sin(M) + 0.01614 * sin(2 * Mp)
                    : -0.40720 * sin(Mp) + 0.17241 * E * sin(M) + 0.01608 * sin(2 * Mp);
    c += (full ? 0.01043 : 0.01039) * sin(2 * F) + (full ? 0.00734 : 0.00739) * E * sin(Mp - M) -
         (full ? 0.00515 : 0.00514) * E * sin(Mp + M) + (full ? 0.00209 : 0.00208) * E * E * sin(2 * M) -
         0.00111 * sin(Mp - 2 * F) - 0.00057 * sin(Mp + 2 * F) + 0.00056 * E * sin(2 * Mp + M) - 0.00042 * sin(3 * Mp) +
         0.00042 * E * sin(M + 2 * F) + 0.00038 * E * sin(M - 2 * F) - 0.00024 * E * sin(2 * Mp - M) -
         0.00017 * sin(om);
    return jde + c;
}

time_t moon_next(time_t t, bool full)
{
    double jd = julian_day(t);
    double k = floor((jd - 2451550.09766) / 29.530588861) - 1;
    for (int i = 0; i < 4; i++, k += 1) {
        double kk = full ? k + 0.5 : k;
        time_t when = from_julian_day(phase_jde(kk, full)) - (time_t)DELTA_T_S;
        if (when > t) return when;
    }
    return 0;
}
