#!/bin/sh
# Build the host UI preview: the real UI sources and LVGL, with the device
# services stubbed out, rendering screens to PNG. Needs a firmware build first
# (for build/config/sdkconfig.h and managed_components/).
#
#   test/host/preview/build.sh [map_builder_output_dir]   # PNGs land in test/host/preview/out/
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
FW=$(cd "$HERE/../../.." && pwd)
OUT="$HERE/out"
OBJ="$HERE/build"
LVGL="$FW/managed_components/lvgl__lvgl"
CC=${CC:-cc}
mkdir -p "$OUT" "$OBJ/lvgl"

CFLAGS="-O1 -g -w -I$HERE/stubs -I$FW/build/config -I$LVGL -include $HERE/stubs/preview_defs.h"

# LVGL, compiled once.
if [ ! -f "$OBJ/liblvgl.a" ]; then
    echo "building LVGL (once)..."
    # LVGL's source names are unique (bar a disabled GPU driver), so batch-compile into one directory.
    (cd "$OBJ/lvgl" && find "$LVGL/src" -name '*.c' | xargs -P"$(nproc)" -n 16 $CC $CFLAGS -c)
    ar rcs "$OBJ/liblvgl.a" "$OBJ"/lvgl/*.o
fi

M="$FW/main"
INC="-I$M -I$M/config -I$M/ais -I$M/tide -I$M/geo -I$M/map -I$M/ui -I$M/net -I$M/sdr -I$M/weather -I$FW/components/ais/include -I$FW/components/ais_demod/include -I$FW/components/weather_parse/include -I$FW/components/vessel_cache/include"
WP="$FW/components/weather_parse"
CJSON=${IDF_PATH:-$HOME/esp/esp-idf}/components/json/cJSON
$CC $CFLAGS $INC -I"$CJSON" -Wall -o "$OBJ/preview" "$HERE/preview.c" "$M/ui/ui.c" "$M/ui/ui_map.c" \
    "$M/ui/ui_vessels.c" "$M/ui/ui_netinfo.c" "$M/ui/ui_settings.c" "$M/ui/ui_tide.c" "$M/ui/map_tile_decode.c" "$M/map/map_store.c" "$M/map/map_pack.c" \
    "$M/geo/geo.c" "$FW/components/ais/ais.c" "$M/ui/wx_icon_render.c" "$M/ui/ui_weather.c" "$WP/wx.c" "$WP/astro.c" "$WP/nws.c" "$WP/ndbc.c" \
    "$CJSON/cJSON.c" "$OBJ/liblvgl.a" -lm

# The weather icon in every state, on one sheet (icons.png).
$CC -std=c11 -O2 -Wall -Wno-missing-field-initializers -I"$M/ui" -I"$WP/include" -o "$OBJ/icon_sheet" \
    "$HERE/icon_sheet.c" "$M/ui/wx_icon_render.c" -lm

# Optional chart set: a map builder output directory (the one holding ais_map/).
CHART=${1:-}
rm -rf "$OUT/sd" && mkdir -p "$OUT/sd"
if [ -n "$CHART" ]; then ln -s "$(cd "$CHART" && pwd)/ais_map" "$OUT/sd/ais_map"; fi
cd "$OUT"
rm -f ./*.png
"$OBJ/preview"
for f in *.raw; do python3 "$HERE/topng.py" "$f" "${f%.raw}.png" && rm "$f"; done
"$OBJ/icon_sheet" 2>/dev/null && python3 "$HERE/rgba2png.py" icons.rgba icons.png && rm icons.rgba
ls "$OUT"
