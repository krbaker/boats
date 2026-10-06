// Weather icon renderer: see wx_icon_render.h.
//
// Each shape is a signed distance function in a unit square (0,0 top left,
// 1,1 bottom right). Pixels are 4x4 supersampled and painted back to front,
// so edges are smooth at the 36 px the bubble uses.

#include "wx_icon_render.h"

#include <math.h>
#include <string.h>

#define SS 4 // samples per pixel, each way
#define PI_F 3.14159265f

typedef struct {
    float r, g, b, a; // 0-1, not premultiplied
} rgba_t;

static rgba_t rgb(uint32_t hex, float a)
{
    return (rgba_t){((hex >> 16) & 255) / 255.0f, ((hex >> 8) & 255) / 255.0f, (hex & 255) / 255.0f, a};
}

static const uint32_t SUN = 0xFFC83D, MOON_LIT = 0xF4F1E4, MOON_DARK = 0x3A4A5C, CLOUD = 0xDCE4EC,
                      CLOUD_EDGE = 0x8A9AAB, RAIN = 0x5FB3FF, SNOW = 0xE6F3FF;

// Painter's algorithm on one sample: "over" compositing.
static void over(rgba_t *dst, rgba_t src)
{
    float a = src.a + dst->a * (1 - src.a);
    if (a <= 0) return;
    dst->r = (src.r * src.a + dst->r * dst->a * (1 - src.a)) / a;
    dst->g = (src.g * src.a + dst->g * dst->a * (1 - src.a)) / a;
    dst->b = (src.b * src.a + dst->b * dst->a * (1 - src.a)) / a;
    dst->a = a;
}

static float sd_circle(float x, float y, float cx, float cy, float r)
{
    return hypotf(x - cx, y - cy) - r;
}

