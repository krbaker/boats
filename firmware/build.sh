#!/bin/sh
# Builds the firmware for one board, in its own build directory (BOARDS_PLAN.md).
#
#   ./build.sh <board> [idf.py actions...]     default action: build
#
#   ./build.sh lcd34c                          # -> build-lcd34c/ais_display.bin
#   ./build.sh lcd4c -p /dev/ttyACM0 flash monitor
#
# Boards: lcd34c (3.4" round), lcd4c (4" round), lcd10 (10.1"),
#         headless (no screen: ESP32-P4-WIFI6-POE-ETH, ESP32-P4-WIFI6-DB).
# Plain `idf.py build` still builds the 3.4C, in build/.
set -e
cd "$(dirname "$0")"
board=$1
case "$board" in
    lcd34c | lcd4c | lcd10 | headless) shift ;;
    *)
        sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
[ $# -gt 0 ] || set -- build
exec idf.py -B "build-$board" -D AIS_BOARD="$board" -D SDKCONFIG="build-$board/sdkconfig" "$@"
