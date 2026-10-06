// Draws the weather icon (sun or moon, cloud, rain, snow) into an ARGB8888
// buffer, anti-aliased, for the time bubble (WEATHER_PLAN.md). Plain C with
// no LVGL, so it's tested and previewed on the host.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "weather_parse.h"

// size x size pixels, 0xAARRGGBB, not premultiplied. grey: data is stale.
void wx_icon_render(const wx_icon_t *ic, bool grey, uint32_t *argb, int size);
