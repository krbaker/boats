#!/bin/sh
# Build and run the host-side unit tests (AIS parser, geo helpers, simulator round trip).
set -e
cd "$(dirname "$0")"
CC=${CC:-cc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../../components/ais/include -I../../main/geo"
mkdir -p build
$CC $CFLAGS -o build/test_ais test_ais.c ../../components/ais/ais.c -lm
$CC $CFLAGS -o build/test_geo test_geo.c ../../main/geo/geo.c -lm
$CC $CFLAGS -o build/ais_dump ais_dump.c ../../components/ais/ais.c
build/test_ais
build/test_geo
python3 roundtrip.py

# Map upload (streaming zip reader + manifest validation) against a scratch directory.
CJSON=${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON
rm -rf build/fs && mkdir -p build/fs
$CC $CFLAGS -Istubs -I../../main/map -I"$CJSON" -D_DEFAULT_SOURCE -DMAP_ROOT='"build/fs"' -o build/test_map_store \
    test_map_store.c ../../main/map/map_store.c "$CJSON/cJSON.c" -lm
build/test_map_store $(python3 make_test_zips.py) 2>/dev/null
