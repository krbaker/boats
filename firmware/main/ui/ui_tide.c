// Tide overlay (tap the tide on the map): predicted water level for the last
// 6 hours and the next 24, with the highs and lows marked.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tide.h"
#include "ui.h"

#define PAST_S          (6 * 3600)
#define FUTURE_S        (24 * 3600)
#define GRAPH_W         620
#define GRAPH_H         300
#define AXIS_LEFT       44      // room for the height labels
#define AXIS_BOTTOM     28      // room for the time labels
#define MAX_SAMPLES     128
#define MAX_EVENTS      32
#define REFRESH_MS      30000

static lv_obj_t *s_root, *s_title, *s_now, *s_graph, *s_msg;
static tide_sample_t s_samples[MAX_SAMPLES];
static int s_nsamples;
static tide_event_t s_events[MAX_EVENTS];
static int s_nevents;
static time_t s_t0, s_t1;
static float s_hmin, s_hmax;

// Water level at t, interpolated between curve samples. False if t is outside the curve.
static bool height_at(time_t t, float *out)
{
    for (int i = 1; i < s_nsamples; i++) {
        if (s_samples[i].t >= t && s_samples[i - 1].t <= t) {
            const tide_sample_t *a = &s_samples[i - 1], *b = &s_samples[i];
            float f = b->t == a->t ? 0 : (float)(t - a->t) / (float)(b->t - a->t);
            *out = a->height_ft + f * (b->height_ft - a->height_ft);
            return true;
        }
    }
    return false;
}

static void plot_area(lv_obj_t *obj, lv_area_t *a)
{
    lv_obj_get_content_coords(obj, a);
    a->x1 += AXIS_LEFT;
    a->y2 -= AXIS_BOTTOM;
    a->y1 += 34; // headroom for the high-tide labels
}

static float px_x(const lv_area_t *a, time_t t)
{
    return a->x1 + (float)(t - s_t0) / (float)(s_t1 - s_t0) * lv_area_get_width(a);
}

static float px_y(const lv_area_t *a, float h)
{
    return a->y2 - (h - s_hmin) / (s_hmax - s_hmin) * lv_area_get_height(a);
}

static void text_at(lv_layer_t *layer, const char *txt, int32_t cx, int32_t y, const lv_font_t *font, lv_color_t color)
{
    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.text = txt;
    l.text_local = 1;
    l.font = font;
    l.color = color;
    l.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a = {cx - 60, y, cx + 60, y + lv_font_get_line_height(font) * 2};
    lv_draw_label(layer, &l, &a);
}

static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, lv_color_t c, int w, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1 = (lv_point_precise_t){x1, y1};
    d.p2 = (lv_point_precise_t){x2, y2};
    d.color = c;
    d.width = w;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

static void graph_draw_cb(lv_event_t *e)
{
    if (s_nsamples < 2) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    plot_area(lv_event_get_target(e), &a);
    time_t now = time(NULL);

    // Horizontal grid with height labels.
    float step = s_hmax - s_hmin > 12 ? 4 : 2;
    for (float h = ceilf(s_hmin / step) * step; h <= s_hmax; h += step) {
        float y = px_y(&a, h);
        line(layer, a.x1, y, a.x2, y, UI_COLOR_PANEL, 1, LV_OPA_COVER);
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)h);
        text_at(layer, buf, a.x1 - AXIS_LEFT / 2 - 2, (int32_t)y - 9, &lv_font_montserrat_16, UI_COLOR_MUTED);
    }
    // Vertical grid every 6 hours of local time.
    for (time_t t = s_t0 - s_t0 % 3600 + 3600; t < s_t1; t += 3600) {
        struct tm tm;
        localtime_r(&t, &tm);
        if (tm.tm_hour % 6) continue;
        float x = px_x(&a, t);
        line(layer, x, a.y1, x, a.y2, UI_COLOR_PANEL, 1, LV_OPA_COVER);
        if (x > a.x2 - 30) continue; // label would hang off the end
        int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d %s", h12, tm.tm_hour < 12 ? "AM" : "PM");
        text_at(layer, buf, (int32_t)x, a.y2 + 6, &lv_font_montserrat_16, UI_COLOR_MUTED);
    }

    // Curve: past dimmed, future bright, with a light fill underneath.
    lv_draw_triangle_dsc_t fill;
    lv_draw_triangle_dsc_init(&fill);
    fill.color = UI_COLOR_ACCENT;
    for (int i = 1; i < s_nsamples; i++) {
        const tide_sample_t *p = &s_samples[i - 1], *q = &s_samples[i];
        if (q->t < s_t0 || p->t > s_t1) continue;
        float x1 = px_x(&a, p->t), y1 = px_y(&a, p->height_ft), x2 = px_x(&a, q->t), y2 = px_y(&a, q->height_ft);
        x1 = fmaxf(x1, a.x1);
        x2 = fminf(x2, a.x2);
        bool past = q->t <= now;
        fill.opa = past ? LV_OPA_10 : LV_OPA_20;
        fill.p[0] = (lv_point_precise_t){x1, y1};
        fill.p[1] = (lv_point_precise_t){x2, y2};
        fill.p[2] = (lv_point_precise_t){x2, a.y2};
        lv_draw_triangle(layer, &fill);
        fill.p[1] = (lv_point_precise_t){x2, a.y2};
        fill.p[2] = (lv_point_precise_t){x1, a.y2};
        lv_draw_triangle(layer, &fill);
        line(layer, x1, y1, x2, y2, past ? UI_COLOR_MUTED : UI_COLOR_ACCENT, 3, LV_OPA_COVER);
    }

    // Highs and lows.
    for (int i = 0; i < s_nevents; i++) {
        const tide_event_t *ev = &s_events[i];
        if (ev->t < s_t0 || ev->t > s_t1) continue;
        float x = px_x(&a, ev->t), y = px_y(&a, ev->height_ft);
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = UI_COLOR_TEXT;
        r.radius = LV_RADIUS_CIRCLE;
        lv_area_t dot = {(int32_t)x - 5, (int32_t)y - 5, (int32_t)x + 5, (int32_t)y + 5};
        lv_draw_rect(layer, &r, &dot);
        char t[16], buf[40];
        ui_format_time(ev->t, t, sizeof(t), false);
        snprintf(buf, sizeof(buf), "%.1f ft\n%s", ev->height_ft, t);
        // Highs labelled above the dot, lows below, unless that would run into the time axis.
        int32_t lh = 2 * lv_font_get_line_height(&lv_font_montserrat_14);
        bool above = ev->high || y + 8 + lh > a.y2;
        int32_t ty = above ? (int32_t)y - 8 - lh : (int32_t)y + 8;
        text_at(layer, buf, (int32_t)x, ty, &lv_font_montserrat_14, UI_COLOR_TEXT);
    }

    // Now.
    float h;
    float x = px_x(&a, now);
    line(layer, x, a.y1 - 10, x, a.y2, UI_COLOR_WARN, 2, LV_OPA_COVER);
    if (height_at(now, &h)) {
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = UI_COLOR_WARN;
        r.radius = LV_RADIUS_CIRCLE;
        float y = px_y(&a, h);
        lv_area_t dot = {(int32_t)x - 8, (int32_t)y - 8, (int32_t)x + 8, (int32_t)y + 8};
        lv_draw_rect(layer, &r, &dot);
    }
}

