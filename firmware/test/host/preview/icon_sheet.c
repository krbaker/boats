// Renders every weather icon state into one contact sheet (icons.rgba, turned
// into icons.png by build.sh): each state large (3x) and at the size the
// display uses, on the time bubble's dark background.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wx_icon_render.h"

#define SIZE   36
#define BIG    (SIZE * 3)
#define CELL_W (BIG + SIZE + 30)
#define CELL_H (BIG + 20)
#define COLS   6

typedef struct {
    const char *name;
    wx_icon_t ic;
    bool grey;
} state_t;

static const state_t states[] = {
    {"sun", {.night = false}},
    {"sun, small cloud", {.cloud = 1}},
    {"sun, large cloud", {.cloud = 2}},
    {"overcast (day)", {.cloud = 3}},
    {"rain (day)", {.cloud = 2, .rain = true}},
    {"snow", {.snow = true, .cloud = 3}},
    {"new moon 2%", {.night = true, .moon_illum = 0.02, .moon_waxing = true}},
    {"waxing crescent 20%", {.night = true, .moon_illum = 0.2, .moon_waxing = true}},
    {"first quarter 50%", {.night = true, .moon_illum = 0.5, .moon_waxing = true}},
    {"waxing gibbous 80%", {.night = true, .moon_illum = 0.8, .moon_waxing = true}},
    {"full moon 100%", {.night = true, .moon_illum = 1.0, .moon_waxing = true}},
    {"waning crescent 20%", {.night = true, .moon_illum = 0.2, .moon_waxing = false}},
    {"moon 60%, small cloud", {.night = true, .moon_illum = 0.6, .moon_waxing = false, .cloud = 1}},
    {"moon 60%, large cloud", {.night = true, .moon_illum = 0.6, .moon_waxing = false, .cloud = 2}},
    {"overcast (night)", {.night = true, .moon_illum = 0.6, .cloud = 3}},
    {"rain (night)", {.night = true, .moon_illum = 0.6, .cloud = 2, .rain = true}},
    {"stale (greyed)", {.cloud = 1}, true},
    {"waning gibbous 80%", {.night = true, .moon_illum = 0.8, .moon_waxing = false}},
};
#define NSTATES (int)(sizeof(states) / sizeof(states[0]))

static uint32_t sheet[(CELL_H * ((NSTATES + COLS - 1) / COLS)) * (CELL_W * COLS)];

static void put(int w, int x, int y, uint32_t argb)
{
    // Blend over the bubble colour (black at 60% over the map's navy).
    uint32_t bg = sheet[y * w + x];
    float a = (argb >> 24) / 255.0f;
    uint32_t out = 0xFF000000;
    for (int sh = 0; sh < 24; sh += 8) {
        float c = ((argb >> sh) & 255) * a + ((bg >> sh) & 255) * (1 - a);
        out |= (uint32_t)(c + 0.5f) << sh;
    }
    sheet[y * w + x] = out;
}

int main(void)
{
    const int w = CELL_W * COLS, h = CELL_H * ((NSTATES + COLS - 1) / COLS);
    for (int i = 0; i < w * h; i++) sheet[i] = 0xFF09141F;
    static uint32_t icon[SIZE * SIZE], big[BIG * BIG];
    for (int s = 0; s < NSTATES; s++) {
        int ox = (s % COLS) * CELL_W + 10, oy = (s / COLS) * CELL_H + 10;
        wx_icon_render(&states[s].ic, states[s].grey, big, BIG);
        wx_icon_render(&states[s].ic, states[s].grey, icon, SIZE);
        for (int y = 0; y < BIG; y++)
            for (int x = 0; x < BIG; x++) put(w, ox + x, oy + y, big[y * BIG + x]);
        for (int y = 0; y < SIZE; y++)
            for (int x = 0; x < SIZE; x++) put(w, ox + BIG + 10 + x, oy + BIG - SIZE + y, icon[y * SIZE + x]);
        fprintf(stderr, "%2d %s\n", s, states[s].name);
    }
    FILE *f = fopen("icons.rgba", "wb");
    fwrite(&w, sizeof(int), 1, f);
    fwrite(&h, sizeof(int), 1, f);
    fwrite(sheet, 4, (size_t)w * h, f);
    fclose(f);
    return 0;
}
