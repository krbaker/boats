// Host tests for the weather icon renderer (main/ui/wx_icon_render.c).

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wx_icon_render.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            failures++;                                                \
        }                                                              \
    } while (0)

#define N 144
static uint32_t px[N * N];

// The moon's lit share of its disc, from the pixels: lit ones are bright.
static double lit_fraction(double illum, bool waxing, double *lit_right)
{
    wx_icon_t ic = {.night = true, .moon_illum = illum, .moon_waxing = waxing};
    wx_icon_render(&ic, false, px, N);
    int disc = 0, lit = 0, right = 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            uint32_t p = px[y * N + x];
            double dx = (x + 0.5) / N - 0.5, dy = (y + 0.5) / N - 0.5;
            if (hypot(dx, dy) > 0.36) continue; // the disc (the rim's tint on the dark side stays dim)
            disc++;
            if (((p >> 16) & 255) > 200) {
                lit++;
                right += dx > 0;
            }
        }
    }
    *lit_right = lit ? (double)right / lit : 0;
    return (double)lit / disc;
}

int main(void)
{
    // The lit area follows the illumination (the rim aside), on the right
    // when waxing and the left when waning.
    for (double k = 0.05; k < 1.0; k += 0.15) {
        double right, f = lit_fraction(k, true, &right);
        CHECK(fabs(f - k) < 0.04);
        if (k < 0.5) CHECK(right > 0.95);
        f = lit_fraction(k, false, &right);
        CHECK(fabs(f - k) < 0.04);
        if (k < 0.5) CHECK(right < 0.05);
    }

    // Nothing reaches the edge of the square, in any state.
    static const wx_icon_t states[] = {
        {.cloud = 0}, {.cloud = 1}, {.cloud = 2}, {.cloud = 3}, {.cloud = 2, .rain = true}, {.snow = true},
        {.night = true, .moon_illum = 1, .cloud = 0}, {.night = true, .moon_illum = 1, .cloud = 1},
        {.night = true, .moon_illum = 1, .cloud = 2}, {.night = true, .moon_illum = 1, .cloud = 3},
        {.night = true, .moon_illum = 1, .cloud = 2, .rain = true}, {.cloud = 0, .rain = true},
    };
    for (unsigned s = 0; s < sizeof(states) / sizeof(states[0]); s++) {
        wx_icon_render(&states[s], false, px, N);
        int edge = 0, any = 0;
        for (int i = 0; i < N * N; i++) {
            int x = i % N, y = i / N;
            bool on = (px[i] >> 24) > 8;
            any += on;
            if (on && (x == 0 || y == 0 || x == N - 1 || y == N - 1)) edge++;
        }
        if (edge || !any) printf("state %u: %d edge pixels, %d drawn\n", s, edge, any);
        CHECK(edge == 0 && any > 0);
    }

    // Stale: grey and see-through.
    wx_icon_t sun = {0};
    wx_icon_render(&sun, true, px, N);
    uint32_t c = px[(N / 2) * N + N / 2];
    CHECK(((c >> 16) & 255) == ((c >> 8) & 255) && ((c >> 8) & 255) == (c & 255) && (c >> 24) < 160);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all weather icon tests passed\n");
    return 0;
}