static void refresh(void)
{
    time_t now = time(NULL);
    s_t0 = now - PAST_S;
    s_t1 = now + FUTURE_S;
    s_nsamples = tide_curve(s_samples, MAX_SAMPLES);
    s_nevents = tide_events(s_events, MAX_EVENTS);

    tide_status_t st;
    tide_get_status(&st);
    if (st.station_name[0]) lv_label_set_text_fmt(s_title, "Tide at %s", st.station_name);
    else if (st.station[0]) lv_label_set_text_fmt(s_title, "Tide station %s", st.station);
    else lv_label_set_text(s_title, "Tide");

    // Vertical range from the samples on screen, padded and rounded to whole feet.
    s_hmin = INFINITY;
    s_hmax = -INFINITY;
    for (int i = 0; i < s_nsamples; i++) {
        if (s_samples[i].t < s_t0 - 3600 || s_samples[i].t > s_t1 + 3600) continue;
        s_hmin = fminf(s_hmin, s_samples[i].height_ft);
        s_hmax = fmaxf(s_hmax, s_samples[i].height_ft);
    }
    bool have = clock_is_set() && s_hmax >= s_hmin;
    if (have) {
        s_hmin = floorf(s_hmin - 0.5f);
        s_hmax = ceilf(s_hmax + 0.5f);
    }

    float h, h_next;
    if (have && height_at(now, &h) && height_at(now + 600, &h_next)) {
        lv_label_set_text_fmt(s_now, "Now %.1f ft, %s", h, h_next >= h ? "rising " LV_SYMBOL_UP : "falling " LV_SYMBOL_DOWN);
    } else {
        lv_label_set_text(s_now, "");
    }

    if (!have) {
        if (st.state == TIDE_NO_STATION) lv_label_set_text(s_msg, "No tide station set.\nChoose one on the config page.");
        else if (st.state == TIDE_ERROR) lv_label_set_text_fmt(s_msg, "Tide unavailable\n%s", st.error);
        else lv_label_set_text(s_msg, "Loading tide predictions...");
    }
    lv_obj_set_hidden(s_msg, have);
    lv_obj_set_hidden(s_graph, !have);
    lv_obj_invalidate(s_graph);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    if (!lv_obj_is_hidden(s_root)) refresh();
}

static void close_cb(lv_event_t *e)
{
    lv_obj_set_hidden(s_root, true);
}

lv_obj_t *ui_tide_create(lv_obj_t *parent)
{
    s_root = ui_overlay_create(parent, close_cb);

    s_title = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_title, UI_COLOR_MUTED, 0);
    lv_obj_set_width(s_title, 560);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 150);

    s_now = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_now, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_now, UI_COLOR_TEXT, 0);
    lv_obj_align(s_now, LV_ALIGN_TOP_MID, 0, 190);

    s_graph = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_graph);
    lv_obj_set_size(s_graph, GRAPH_W, GRAPH_H);
    lv_obj_align(s_graph, LV_ALIGN_CENTER, -AXIS_LEFT / 2, 70);
    lv_obj_set_clickable(s_graph, false);
    lv_obj_add_event_cb(s_graph, graph_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_msg = lv_label_create(s_root);
    lv_obj_set_style_text_color(s_msg, UI_COLOR_MUTED, 0);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_msg);

    lv_timer_create(refresh_timer_cb, REFRESH_MS, NULL);
    return s_root;
}

void ui_tide_open(void)
{
    refresh();
    lv_obj_set_hidden(s_root, false);
}
