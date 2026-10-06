// Host tests for the weather parsers (components/weather_parse), on real
// responses saved in fixtures/weather/. Run by run_tests.sh.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astro.h"
#include "weather_parse.h"
#include "weatherlink_parse.h"

static int failures;

#define NEAR(a, b, eps) (fabs((double)(a) - (double)(b)) <= (eps))

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

static char *load(const char *name)
{
    char path[256];
    snprintf(path, sizeof(path), "fixtures/weather/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL: can't open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc(n + 1);
    buf[fread(buf, 1, n, f)] = '\0';
    fclose(f);
    return buf;
}

static void test_stations(void)
{
    char *j = load("weatherlink_stations.json");
    wl_station_t s[4];
    int n = wl_parse_stations(j, s, 4);
    CHECK(n == 2);
    CHECK(s[0].id == 192231 && strcmp(s[0].product, "6313") == 0 && strcmp(s[0].name, "Home") == 0);
    CHECK(s[1].id == 192412 && strcmp(s[1].product, "6100") == 0 && strcmp(s[1].name, "Home (Live)") == 0);
    CHECK(wl_parse_stations(j, s, 1) == 1); // capped
    free(j);
}

static void test_transmitters(void)
{
    wl_tx_t t[4];
    // The Live hears two outdoor suites; its indoor sensor and barometer aren't listed.
    char *j = load("weatherlink_current_live.json");
    CHECK(wl_parse_transmitters(j, t, 4) == 2);
    CHECK(t[0].tx_id == 6 && t[0].has_temp && t[0].has_wind);
    CHECK(t[0].temp_f > 57.2f && t[0].temp_f < 57.4f && t[0].wind_dir == 329);
    CHECK(t[1].tx_id == 1 && t[1].temp_f > 55.9f && t[1].temp_f < 56.1f && t[1].wind_dir == 257);
    free(j);
    // The Console's record layout is different but holds the same fields.
    j = load("weatherlink_current_console.json");
    CHECK(wl_parse_transmitters(j, t, 4) == 1);
    CHECK(t[0].tx_id == 6 && t[0].wind_dir == 319 && t[0].wind_mph == 9);
    free(j);
}

static void test_errors(void)
{
    char *j = load("weatherlink_error_401.json");
    char msg[64];
    wl_station_t s[2];
    wl_tx_t t[2];
    CHECK(wl_parse_stations(j, s, 2) == -1);
    CHECK(wl_parse_transmitters(j, t, 2) == -1);
    CHECK(wl_parse_error(j, msg, sizeof(msg)) && strcmp(msg, "Invalid API Key/API Secret.") == 0);
    free(j);
    CHECK(wl_parse_stations("not json", s, 2) == -1);
    CHECK(!wl_parse_error("<html>", msg, sizeof(msg)));
}

static time_t utc(int y, int mo, int d, int h, int mi)
{
    char iso[32];
    snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:00Z", y, mo, d, h, mi);
    return wx_parse_iso8601(iso);
}

static void test_current(void)
{
    wl_current_t c;
    char *j = load("weatherlink_current_live.json");
    CHECK(wl_parse_current(j, 6, &c));
    CHECK(c.when == 1791248432 && c.has_wind && c.wind_mph == 2 && c.wind_dir == 329);
    CHECK(NEAR(c.wind_avg10_mph, 3.87, 0.01) && c.wind_dir_avg10 == 322 && c.gust10_mph == 12);
    CHECK(NEAR(c.temp_f, 57.3, 0.01) && NEAR(c.hum_pct, 51.1, 0.01) && NEAR(c.dew_f, 39.4, 0.01));
    CHECK(c.has_rain && c.rain_rate_in_hr == 0 && c.rain_today_in == 0);
    CHECK(c.has_bar && NEAR(c.bar_inhg, 29.901, 0.001) && NEAR(c.bar_trend_inhg, 0.08, 0.001));
    CHECK(wl_parse_current(j, 1, &c) && c.wind_dir == 257 && NEAR(c.temp_f, 56, 0.01)); // the other suite
    CHECK(wl_parse_current(j, 0, &c) && c.wind_dir == 329);                            // 0: the first
    CHECK(!wl_parse_current(j, 9, &c));                                                // not heard
    free(j);
    // The Console names today's rain differently.
    j = load("weatherlink_current_console.json");
    CHECK(wl_parse_current(j, 6, &c) && c.wind_mph == 9 && c.has_rain && c.has_bar && NEAR(c.bar_inhg, 29.896, 0.001));
    free(j);
    char rainy[] = "{\"sensors\":[{\"data\":[{\"tx_id\":6,\"temp\":50,\"rain_rate_last_in\":0.12,"
                   "\"rainfall_day_in\":0.5,\"ts\":100}]}]}";
    CHECK(wl_parse_current(rainy, 6, &c) && NEAR(c.rain_rate_in_hr, 0.12, 1e-4) && NEAR(c.rain_today_in, 0.5, 1e-4));
    CHECK(!c.has_wind && !c.has_bar);
}

static void test_marine_forecast(void)
{
    static marine_forecast_t f;
    char *t = load("nws_marine_PZZ135.txt");
    CHECK(mf_parse(t, &f));
    CHECK(strcmp(f.zone, "PZZ135") == 0 && strcmp(f.zone_name, "Puget Sound and Hood Canal") == 0);
    CHECK(strcmp(f.issued, "133 PM PDT Mon Oct 5 2026") == 0);
    CHECK(f.expires == utc(2026, 10, 6, 9, 45));
    CHECK(f.nheadlines == 0 && f.nperiods == 11);
    CHECK(strcmp(f.period[0].name, "TONIGHT") == 0);
    CHECK(strcmp(f.period[0].text, "N wind around 5 kt. Waves around 2 ft or less. Areas of dense fog after midnight.") == 0);
    CHECK(strcmp(f.period[2].name, "TUE NIGHT") == 0);
    CHECK(strcmp(f.period[10].name, "SAT NIGHT") == 0 && strcmp(f.period[10].text, "N wind 5 to 10 kt. Waves around 2 ft or less.") == 0);
    free(t);
    // A zone with an advisory headline above the periods.
    t = load("nws_marine_ANZ535.txt");
    CHECK(mf_parse(t, &f));
    CHECK(strcmp(f.zone_name, "Tidal Potomac from Key Bridge to Indian Head") == 0);
    CHECK(f.nheadlines == 1 && strcmp(f.headline[0], "SMALL CRAFT ADVISORY IN EFFECT UNTIL 10 PM EDT THIS EVENING") == 0);
    CHECK(f.nperiods == 9 && strcmp(f.period[0].text, "N winds 5 to 10 kt. Waves 1 ft.") == 0);
    free(t);
    // A headline wrapped over two lines.
    const char *wrapped = "PZZ135-061000-\nPuget Sound and Hood Canal-\n300 AM PDT Tue Oct 6 2026\n\n"
                          "...GALE WARNING IN EFFECT FROM 2 PM THIS AFTERNOON THROUGH\nWEDNESDAY MORNING...\n\n"
                          ".TODAY...S wind 25 to 35 kt.\n$$\n";
    CHECK(mf_parse(wrapped, &f) && f.nheadlines == 1);
    CHECK(strcmp(f.headline[0], "GALE WARNING IN EFFECT FROM 2 PM THIS AFTERNOON THROUGH WEDNESDAY MORNING") == 0);
    CHECK(f.nperiods == 1 && strcmp(f.period[0].text, "S wind 25 to 35 kt.") == 0);
    CHECK(!mf_parse("<html>Not Found</html>", &f));
}

static void test_alerts(void)
{
    static wx_alert_t a[WX_MAX_ALERTS];
    char *j = load("nws_alerts_none.json");
    CHECK(wx_parse_alerts(j, a, WX_MAX_ALERTS) == 0);
    free(j);
    j = load("nws_alerts_marine.json");
    CHECK(wx_parse_alerts(j, a, WX_MAX_ALERTS) == 1);
    CHECK(strcmp(a[0].event, "Small Craft Advisory") == 0 && a[0].flag);
    CHECK(a[0].ends == utc(2026, 10, 6, 2, 0)); // 10 PM EDT
    CHECK(strncmp(a[0].description, "* WHAT...North winds 10 to 15 kt", 32) == 0);
    CHECK(strstr(a[0].description, "Inexperienced mariners") != NULL); // the instruction is appended
    free(j);
    // A statement is listed but doesn't flag; a cancellation is dropped.
    const char *mixed = "{\"features\":[{\"properties\":{\"status\":\"Actual\",\"messageType\":\"Alert\","
                        "\"event\":\"Marine Weather Statement\"}},{\"properties\":{\"status\":\"Actual\","
                        "\"messageType\":\"Cancel\",\"event\":\"Gale Warning\"}},{\"properties\":{\"status\":"
                        "\"Test\",\"event\":\"Gale Warning\"}}]}";
    CHECK(wx_parse_alerts(mixed, a, WX_MAX_ALERTS) == 1 && !a[0].flag);
    CHECK(wx_parse_alerts("{}", a, WX_MAX_ALERTS) == -1);
}

static void test_observation(void)
{
    wx_obs_t o;
    char *j = load("nws_observation_KBFI.json");
    CHECK(wx_parse_observation(j, &o));
    CHECK(o.when == utc(2026, 10, 6, 0, 50) && strcmp(o.text, "Clear") == 0);
    CHECK(o.cloud == CLOUD_CLEAR && o.precip == PRECIP_NONE);
    free(j);
    const char *snowy = "{\"properties\":{\"timestamp\":\"2026-01-10T15:53:00+00:00\",\"textDescription\":"
                        "\"Light Snow and Fog/Mist\",\"cloudLayers\":[{\"amount\":\"BKN\"},{\"amount\":\"OVC\"},"
                        "{\"amount\":\"SCT\"}],\"presentWeather\":[{\"weather\":\"snow\"},{\"weather\":\"fog_mist\"}]}}";
    CHECK(wx_parse_observation(snowy, &o) && o.cloud == CLOUD_OVERCAST && o.precip == PRECIP_SNOW);
    const char *rainy = "{\"properties\":{\"timestamp\":\"2026-01-10T15:53:00+00:00\",\"textDescription\":"
                        "\"Light Rain\",\"cloudLayers\":[{\"amount\":\"SCT\"}],\"presentWeather\":[]}}";
    CHECK(wx_parse_observation(rainy, &o) && o.cloud == CLOUD_SCATTERED && o.precip == PRECIP_RAIN);

    char url[128], id[16];
    j = load("nws_points.json");
    CHECK(wx_parse_points(j, url, sizeof(url)) && strcmp(url, "https://api.weather.gov/gridpoints/SEW/124,68/stations") == 0);
    free(j);
    j = load("nws_stations.json");
    CHECK(wx_parse_first_station(j, id, sizeof(id)) && strcmp(id, "KBFI") == 0);
    free(j);
}

static void test_buoy(void)
{
    buoy_t b;
    char *t = load("ndbc_46088.txt");
    CHECK(buoy_parse(t, &b));
    time_t t50 = utc(2026, 10, 6, 0, 50);
    CHECK(b.latest == t50);
    CHECK(b.wave_ft.ok && NEAR(b.wave_ft.v, 0.98, 0.01) && b.wave_ft.when == t50);
    CHECK(b.dom_period_s.v == 3 && NEAR(b.avg_period_s.v, 2.9, 0.01) && b.wave_dir.v == 227);
    CHECK(b.wind_dir.v == 220 && NEAR(b.wind_mph.v, 15.66, 0.01) && NEAR(b.gust_mph.v, 17.9, 0.01));
    CHECK(NEAR(b.pressure_inhg.v, 30.12, 0.01) && NEAR(b.air_f.v, 54.5, 0.01) && NEAR(b.dew_f.v, 53.78, 0.01));
    // Missing from the newest row: taken from older ones, with their times.
    CHECK(b.water_f.ok && NEAR(b.water_f.v, 52.7, 0.01) && b.water_f.when == utc(2026, 10, 6, 0, 40));
    CHECK(b.pressure_trend_inhg.ok && NEAR(b.pressure_trend_inhg.v, -0.0266, 0.001) &&
          b.pressure_trend_inhg.when == utc(2026, 10, 6, 0, 0));
    free(t);
    // A buoy without waves.
    const char *nowaves = "#YY  MM DD hh mm WDIR WSPD GST  WVHT   DPD   APD MWD   PRES\n"
                          "#yr  mo dy hr mn degT m/s  m/s     m   sec   sec degT   hPa\n"
                          "2026 10 06 00 50 220  7.0  8.0    MM    MM    MM  MM 1019.9\n";
    CHECK(buoy_parse(nowaves, &b) && !b.wave_ft.ok && !b.wave_dir.ok && b.wind_mph.ok);
    CHECK(!buoy_parse("<html>", &b));
}

static void test_icon(void)
{
    const double lat = 47.6005, lon = -122.3550;
    time_t noon = utc(2026, 10, 6, 20, 0), night = utc(2026, 10, 6, 8, 0); // 1 PM and 1 AM PDT
    wx_obs_t o = {.when = noon - 1800, .cloud = CLOUD_CLEAR};
    wx_icon_t ic;
    wx_icon_decide(lat, lon, noon, &o, 0, false, &ic);
    CHECK(!ic.night && ic.cloud == 0 && !ic.rain && !ic.snow && !ic.alert);
    o.cloud = CLOUD_BROKEN;
    wx_icon_decide(lat, lon, noon, &o, 0, true, &ic);
    CHECK(ic.cloud == 2 && ic.alert);
    // At night: the moon, waning crescent (17-24% lit) that morning.
    o.when = night - 1800;
    o.cloud = CLOUD_SCATTERED;
    wx_icon_decide(lat, lon, night, &o, -1, false, &ic);
    CHECK(ic.night && !ic.moon_waxing && ic.moon_illum > 0.15 && ic.moon_illum < 0.3 && ic.cloud == 1);
    // The station's rain gauge brings rain and a cloud; snow from the NWS.
    o.cloud = CLOUD_CLEAR;
    wx_icon_decide(lat, lon, night, &o, 0.05f, false, &ic);
    CHECK(ic.rain && ic.cloud == 2);
    o.precip = PRECIP_SNOW;
    o.cloud = CLOUD_OVERCAST;
    wx_icon_decide(lat, lon, night, &o, 0, false, &ic);
    CHECK(ic.snow && ic.cloud == 3);
    // An observation over 3 hours old is ignored; so is a missing one.
    o.when = night - 4 * 3600;
    wx_icon_decide(lat, lon, night, &o, 0, false, &ic);
    CHECK(!ic.snow && ic.cloud == 0);
    wx_icon_decide(lat, lon, noon, NULL, -1, false, &ic);
    CHECK(!ic.night && ic.cloud == 0);

    CHECK(wx_pressure_trend(0.08f) == TREND_RISING_FAST && wx_pressure_trend(0.03f) == TREND_RISING);
    CHECK(wx_pressure_trend(0.01f) == TREND_STEADY && wx_pressure_trend(-0.019f) == TREND_STEADY);
    CHECK(wx_pressure_trend(-0.02f) == TREND_FALLING && wx_pressure_trend(-0.06f) == TREND_FALLING_FAST);
    CHECK(strcmp(wx_compass(0), "N") == 0 && strcmp(wx_compass(329), "NNW") == 0 && strcmp(wx_compass(348), "NNW") == 0);
    CHECK(strcmp(wx_compass(349), "N") == 0 && strcmp(wx_compass(257), "WSW") == 0 && strcmp(wx_compass(-90), "W") == 0);
}

static void test_iso8601(void)
{
    CHECK(wx_parse_iso8601("1970-01-01T00:00:00Z") == 0 && wx_parse_iso8601("1970-01-01T00:00:01Z") == 1);
    CHECK(wx_parse_iso8601("2026-10-05T22:00:00-04:00") == wx_parse_iso8601("2026-10-06T02:00:00+00:00"));
    CHECK(wx_parse_iso8601("2026-10-06T00:50:00.123+00:00") == utc(2026, 10, 6, 0, 50));
    CHECK(wx_parse_iso8601("yesterday") == 0 && wx_parse_iso8601(NULL) == 0);
}

// Sunrise, sunset and the moon against USNO (fixtures/weather/usno_*.csv).
static void test_astro(void)
{
    FILE *f = fopen("fixtures/weather/usno_sun_moon.csv", "r");
    char line[256];
    int rows = 0;
    double worst_sun = 0, worst_illum = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        int y, mo, d;
        char place[32], rise[8], set[8], phase[32];
        double lat, lon, illum;
        if (sscanf(line, "%d-%d-%d,%31[^,],%lf,%lf,%7[^,],%7[^,],%lf,%31[^\n]", &y, &mo, &d, place, &lat, &lon, rise,
                   set, &illum, phase) != 10) {
            continue;
        }
        rows++;
        time_t day = utc(y, mo, d, 0, 0);
        // USNO lists the events that fall on that UTC date.
        for (int r = 0; r < 2; r++) {
            const char *want = r ? set : rise;
            time_t ev;
            bool got = sun_event(lat, lon, day, day + 86400, r == 0, &ev);
            if (want[0] == '-') {
                CHECK(!got);
                continue;
            }
            int hh, mm;
            sscanf(want, "%d:%d", &hh, &mm);
            double err = got ? fabs((double)(ev - (day + hh * 3600 + mm * 60))) / 60.0 : 1e9;
            if (err > worst_sun) worst_sun = err;
            if (err > 2) printf("FAIL sun %s %s on %d-%02d-%02d off by %.1f min\n", place, r ? "set" : "rise", y, mo, d, err);
            CHECK(err <= 2);
        }
        // USNO's illumination is at 12:00 UT.
        moon_t m;
        moon_at(day + 43200, &m);
        double e = fabs(m.illum * 100 - illum);
        if (e > worst_illum) worst_illum = e;
        CHECK(e <= 2);
        CHECK(m.waxing == (strncmp(phase, "Waxing", 6) == 0 || strcmp(phase, "First Quarter") == 0));
    }
    fclose(f);
    CHECK(rows == 25);

    f = fopen("fixtures/weather/usno_moon_phases_2026.csv", "r");
    double worst_phase = 0;
    int phases = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[32];
        int y, mo, d, hh, mm;
        if (line[0] == '#' || sscanf(line, "%31[^,],%d,%d,%d,%d:%d", name, &y, &mo, &d, &hh, &mm) != 6) continue;
        bool full = strcmp(name, "Full Moon") == 0;
        if (!full && strcmp(name, "New Moon") != 0) continue;
        time_t want = utc(y, mo, d, hh, mm);
        double err = fabs((double)(moon_next(want - 5 * 86400, full) - want)) / 60.0;
        if (err > worst_phase) worst_phase = err;
        CHECK(err <= 5);
        phases++;
        moon_t m;
        moon_at(want, &m);
        CHECK(m.phase == (full ? MOON_FULL : MOON_NEW));
    }
    fclose(f);
    CHECK(phases >= 26);
    // Polar night: no sunrise in Utqiagvik in late December.
    time_t ev;
    CHECK(!sun_event(71.29, -156.79, utc(2026, 12, 21, 0, 0), utc(2026, 12, 22, 0, 0), true, &ev));
    printf("sun within %.1f min, moon within %.1f%% and %.1f min of USNO\n", worst_sun, worst_illum, worst_phase);
}

int main(void)
{
    test_stations();
    test_transmitters();
    test_errors();
    test_current();
    test_marine_forecast();
    test_alerts();
    test_observation();
    test_buoy();
    test_icon();
    test_iso8601();
    test_astro();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all weather parser tests passed\n");
    return 0;
}
