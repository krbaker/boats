#!/bin/sh
# Build and run the host-side unit tests (AIS parser, AIS demodulator, geo
# helpers, vessel cache, retransmit, weather parsers and icon, simulator round
# trip). Set AIS_CAPTURE to
# a .cu8 I/Q capture (288 kS/s, centred on 162.000 MHz) to also run the
# demodulator over real signals.
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../../components/ais/include -I../../components/ais_demod/include -I../../main/geo"
mkdir -p build
$CC $CFLAGS -o build/test_ais test_ais.c ../../components/ais/ais.c -lm
$CC $CFLAGS -o build/test_geo test_geo.c ../../main/geo/geo.c -lm
$CC $CFLAGS -o build/ais_dump ais_dump.c ../../components/ais/ais.c
# The demodulator test pushes ~100 s of samples through: optimise it.
$CC $CFLAGS -O2 -o build/test_ais_demod test_ais_demod.c ../../components/ais_demod/ais_demod.c ../../components/ais/ais.c -lm
$CC $CFLAGS -O2 -o build/ais_demod_file ais_demod_file.c ../../components/ais_demod/ais_demod.c ../../components/ais/ais.c -lm
$CC $CFLAGS -O2 -o build/ais_capture_score ais_capture_score.c ../../components/ais_demod/ais_demod.c ../../components/ais/ais.c -lm
build/test_ais
build/test_ais_demod
if [ -n "$AIS_CAPTURE" ]; then build/ais_demod_file "$AIS_CAPTURE" | tail -1; fi
build/test_geo
$CC $CFLAGS -I../../components/vessel_cache/include -o build/test_vessel_cache test_vessel_cache.c \
    ../../components/vessel_cache/vessel_cache.c -pthread
rm -rf build/vessel_cache && mkdir -p build/vessel_cache
build/test_vessel_cache build/vessel_cache
# Retransmit: the real main/net/ais_fwd.c on FreeRTOS stand-ins, against local
# UDP and TCP listeners (about 15 s: it waits out the TCP reconnect back-off).
$CC $CFLAGS -D_DEFAULT_SOURCE -Wno-unused-parameter -Ifwd_stubs -I../../main/config -I../../main/net \
    -o build/test_ais_fwd test_ais_fwd.c ../../main/net/ais_fwd.c -pthread
build/test_ais_fwd 2>/dev/null
python3 roundtrip.py

# Weather parsers and sun/moon, on real responses and USNO tables in fixtures/weather/.
CJSON=${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON
W=../../components/weather_parse
$CC $CFLAGS -I$W/include -I"$CJSON" -o build/test_weather_parse test_weather_parse.c $W/weatherlink.c $W/nws.c \
    $W/ndbc.c $W/wx.c $W/astro.c "$CJSON/cJSON.c" -lm
build/test_weather_parse

# Weather icon renderer: moon phases by lit area, nothing clipped, stale = grey.
$CC $CFLAGS -Wno-missing-field-initializers -I../../main/ui -I$W/include -o build/test_wx_icon test_wx_icon.c \
    ../../main/ui/wx_icon_render.c -lm
build/test_wx_icon

# Map upload (streaming zip reader + manifest validation) against a scratch directory.
CJSON=${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON
rm -rf build/fs && mkdir -p build/fs
$CC $CFLAGS -Istubs -I../../main/map -I"$CJSON" -D_DEFAULT_SOURCE -DMAP_ROOT='"build/fs"' -o build/test_map_store \
    test_map_store.c ../../main/map/map_store.c ../../main/map/map_pack.c "$CJSON/cJSON.c" -lm
build/test_map_store $(python3 make_test_zips.py) 2>/dev/null
