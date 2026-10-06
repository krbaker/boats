# Plan: weather

Add marine weather to the map screen. The time bubble at the top gets a weather icon, plus a red "!" when the marine forecast zone has a watch, warning or advisory. The edge of the map gets two markers: a yellow wind arrow and a blue wave triangle. Tapping the time bubble opens a weather overlay styled like the vessel menu, with three tabs: **Marine forecast**, **Waves** and **Weather**.

New settings sit next to the tide station: a **NOAA marine forecast zone**, a **WeatherLink station** (your Davis station, through the WeatherLink v2 API), and an **NDBC buoy**.

## Data sources

All were checked on 2026-10-05 and 2026-10-06, and real responses are saved in `firmware/test/host/fixtures/weather/`.

| What | Source | Key needed | How often |
|---|---|---|---|
| **Marine forecast text** | NWS text product, `tgftp.nws.noaa.gov/data/forecasts/marine/coastal/<pz>/<pzz135>.txt` (the zone in lower case, after its first two letters). About 1.5 KB: the zone name, the issue time, any headlines (`...SMALL CRAFT ADVISORY IN EFFECT UNTIL 10 PM EDT THIS EVENING...`), then one paragraph per period (`.TONIGHT...N wind around 5 kt...`). The NWS API's JSON version of the same forecast is 1 MB, almost all of it the zone's outline, so it isn't used. | No | 30 min |
| **Watches, warnings and advisories** | NWS API, `api.weather.gov/alerts/active/zone/<zone>`. Returns each alert's event ("Gale Warning", "Small Craft Advisory"), severity, headline, description and expiry. Usually well under 10 KB. | No (a User-Agent with contact details is required) | 5 min |
| **Waves** | NDBC buoy, `www.ndbc.noaa.gov/data/realtime2/<buoy>.txt`. A text table, newest row first: wave height, dominant and average period, mean wave direction, plus wind, pressure, air and water temperature where the buoy has them. Waves are often in every other row only, so each value comes from the newest row that has it. | No | 10 min |
| **Current weather** | WeatherLink v2 API, `api.weatherlink.com/v2/current/<station-id>?api-key=<key>`, with the secret in an `X-Api-Secret` header. About 5 KB, already in mph, °F, inHg and inches. Each sensor is listed separately: outdoor sensor suites, the barometer and the indoor sensor. See below. | **Yes**: your v2 API key and secret | 1 min (the limit is 1,000 calls an hour) |
| **Cloud cover, rain or snow** (for the icon) | The nearest NWS observation station, found from the chart's centre through `api.weather.gov/points/<lat>,<lon>`, then `/stations/<id>/observations/latest`. Returns cloud layers (CLR / FEW / SCT / BKN / OVC) and present weather ("Light Snow", "Rain"). | No | 15 min (stations report hourly) |
| **Sun and moon** | Worked out on the display from the chart's centre and the clock: sunrise, sunset, and the moon's phase and illumination. | — | — |

The fetching follows the tide code: `esp_http_client` with the certificate bundle, in a background task, retrying after 5 minutes on failure. Every response is under 10 KB.

### WeatherLink

Your key can see two stations: a **WeatherLink Console** (product 6313) and a **WeatherLink Live** (product 6100).

- **Outdoor sensor suites:**
  - The Console hears one, on transmitter **6**.
  - The Live hears two, on transmitters **6** and **1**.
  - Each reports wind, temperature, humidity, dew point and rain.
- **Wind:**
  - `wind_speed_last`, the latest reading;
  - `wind_speed_avg_last_10_min` with `wind_dir_scalar_avg_last_10_min`, the 10-minute average and its direction;
  - `wind_speed_hi_last_10_min`, the gust.
- **Barometer:** `bar_sea_level` (inHg), and `bar_trend`, the change over the last 3 hours in inHg. WeatherLink works the trend out itself, so no history is needed.
- **Rain:** `rain_rate_last_in`, and today's total. The Console calls that `rainfall_day_in` and the Live calls it `rainfall_daily_in`, so the parser accepts both.
- **Time:** each sensor's own `ts`, so stale data can be spotted.

Settings:
- **Credentials:** the API key, and the secret, which is stored like the Wi-Fi password and never sent back to the page.
- **Choosing the station:** after you enter the key and secret, the web page asks the display to list your stations and their outdoor transmitters, so you choose from a list rather than typing IDs.

## Units

Wind in **mph**, as you asked, with **°F**, **inHg** and rain in inches. WeatherLink already reports in these units. Waves are in **feet** and seconds. The buoy reports m/s, metres, °C and hPa, so those are converted.

