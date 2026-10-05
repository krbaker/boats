#!/bin/sh
# Build and run the host-side unit tests (AIS parser, AIS demodulator, geo
# helpers, simulator round trip). Set AIS_CAPTURE to a .cu8 I/Q capture (288
# kS/s, centred on 162.000 MHz) to also run the demodulator over real signals.
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
build/test_ais
build/test_ais_demod
if [ -n "$AIS_CAPTURE" ]; then build/ais_demod_file "$AIS_CAPTURE" | tail -1; fi
build/test_geo
python3 roundtrip.py

# Map upload (streaming zip reader + manifest validation) against a scratch directory.
CJSON=${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON
rm -rf build/fs && mkdir -p build/fs
$CC $CFLAGS -Istubs -I../../main/map -I"$CJSON" -D_DEFAULT_SOURCE -DMAP_ROOT='"build/fs"' -o build/test_map_store \
    test_map_store.c ../../main/map/map_store.c ../../main/map/map_pack.c "$CJSON/cJSON.c" -lm
build/test_map_store $(python3 make_test_zips.py) 2>/dev/null