// A line from a to b with round ends, half-thickness h.
static float sd_capsule(float x, float y, float ax, float ay, float bx, float by, float h)
{
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float t = (px * dx + py * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return hypotf(px - dx * t, py - dy * t) - h;
}

// A cloud: three puffs on a flat-bottomed base, centred on (cx, cy), width ~ s.
static float sd_cloud(float x, float y, float cx, float cy, float s)
{
    float d = sd_circle(x, y, cx - 0.22f * s, cy + 0.06f * s, 0.20f * s);
    d = fminf(d, sd_circle(x, y, cx + 0.02f * s, cy - 0.08f * s, 0.27f * s));
    d = fminf(d, sd_circle(x, y, cx + 0.25f * s, cy + 0.06f * s, 0.19f * s));
    d = fminf(d, sd_capsule(x, y, cx - 0.22f * s, cy + 0.12f * s, cx + 0.25f * s, cy + 0.12f * s, 0.13f * s));
    return d;
}

typedef struct {
    const wx_icon_t *ic;
    float body_x, body_y, body_s;   // the sun or moon: centre and scale
    bool cloud;
    float cloud_x, cloud_y, cloud_s;
} scene_t;

// The sun or moon at one sample point.
static void paint_body(const scene_t *sc, float x, float y, rgba_t *px)
{
    float s = sc->body_s, cx = sc->body_x, cy = sc->body_y;
    if (!sc->ic->night) {
        if (sd_circle(x, y, cx, cy, 0.21f * s) < 0) over(px, rgb(SUN, 1));
        for (int i = 0; i < 8; i++) {
            float a = i * PI_F / 4, c = cosf(a), sn = sinf(a);
            if (sd_capsule(x, y, cx + c * 0.31f * s, cy + sn * 0.31f * s, cx + c * 0.44f * s, cy + sn * 0.44f * s,
                           0.04f * s) < 0) {
                over(px, rgb(SUN, 1));
            }
        }
        return;
    }
    // The moon: lit where it's past the terminator, an ellipse whose
    // half-width goes from the full radius (new) through 0 (quarter) back to
    // the full radius on the other side (full).
    float R = 0.36f * s, u = (x - cx) / R, v = (y - cy) / R;
    if (u * u + v * v > 1) return;
    if (!sc->ic->moon_waxing) u = -u; // waning: lit on the left
    float edge = (1 - 2 * (float)sc->ic->moon_illum) * sqrtf(fmaxf(0, 1 - v * v));
    over(px, u > edge ? rgb(MOON_LIT, 1) : rgb(MOON_DARK, 0.85f));
    if (u * u + v * v > 0.8f) over(px, rgb(MOON_LIT, 0.3f)); // a faint rim, so a new moon still shows
}

static void paint_cloud(const scene_t *sc, float x, float y, rgba_t *px)
{
    if (!sc->cloud) return;
    float d = sd_cloud(x, y, sc->cloud_x, sc->cloud_y, sc->cloud_s);
    if (d < 0) over(px, rgb(d > -0.025f ? CLOUD_EDGE : CLOUD, 1)); // a thin darker rim keeps it apart from the sun
}

static void paint_rain(const scene_t *sc, float x, float y, rgba_t *px)
{
    for (int i = 0; i < 3; i++) {
        float bx = sc->cloud_x - 0.18f + i * 0.17f, by = sc->cloud_y + 0.24f;
        if (sd_capsule(x, y, bx, by, bx - 0.07f, by + 0.2f, 0.035f) < 0) over(px, rgb(RAIN, 1));
    }
}

static void paint_snow(float x, float y, rgba_t *px)
{
    const float cx = 0.5f, cy = 0.5f, L = 0.42f, h = 0.04f;
    for (int i = 0; i < 3; i++) {
        float a = i * PI_F / 3 + PI_F / 2, c = cosf(a), s = sinf(a);
        bool hit = sd_capsule(x, y, cx - c * L, cy - s * L, cx + c * L, cy + s * L, h) < 0;
        // Small branches near each end.
        for (int end = -1; end <= 1 && !hit; end += 2) {
            float ex = cx + end * c * L * 0.62f, ey = cy + end * s * L * 0.62f;
            for (int side = -1; side <= 1 && !hit; side += 2) {
                float b = a + side * PI_F / 4 * (end > 0 ? 1 : -1);
                float tx = ex + end * cosf(b) * 0.14f, ty = ey + end * sinf(b) * 0.14f;
                hit = sd_capsule(x, y, ex, ey, tx, ty, h * 0.85f) < 0;
            }
        }
        if (hit) {
            over(px, rgb(SNOW, 1));
            return;
        }
    }
}

void wx_icon_render(const wx_icon_t *ic, bool grey, uint32_t *argb, int size)
{
    // Where things go for each amount of cloud. The sun's rays reach 0.44 x
    // its scale from its centre, so each layout keeps them inside the square.
    scene_t sc = {.ic = ic, .body_x = 0.5f, .body_y = 0.5f, .body_s = 1, .cloud_x = 0.5f, .cloud_y = 0.5f};
    switch (ic->cloud) {
    case 1: // a small cloud in front, lower right
        sc = (scene_t){ic, 0.42f, 0.42f, 0.82f, true, 0.66f, 0.68f, 0.55f};
        break;
    case 2: // a large cloud covering most of it
        sc = (scene_t){ic, 0.38f, 0.38f, 0.75f, true, 0.55f, 0.62f, 0.85f};
        break;
    case 3: // overcast: a sliver showing behind
        sc = (scene_t){ic, 0.34f, 0.32f, 0.6f, true, 0.52f, 0.58f, 1.0f};
        break;
    }
    if (ic->rain && !ic->snow) { // the cloud higher, to make room for the rain under it
        sc = (scene_t){ic, 0.36f, 0.29f, 0.58f, true, 0.54f, 0.46f, 0.85f};
    }

    for (int py = 0; py < size; py++) {
        for (int pxl = 0; pxl < size; pxl++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float x = (pxl + (sx + 0.5f) / SS) / size, y = (py + (sy + 0.5f) / SS) / size;
                    rgba_t s = {0};
                    if (ic->snow) {
                        paint_snow(x, y, &s);
                    } else {
                        paint_body(&sc, x, y, &s);
                        paint_cloud(&sc, x, y, &s);
                        if (ic->rain) paint_rain(&sc, x, y, &s);
                    }
                    r += s.r * s.a, g += s.g * s.a, b += s.b * s.a, a += s.a;
                }
            }
            uint32_t out = 0;
            if (a > 0) {
                r /= a, g /= a, b /= a, a /= SS * SS;
                if (grey) {
                    float l = 0.3f * r + 0.59f * g + 0.11f * b;
                    r = g = b = l;
                    a *= 0.55f;
                }
                out = (uint32_t)(a * 255 + 0.5f) << 24 | (uint32_t)(r * 255 + 0.5f) << 16 |
                      (uint32_t)(g * 255 + 0.5f) << 8 | (uint32_t)(b * 255 + 0.5f);
            }
            argb[py * size + pxl] = out;
        }
    }
}

// Distance to y = y0 + a sin(k x), roughly: the vertical gap over the slope.
static float sd_wave(float x, float y, float y0, float a, float k, float h)
{
    float d = y - (y0 + a * sinf(k * x));
    float slope = a * k * cosf(k * x);
    return fabsf(d) / sqrtf(1 + slope * slope) - h;
}

void wx_wave_render(uint32_t *argb, int size)
{
    const float k = 2 * PI_F * 1.5f; // one and a half waves across
    for (int py = 0; py < size; py++) {
        for (int pxl = 0; pxl < size; pxl++) {
            float a = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    float x = (pxl + (sx + 0.5f) / SS) / size, y = (py + (sy + 0.5f) / SS) / size;
                    bool in = x > 0.06f && x < 0.94f &&
                              (sd_wave(x, y, 0.36f, 0.09f, k, 0.06f) < 0 || sd_wave(x, y, 0.66f, 0.09f, k, 0.06f) < 0);
                    a += in;
                }
            }
            a /= SS * SS;
            argb[py * size + pxl] = a > 0 ? (uint32_t)(a * 255 + 0.5f) << 24 | RAIN : 0;
        }
    }
}