## The map screen

The map is always north-up, so a direction on the screen is a compass direction.

### Weather icon (in the time bubble)

The icon is drawn just after the time ("3:45 PM ☀"). It's drawn by the display itself, not from image files, so any moon phase comes out exactly.

| Condition | Icon |
|---|---|
| **Day** (the sun is up at the chart's centre) | A sun |
| **Night** | The moon at its current phase, with the lit part drawn as it looks that night |
| **Scattered cloud** (SCT) | A small cloud in front of the sun or moon |
| **Broken cloud** (BKN) | A large cloud mostly covering it |
| **Overcast** (OVC) | Cloud only, with a sliver of the sun or moon behind it |
| **Rain**: your station's rain rate is above zero, or the NWS station reports rain, drizzle or showers | The cloud with rain lines under it |
| **Snow**: the NWS station reports snow, sleet or flurries | A snowflake, whatever else is happening |
| **Alert**: the marine zone has an active watch, warning or advisory (including Small Craft Advisory) | A red "!" after the icon |

If the cloud data is missing, the plain sun or moon is shown. If everything is older than 3 hours, the icon is greyed out.

### Wind arrow

- **Shape:** a translucent yellow arrow just inside the rim, at the compass bearing the wind comes *from*, pointing in towards the centre.
- **Label:** the 10-minute average wind speed in mph, written upright inside it, such as "12". When the 10-minute gust is well above the average (by 10 mph or more), it shows both, such as "12 G 22".
- **Source:** your WeatherLink station's 10-minute average direction. Without a station, the buoy's wind is used instead.
- **Stale or calm:** the arrow is hidden when the reading is more than an hour old, and shrinks to a small circle when the wind is calm.

### Wave triangle

- **Shape:** a translucent blue triangle at the bearing the waves come *from* (the buoy's mean wave direction), pointing in.
- **Label:** height and period inside it, such as "2.3 ft 8 s".
- **Overlap:** it sits a little further in than the wind arrow, so the two can share a bearing without covering each other.
- **Stale:** hidden when the buoy's last wave reading is more than 3 hours old.

Both are drawn under the edge bubbles, and are translucent so the map shows through. When the wind comes from due north, the arrow sits behind the time bubble, which stays readable on top.

### Tap the time bubble

Tapping it opens the weather overlay: the same round panel, close button and tab bar as the vessel menu.

## The overlay

**Marine forecast**
- **Header:** the zone's name and when the forecast was issued.
- **Alerts:** any active alert is listed first, in red: the event, such as "Gale Warning", then until when. Tapping it shows the full text.
- **Forecast:** each period's name in bold, then its text, wrapped and scrolling.
- **The forecast's own warnings:** NWS writes these at the top in capitals ("...SMALL CRAFT ADVISORY IN EFFECT..."). They're picked out the same way.

**Waves**
- **Header:** the buoy's ID and name, and the time of the reading.
- **Waves:** height, dominant and average period, and the direction they come from, as a compass point and degrees with a small arrow.
- **Everything else the buoy reports:** wind and gust, water and air temperature, pressure and its 3-hour trend. A value the buoy doesn't report is left out.

**Weather**
- **Wind:** now, the 10-minute average, the gust, and the direction (compass point and degrees).
- **Temperature and humidity**, with dew point.
- **Pressure:**
  - **Reading:** in inHg.
  - **Trend:** rising, falling or steady, from WeatherLink's 3-hour change. "Steady" means a change under 0.02 inHg, and "rapidly" means 0.06 inHg or more.
  - **Rate:** the change in inHg per 3 hours.
- **Rain:** rate, and the total today.
- **Sun:** sunrise and sunset today.
- **Moon:** phase name ("Waxing gibbous"), percent lit, and the next full or new moon.
- **Footer:** the station's name and how long ago it reported.

## Settings and web page

In the "Tides & time" section, renamed **"Tides, weather & time"**:

| Setting | Example | Check |
|---|---|---|
| **Marine forecast zone** | `PZZ135` | Three letters and three digits, with a link to NOAA's marine zone map |
| **WeatherLink API key** and **API secret** | (from weatherlink.com → Account → API Key v2) | The secret is stored like the Wi-Fi password, and never sent back to the page |
| **WeatherLink station and transmitter** | The Live, transmitter 6 | Picked from the list the display fetches with your key |
| **NDBC buoy** | `46088` | Letters and digits, up to 7 characters |
| **NWS observation station** (the icon's sky) | `KBOS`, or empty | Letters and digits. Empty means automatic: the station nearest the chart's centre |

Each source gets a row in the web page's status table, such as "Marine forecast: issued 1:33 PM", "Buoy 46088: 10 min ago" or "WeatherLink: API key or secret rejected". Each is optional. A source that isn't set is left out of the icon, the markers and the overlay, and the matching tab says how to set it.

## How it's built

- **`main/weather/weather.c`:** one fetch task, with each source on its own timer. It keeps the latest parsed values behind a lock, like `tide.c`.
- **`components/weather_parse`:** portable C, tested on the PC with real responses saved as fixtures. It parses the NWS marine forecast text, the alerts and observation JSON, the NDBC text, and the WeatherLink current JSON. It also decides the icon from those values, and computes sunrise, sunset and the moon's phase.
- **`main/ui/ui_weather.c`:** the overlay. The icon and the edge markers are drawn in `ui_map.c` (`draw_pills` and the layer under it).
- **The host UI preview** gets simulated weather, and PNGs of:
  - the icon in each state: day, night at a few phases, each cloud level, rain, snow, alert;
  - the wind and wave markers at a few bearings, including behind a bubble;
  - each tab.

  This lets the look be settled before it goes on the display.

## Steps

| # | Step | Done when |
|---|---|---|
| 1 | **Settings and web page:** the zone, buoy and WeatherLink settings, their checks, the station and transmitter list, and the status rows (empty for now). | They save, survive a restart, the WeatherLink secret is never returned by `/api/config`, and the station list shows your two stations and their transmitters. |
| 2 | **Parsers and sun/moon** (`components/weather_parse`, host-tested), on the saved fixtures. | Host tests pass on the real responses: both WeatherLink products, a forecast with and without an advisory headline, buoy rows with missing (`MM`) values, no alerts, and an alert. Sunrise, sunset and moon phase match USNO tables to within 2 minutes and 2% illumination. |
| 3 | **Fetching** (`main/weather/weather.c`): the task, timers, retries, status on the web page. | All sources show on the status table with sensible times, and survive Wi-Fi drops. |
| 4 | **Map screen:** the icon and alert mark in the time bubble, the wind arrow and the wave triangle. Previews first. | The previews look right in every icon state and at several bearings, and then on the display. Panning stays smooth. |
| 5 | **Overlay:** the three tabs, opened from the time bubble. Previews first. | The previews read well, including a long forecast with an alert, and then on the display. |
| 6 | **Soak:** a week on the display. | No memory growth, data stays current, and WeatherLink stays well inside its limit (this makes 60 calls an hour, of 1,000). |

## Progress (2026-10-06)

- **Step 1 built, not yet tested on the display.**
  - **Settings:** `wx_zone`, `wx_buoy`, `wl_key`, `wl_station` and `wl_tx` are in `settings_t` and NVS, and a change raises `SETTINGS_EV_WEATHER`. The WeatherLink secret is kept apart (`settings_get_wl_secret` / `settings_set_wl_secret`), like the Wi-Fi password. `/api/config` only says whether one is saved (`wl_secret_set`).
  - **Checks:** the zone is two letters, Z and three digits; the buoy, key and secret are letters and digits. The page and the server both check.
  - **Station list:** `POST /api/weatherlink/stations` takes the key and the typed secret, or the saved one. It returns each station with the outdoor sensor suites it hears, with the current temperature and wind, so you can tell them apart. It makes 1 + one request per station, a few seconds, in the web server's task.
  - **Parsing:** `components/weather_parse` (`weatherlink.c`) has the station list, transmitters and error parsing. Its tests, `test/host/test_weather_parse.c`, run on the saved responses in `run_tests.sh`.
  - **Web page:** the section is now "Tides, weather & time": zone and buoy, with links for finding them, the WeatherLink key and secret, "Find my stations", and the picker. A saved secret shows as "saved", and the field is cleared after saving.
  - **Not yet:** the status rows. They come with the fetching in step 3, since until then there's nothing to show.
- **Step 2 done** (`components/weather_parse`, tests in `test/host/test_weather_parse.c`).
  - **Marine forecast text** (`mf_parse`): zone, name, issue time, expiry, headlines (including one wrapped over two lines), and periods with their wrapped lines joined.
  - **Alerts** (`wx_parse_alerts`): event, headline, description with the instruction appended, end time, and whether it flags (watch, warning or advisory). Test messages and cancellations are dropped.
  - **NWS observation** (`wx_parse_observation`): the most cloud of any layer, and rain or snow from `presentWeather` or the text. The points lookup and nearest station come from `wx_parse_points` and `wx_parse_first_station`.
  - **NDBC buoy** (`buoy_parse`): columns found by name, each value from the newest row that has it, with that row's time, converted to mph, feet, °F and inHg.
  - **WeatherLink current** (`wl_parse_current`): the chosen transmitter's wind (latest, 10-minute average and direction, gust), temperature, humidity, dew point and rain, plus the barometer and its 3-hour trend. Both products' names for today's rain are handled.
  - **Icon** (`wx_icon_decide`), **pressure trend** and **compass points**. An NWS observation over 3 hours old is ignored. Rain always brings at least a large cloud.
  - **Sun and moon** (`astro.c`): within 0.5 minutes of USNO for sunrise and sunset, on 25 days across five latitudes from Juneau to Sydney, and within 0.5% for illumination (USNO gives it at 12:00 UT). New and full moon times for all of 2026 are within 1.7 minutes. Polar night is handled.
  - **Checking the tests:** seven rules were broken in turn (headline joining, newest-row-wins, ignoring a stale observation, which alerts flag, choosing the transmitter, refraction), and the tests caught each one.
  - **Not tested on real data:** snow and rain observations, a wrapped headline, and a buoy without waves are written into the tests by hand. None happened in the area while the fixtures were saved.
- **Step 3 built, not yet tested on the display.**
  - **Fetching** (`main/weather/weather.c`): one task, started from `app_main`.
    - **Timers:** WeatherLink every minute, alerts every 5 minutes, buoy every 10, NWS observation every 15, marine forecast every 30. A failure keeps the last good data, shows the error, and retries after 5 minutes.
    - **Waits** for Wi-Fi and a set clock, since HTTPS needs both.
    - **Changes:** saving the weather settings fetches everything again straight away. Moving the chart's centre looks up the nearest NWS station again.
  - **Data:** `weather_get()` copies everything (about 20 KB) for the screens in steps 4 and 5, and `weather_generation()` changes whenever it does. The WeatherLink station's name is looked up once per station.
  - **Forecast location:** the zone's forecast is found under `coastal/`, `near_shore/` (Great Lakes) or `offshore/`, and the folder that worked is remembered. A file that isn't there answers **301**, not 404, so a 301, 302 or 404 means "try the next folder". ESP-IDF's HTTP client only follows redirects in `esp_http_client_perform()`, and following this one ends at a 403 anyway.
  - **URLs checked from a PC:**
    - `stations/<id>` on WeatherLink;
    - the `near_shore/` and `offshore/` folders, using zones LEZ142, PZZ835 and ANZ800;
    - `?limit=1` on the NWS station list, which gives KBFI.
  - **Status:** `/api/status` has a `weather` object with each source's state, error, fetch time and a summary. The web page's status table has a row for each source that's set up: marine forecast (with any headlines), alerts (red when one flags), buoy, weather station, and sky (the NWS station).
- **NWS station, configurable (2026-10-06):** a new `wx_obs` setting. Empty keeps the automatic choice, the nearest station to the chart's centre (KBVY for yours). An ID such as KBOS overrides it. The status row shows the station, with ", nearest" when it was chosen automatically. An unknown ID shows "no such NWS station, or it isn't reporting" (NWS answers 404).
- **Step 4 built and previewed, not yet on the display.**
  - **Icon** (`main/ui/wx_icon_render.c`): drawn in software, 4×4 supersampled, into a 36 px ARGB image the time bubble draws. The moon's lit part is computed per pixel from the terminator, with a faint rim so a new moon still shows. Host tests (`test/host/test_wx_icon.c`) check that the lit area matches the illumination within 4% from 5% to 95%, waxing and waning, that no state touches the image's edge, and that stale means grey and see-through.
  - **Time bubble:** it grows to make room. The time moves left by half the room, so time and icon together stay centred, and the red "!" follows the icon. The arclabel's own text measure runs short of the drawn text (63 against 82 px on "0.5 nm"), so a bubble with an icon also uses the straight text width.
  - **Wind arrow:** translucent yellow, from 356 to 264 px from the centre. It shows the 10-minute average in mph, with the gust underneath ("G26") when it's 10 mph or more above. It falls back to the buoy's wind, is hidden after an hour without a reading, and is a small circle with "0" when calm. The shaft is one thick line, since two translucent triangles showed a seam. A very faint line remains where the shaft meets the head.
  - **Wave triangle:** translucent blue, from 256 to 176 px, with height and period. It's hidden after 3 hours without a wave reading.
  - **Under the bubbles:** both are drawn before the bubbles, so wind from due north tucks the arrow's tail under the time bubble.
  - **Previews** (`test/host/preview/build.sh`):
    - `icons.png`: every icon state, large and at display size.
    - `map.png`: NW wind 14 G26, a SW swell 2.3 ft 8 s, an advisory.
    - `map_weather_rain.png`: rain, wind from due north, waves from the east.
    - `map_weather_calm.png`: calm, waves from the south.
  - **Preview build fix:** `build.sh` lacked the vessel cache's include path, needed since `vessel_db.h` started including it.
- **Step 5 built and previewed, not yet on the display.**
  - **Overlay** (`main/ui/ui_weather.c`): tap the time bubble. Same panel, close button and tab bar as the vessel overlay, with the tabs **Forecast**, **Waves** and **Weather**.
  - **Forecast tab:**
    - **Alerts first,** as red boxes (grey for statements) with the event and when it ends. Tap one to show the full description and instructions.
    - **The forecast:** the zone's own headlines in amber, then the zone name and issue time, then each period (name in title case, in blue) and its text.
    - **Rebuilt only when** the forecast or alerts change, not on every minute's WeatherLink update, so it doesn't jump back to the top while you're reading.
  - **Waves tab:** the buoy's ID and reading age, then waves, periods, direction, wind, water and air temperature, pressure and its 3-hour change. A value from well before the newest row says how old it is.
  - **Weather tab:** the station's name and age, then wind (10-minute average and direction, gust, latest), temperature, humidity with dew point, pressure with trend and 3-hour change, and rain rate and today's total. Then the NWS station's sky, today's sunrise and sunset at the chart's centre, the moon's phase and percentage lit, and the next new and full moons.
  - **Not set up yet:** a tab says how to set it up, "Fetching...", or the error.
  - **Alert text:** NWS wraps it at a fixed width, so `wx_parse_alerts` now joins those lines, keeping paragraphs and `*` bullets. It's tested.
  - **Previews:** `weather_forecast.png`, `weather_alert_open.png`, `weather_waves.png` and `weather_weather.png`, on the saved real Potomac forecast, its Small Craft Advisory, and buoy 46088.
- **Alert popup (added 2026-10-06):** a new or updated watch, warning or advisory pops up over whatever screen is showing. It uses the same red box as the menu, with no dimmed background, and the full text, scrolling past about 330 px. A ✕ dismisses it.
  - **New or updated:** an alert is identified by its event, headline and end time. NWS re-issues an alert with a new headline when it changes, so an update pops up again.
  - **Several new at once:** the next appears after the first is dismissed.
  - **Expiry:** an alert that expires or is cancelled closes its popup.
  - **Dismissals are kept in memory only,** so an alert still active after a restart pops up once more.
  - **Preview:** `alert_popup.png`, then `map.png` after tapping the ✕.
- **Waves without a direction (2026-10-06):** many buoys measure wave height and period but not direction. 44029, Massachusetts Bay, is one: `MWD` is always `MM`. Before this change, no wave marker was shown at all for such a buoy.
  - **On the map:** the triangle still needs a direction. Without one, the height and period go at the end of the tide bubble after a small blue wave icon, such as "… 0.3 ft 〰 3.0 ft 9 s". The tide bubble now has an optional second label for this.
  - **Status page:** the buoy row turns amber and names what's missing (height, period or direction) when the buoy doesn't report it or hasn't in 3 hours.
  - **Preview:** `map_weather_no_wave_dir.png`.
- **Step 6:** not started.

## Decisions

- **Current weather comes from your Davis station through WeatherLink**, not Weather Underground, whose API key stopped working. The v2 key and secret are entered on the web page. They're never written into the code, the plans or the test fixtures; the fixtures have your location, station names, email and network details removed.
- **Marine forecast:** the NWS plain-text product, not the 1 MB JSON.
- **Outdoor sensor suite: transmitter 6**, which is yours. Transmitter 1 is another station's that your WeatherLink Live also picks up. Both of your stations hear transmitter 6. You pick it from the station list on the web page, and can change it there later.
- **Alerts:** the red "!" shows for watches, warnings and advisories, including Small Craft Advisory.
- **Units:** wind in mph; temperature in °F, pressure in inHg, rain in inches, waves in feet.
- **Night:** the sun shows from sunrise to sunset, and the moon from sunset to sunrise.
