// The weather icon, pressure trend and compass points: see weather_parse.h.

#include "weather_parse.h"

#define OBS_FRESH_S (3 * 3600) // an NWS observation older than this is ignored

void wx_icon_decide(double lat, double lon, time_t now, const wx_obs_t *obs, float rain_rate_in_hr, bool alert,
                    wx_icon_t *out)
{
    *out = (wx_icon_t){.alert = alert};
    out->night = !sun_is_up(lat, lon, now);
    if (out->night) {
        moon_t m;
        moon_at(now, &m);
        out->moon_illum = m.illum;
        out->moon_waxing = m.waxing;
    }
    bool fresh = obs && obs->when && now - obs->when < OBS_FRESH_S;
    if (fresh) {
        switch (obs->cloud) {
        case CLOUD_SCATTERED: out->cloud = 1; break;
        case CLOUD_BROKEN: out->cloud = 2; break;
        case CLOUD_OVERCAST: out->cloud = 3; break;
        default: out->cloud = 0; break;
        }
        out->snow = obs->precip == PRECIP_SNOW;
        out->rain = obs->precip == PRECIP_RAIN;
    }
    if (rain_rate_in_hr > 0) out->rain = true; // the station's own gauge says so
    if (out->rain && out->cloud < 2) out->cloud = 2; // rain needs a cloud to fall from
}

trend_t wx_pressure_trend(float change)
{
    if (change >= 0.06f) return TREND_RISING_FAST;
    if (change <= -0.06f) return TREND_FALLING_FAST;
    if (change >= 0.02f) return TREND_RISING;
    if (change <= -0.02f) return TREND_FALLING;
    return TREND_STEADY;
}

const char *wx_trend_name(trend_t t)
{
    static const char *names[] = {"steady", "rising", "falling", "rising rapidly", "falling rapidly"};
    return (unsigned)t < 5 ? names[t] : "?";
}

const char *wx_compass(int deg)
{
    static const char *pts[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    deg %= 360;
    if (deg < 0) deg += 360;
    return pts[((deg * 2 + 22) / 45) % 16];
}
