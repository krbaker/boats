// Map screen: chart tiles (draggable), AIS targets, and pills around the
// round display's border for the time, tide, vessel count, zoom and recentre.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "geo.h"
#include "map_tiles.h"
#include "nmea_client.h"
#include "sdr_probe.h"
#include "settings.h"
#include "tide.h"
#include "ui.h"
#include "vessel_db.h"
#include "weather.h"
#include "wifi_mgr.h"
#include "wx_icon_render.h"

#define PILL_EDGE_GAP   4       // px between the pills and the edge of the screen
#define PILL_PAD_DEG    4       // extra arc either side of a pill's text
#define PILL_WIDTH      (lv_font_get_line_height(&lv_font_montserrat_28) + 10)  // every pill is this thick
#define MIN_SHIP_LEN    18      // px; ships are never drawn smaller than this
#define MIN_SHIP_WIDTH  8
#define HULL_TRUE_LEVEL 2       // hulls are to scale up to this zoom level (0 = closest), and no smaller beyond it
#define STALE_MOVING_S  180     // dim vessels not heard for this long...
#define STALE_SLOW_S    420     // ...or this long if slow / at rest: those report only every 3 minutes
#define SLOW_KN         2.0f    // AIS Class B (and anchored Class A) drop to 3-minute reports below ~2 kn
#define HEADING_MIN_KN  1.0f    // no course vector below this speed
#define HIT_RADIUS_MIN  22
#define VECTOR_MINUTES  6       // course/speed vector length
#define HALO_PX         2       // light rim outside each vessel's dark outline
#define DASH_PX         8
#define DASH_GAP_PX     6
#define DRAG_SLOP_PX    10      // finger movement before a touch becomes a drag
#define DEFAULT_PAN_NM  10.0f   // pan limit when there is no chart
#define TILE_POLL_MS    40

// Weather on the map (WEATHER_PLAN.md).
#define WX_ICON_PX      36      // the icon in the time bubble
#define WX_ICON_GAP_PX  8       // between the time and the icon
#define WX_ALERT_PX     14      // room for the red "!"
#define WX_STALE_S      (3 * 3600)  // weather older than this greys the icon
#define WIND_STALE_S    3600    // the wind arrow goes after an hour without a reading
#define WAVE_STALE_S    (3 * 3600)
#define GUST_SHOW_MPH   10      // show the gust when it's this far above the average
#define WIND_TAIL_R     356     // px from the centre: the wind arrow, just inside the bubbles...
#define WIND_HEAD_R     302
#define WIND_TIP_R      264
#define WIND_SHAFT_W    48
#define WIND_HEAD_W     78
#define WAVE_BASE_R     256     // ...and the wave triangle just inside that
#define WAVE_TIP_R      176
#define WAVE_BASE_W     112
#define WAVE_ICON_PX    26      // the wave icon in the tide bubble (buoy without wave direction)
#define WAVE_ICON_GAP   10
#define WIND_COLOR      lv_color_hex(0xF5D33F)
#define WAVE_COLOR      lv_color_hex(0x3FA7F5)

// Zoom levels when there is no chart.
static const float k_default_radii[] = {0.5f, 1.0f, 2.0f, 4.0f, 10.0f};

typedef struct {
    uint32_t mmsi;
    int16_t x, y, r;
} hit_t;

// Where a vessel ended up on screen, for placing its label afterwards.
typedef struct {
    const vessel_t *v;
    float prio;             // label priority
    float layer;            // drawing order: higher on top
    float x, y, extent;
    bool shown;     // on screen
    bool labelled;  // passes the filter, so it may get a label
    lv_opa_t opa;
} placed_t;

typedef struct {
    geo_bbox_t bbox;        // what the screen shows, EPSG:3857 metres
    double cx, cy;          // view centre, EPSG:3857 metres (moved by dragging)
    double home_x, home_y;  // the configured position
    double pan_x, pan_y;    // centre of the area the view may pan over (the chart's centre)
    double pan_half;        // half-width of that area, EPSG:3857 metres
    double merc_px;         // EPSG:3857 metres per pixel
    double scale;           // EPSG:3857 metres per ground metre around the chart
    float m_per_px;         // ground metres per pixel
    float hull_m_per_px;    // the scale hulls are drawn at: true to scale up to HULL_TRUE_LEVEL
    float radius_nm;
    int nlevels;
} view_t;

// A curved, round-ended strip around the edge of the screen holding a short
// piece of text. Pills are drawn on the overlay; the text is an arclabel on top.
typedef struct {
    lv_obj_t *lbl;
    char text[96];
    float center;           // degrees, 0 = 3 o'clock, clockwise
    float min_span;         // degrees
    float span;             // degrees covered by the text (plus padding); caps extend beyond
    int16_t width;          // radial thickness, px
    bool hidden, pressed, disabled;
    float icon_deg;         // room after the text for an icon (the weather in the time bubble)
    lv_obj_t *lbl2;         // optional second piece of text, after an icon (waves in the tide bubble)
    char text2[32];
    float mid_deg;          // room for the icon between the two pieces
    float mid_angle;        // where that icon goes, screen degrees
    int dir;                // +1 if the text reads clockwise, -1 if counter-clockwise
    void (*on_tap)(void);
} pill_t;

enum { PILL_TIME, PILL_TIDE, PILL_STATUS, PILL_ZOOM_IN, PILL_ZOOM, PILL_ZOOM_OUT, PILL_COUNT };

static lv_obj_t *s_scr, *s_maplayer, *s_overlay, *s_msg;
static const map_manifest_t *s_shown_manifest;
static struct {
    bool active;
    lv_point_t start;
    double cx, cy;
} s_drag;
static pill_t s_pills[PILL_COUNT];

static vessel_t *s_vessels;
static int s_nvessels;
static hit_t *s_hits;
static int s_nhits;
static placed_t *s_placed;
static placed_t **s_label_order;
static lv_area_t *s_label_areas;
static vessel_filter_t s_filter;
static int s_zoom;
static view_t s_view;
static EventGroupHandle_t s_ev;

// What the weather shows on the map, worked out from weather_get() once a
// second when it changes (and each minute, for sunset and the moon).
static struct {
    bool icon_on, grey, alert;
    wx_icon_t icon;
    bool wind_on;
    float wind_dir, wind_mph, gust_mph;
    bool wave_on;           // the triangle: waves with a direction
    float wave_dir, wave_ft, wave_s;
} s_wx;
static uint32_t s_wx_argb[WX_ICON_PX * WX_ICON_PX];
static lv_image_dsc_t s_wx_img;
static uint32_t s_wave_argb[WAVE_ICON_PX * WAVE_ICON_PX];
static lv_image_dsc_t s_wave_img;

// ---------------------------------------------------------------------------
// Edge pills
// ---------------------------------------------------------------------------

static inline int pill_outer_r(void) { return UI_CENTER - PILL_EDGE_GAP; }
static inline float px_to_deg(float px, float r) { return px / r * 180.0f / (float)M_PI; }

static lv_obj_t *pill_label(pill_t *p, const lv_font_t *font, float center, lv_arclabel_dir_t dir)
{
    lv_obj_t *a = lv_arclabel_create(s_scr);
    lv_obj_set_size(a, UI_SIZE, UI_SIZE);
    lv_obj_center(a);
    lv_obj_set_clickable(a, false);
    lv_obj_set_style_text_font(a, font, 0);
    lv_obj_set_style_text_color(a, lv_color_white(), 0);
    lv_arclabel_set_radius(a, pill_outer_r() - p->width / 2);
    lv_arclabel_set_angle_start(a, center - 60);
    lv_arclabel_set_angle_size(a, 120);
    lv_arclabel_set_dir(a, dir);
    lv_arclabel_set_text_vertical_align(a, LV_ARCLABEL_TEXT_ALIGN_CENTER);
    lv_arclabel_set_text_horizontal_align(a, LV_ARCLABEL_TEXT_ALIGN_CENTER);
    lv_arclabel_set_recolor(a, true);
    lv_arclabel_set_text(a, "");
    return a;
}

static void pill_create(pill_t *p, const lv_font_t *font, float center, float min_span, lv_arclabel_dir_t dir,
                        void (*on_tap)(void))
{
    p->center = center;
    p->min_span = p->span = min_span;
    p->width = PILL_WIDTH;
    p->on_tap = on_tap;
    p->hidden = true;
    p->dir = dir == LV_ARCLABEL_DIR_COUNTER_CLOCKWISE ? -1 : 1;
    p->lbl = pill_label(p, font, center, dir);
}

// The pill's length: its text, padding and any icon. The text sits left of
// centre by half the icon's room, so text and icon together are centred.
// The arclabel's own measure runs a little short of the drawn text (82 px
// drawn for 63 px measured, on "0.5 nm"): where an icon sits next to text,
// use the straight width too, so the icon never touches it.
static float text_deg(lv_obj_t *lbl, const char *text, const pill_t *p)
{
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(lbl, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return fmaxf((float)lv_arclabel_get_text_angle(lbl), px_to_deg(size.x, pill_outer_r() - p->width / 2.0f));
}

static void pill_layout(pill_t *p)
{
    if (p->text2[0]) {
        // Text, icon, second text, in reading order (which runs backwards in
        // screen angles along the bottom of the screen).
        float t1 = text_deg(p->lbl, p->text, p), t2 = text_deg(p->lbl2, p->text2, p);
        p->span = fmaxf(p->min_span, t1 + p->mid_deg + t2 + 2 * PILL_PAD_DEG);
        float start = p->center - p->dir * (p->span / 2 - PILL_PAD_DEG);
        lv_arclabel_set_angle_start(p->lbl, start + p->dir * t1 / 2 - 60);
        p->mid_angle = start + p->dir * (t1 + p->mid_deg / 2);
        lv_arclabel_set_angle_start(p->lbl2, start + p->dir * (t1 + p->mid_deg + t2 / 2) - 60);
    } else {
        float t = p->icon_deg ? text_deg(p->lbl, p->text, p) : (float)lv_arclabel_get_text_angle(p->lbl);
        p->span = fmaxf(p->min_span, t + 2 * PILL_PAD_DEG + p->icon_deg);
        lv_arclabel_set_angle_start(p->lbl, p->center - p->icon_deg / 2 - 60);
    }
    if (s_overlay) lv_obj_invalidate(s_overlay);
}

// A second piece of text after the first, with room for an icon between
// ("" removes it). The pill must have been given lbl2.
static void pill_set_text2(pill_t *p, const char *text, float mid_px)
{
    if (strcmp(p->text2, text) == 0) return;
    strlcpy(p->text2, text, sizeof(p->text2));
    lv_arclabel_set_text(p->lbl2, text);
    p->mid_deg = text[0] ? px_to_deg(mid_px, pill_outer_r() - p->width / 2.0f) : 0;
    pill_layout(p);
}

static void pill_set_text(pill_t *p, const char *text)
{
    if (strcmp(p->text, text) == 0 && p->hidden == !text[0]) return;
    strlcpy(p->text, text, sizeof(p->text));
    lv_arclabel_set_text(p->lbl, text);
    p->hidden = !text[0];
    pill_layout(p);
}

static void pill_set_icon_room(pill_t *p, float px)
{
    float deg = px ? px_to_deg(px, pill_outer_r() - p->width / 2.0f) : 0;
    if (fabsf(deg - p->icon_deg) < 0.01f) return;
    p->icon_deg = deg;
    pill_layout(p);
}

static void pill_set_disabled(pill_t *p, bool disabled)
{
    p->disabled = disabled;
    lv_obj_set_style_text_opa(p->lbl, disabled ? LV_OPA_30 : LV_OPA_COVER, 0);
}

static void draw_pills(lv_layer_t *layer)
{
    for (int i = 0; i < PILL_COUNT; i++) {
        const pill_t *p = &s_pills[i];
        if (p->hidden) continue;
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.center = (lv_point_t){UI_CENTER, UI_CENTER};
        d.start_angle = fmodf(p->center - p->span / 2 + 360, 360);
        d.end_angle = fmodf(p->center + p->span / 2 + 360, 360);
        d.rounded = 1;
        // Soft dark edge, then the body.
        d.radius = pill_outer_r() + 2;
        d.width = p->width + 4;
        d.color = lv_color_black();
        d.opa = LV_OPA_30;
        lv_draw_arc(layer, &d);
        d.radius = pill_outer_r();
        d.width = p->width;
        d.color = p->pressed && !p->disabled ? UI_COLOR_ACCENT : lv_color_black();
        d.opa = p->pressed && !p->disabled ? LV_OPA_80 : LV_OPA_60;
        lv_draw_arc(layer, &d);
    }
}

// ---------------------------------------------------------------------------
// Weather: the icon in the time bubble, the wind arrow and the wave triangle
// ---------------------------------------------------------------------------

// A point at a compass bearing (0 = north = up) and radius from the centre.
static lv_point_precise_t at_bearing(float bearing, float r)
{
    float a = (bearing - 90) * (float)M_PI / 180;
    return (lv_point_precise_t){UI_CENTER + r * cosf(a), UI_CENTER + r * sinf(a)};
}

static lv_point_precise_t offset(lv_point_precise_t p, float bearing, float side)
{
    // side > 0: to the right, looking in from the rim along the bearing.
    float a = (bearing - 90) * (float)M_PI / 180;
    return (lv_point_precise_t){p.x - side * sinf(a), p.y + side * cosf(a)};
}

static void fill_tri(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c,
                     lv_color_t color, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0] = a, d.p[1] = b, d.p[2] = c;
    d.color = color;
    d.opa = opa;
    lv_draw_triangle(layer, &d);
}

static void outline(lv_layer_t *layer, const lv_point_precise_t *pts, int n, lv_color_t color)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = 2;
    d.opa = LV_OPA_80;
    d.round_start = d.round_end = 1;
    for (int i = 0; i < n; i++) {
        d.p1 = pts[i];
        d.p2 = pts[(i + 1) % n];
        lv_draw_line(layer, &d);
    }
}

// Text centred on a point, upright, with a dark shadow so it reads on the colour.
static void text_at(lv_layer_t *layer, const char *text, const lv_font_t *font, lv_point_precise_t c)
{
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = text;
    d.font = font;
    d.align = LV_TEXT_ALIGN_CENTER;
    int32_t x = (int32_t)lroundf(c.x) - size.x / 2, y = (int32_t)lroundf(c.y) - size.y / 2;
    for (int i = 0; i < 2; i++) {
        lv_area_t a = {x + (i ? 0 : 1), y + (i ? 0 : 1), x + size.x + (i ? 0 : 1), y + size.y + (i ? 0 : 1)};
        d.color = i ? lv_color_white() : lv_color_black();
        d.opa = i ? LV_OPA_COVER : LV_OPA_70;
        lv_draw_label(layer, &d, &a);
    }
}

static void draw_wind(lv_layer_t *layer)
{
    if (!s_wx.wind_on) return;
    float b = s_wx.wind_dir;
    char speed[8], gust[8] = "";
    snprintf(speed, sizeof(speed), "%.0f", s_wx.wind_mph);
    if (s_wx.gust_mph - s_wx.wind_mph >= GUST_SHOW_MPH) snprintf(gust, sizeof(gust), "G%.0f", s_wx.gust_mph);
    if (s_wx.wind_mph < 1) { // calm: a small circle where the arrow would be
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        lv_point_precise_t c = at_bearing(b, (WIND_TAIL_R + WIND_HEAD_R) / 2.0f);
        d.center = (lv_point_t){(int32_t)c.x, (int32_t)c.y};
        d.radius = 20;
        d.width = 20;
        d.start_angle = 0;
        d.end_angle = 360;
        d.color = WIND_COLOR;
        d.opa = LV_OPA_50;
        lv_draw_arc(layer, &d);
        text_at(layer, "0", &lv_font_montserrat_20, c);
        return;
    }
    lv_point_precise_t tail = at_bearing(b, WIND_TAIL_R), head = at_bearing(b, WIND_HEAD_R), tip = at_bearing(b, WIND_TIP_R);
    lv_point_precise_t pts[7] = {
        offset(tail, b, -WIND_SHAFT_W / 2), offset(head, b, -WIND_SHAFT_W / 2), offset(head, b, -WIND_HEAD_W / 2), tip,
        offset(head, b, WIND_HEAD_W / 2),   offset(head, b, WIND_SHAFT_W / 2),  offset(tail, b, WIND_SHAFT_W / 2),
    };
    // The shaft as one thick line, not two triangles: translucent triangles
    // sharing an edge blend it twice and show a seam.
    lv_draw_line_dsc_t shaft;
    lv_draw_line_dsc_init(&shaft);
    shaft.p1 = tail;
    shaft.p2 = at_bearing(b, WIND_HEAD_R);
    shaft.width = WIND_SHAFT_W;
    shaft.color = WIND_COLOR;
    shaft.opa = LV_OPA_50;
    lv_draw_line(layer, &shaft);
    fill_tri(layer, pts[2], pts[3], pts[4], WIND_COLOR, LV_OPA_50); // the head
    outline(layer, pts, 7, WIND_COLOR);
    lv_point_precise_t mid = at_bearing(b, (WIND_TAIL_R + WIND_HEAD_R) / 2.0f + 2);
    if (gust[0]) {
        text_at(layer, speed, &lv_font_montserrat_20, (lv_point_precise_t){mid.x, mid.y - 9});
        text_at(layer, gust, &lv_font_montserrat_14, (lv_point_precise_t){mid.x, mid.y + 11});
    } else {
        text_at(layer, speed, &lv_font_montserrat_24, mid);
    }
}

static void draw_wave(lv_layer_t *layer)
{
    if (!s_wx.wave_on) return;
    float b = s_wx.wave_dir;
    lv_point_precise_t base = at_bearing(b, WAVE_BASE_R), tip = at_bearing(b, WAVE_TIP_R);
    lv_point_precise_t pts[3] = {offset(base, b, -WAVE_BASE_W / 2), tip, offset(base, b, WAVE_BASE_W / 2)};
    fill_tri(layer, pts[0], pts[1], pts[2], WAVE_COLOR, LV_OPA_40);
    outline(layer, pts, 3, WAVE_COLOR);
    char h[12], t[8] = "";
    snprintf(h, sizeof(h), "%.1f ft", s_wx.wave_ft);
    if (s_wx.wave_s > 0) snprintf(t, sizeof(t), "%.0f s", s_wx.wave_s);
    lv_point_precise_t c = at_bearing(b, WAVE_BASE_R - 26);
    text_at(layer, h, &lv_font_montserrat_16, (lv_point_precise_t){c.x, c.y - (t[0] ? 8 : 0)});
    if (t[0]) text_at(layer, t, &lv_font_montserrat_14, (lv_point_precise_t){c.x, c.y + 10});
}

// The icon (and "!") in the room pill_set_icon_room() left after the time.
static void draw_time_icon(lv_layer_t *layer)
{
    const pill_t *p = &s_pills[PILL_TIME];
    if (!s_wx.icon_on || p->hidden) return;
    float rm = pill_outer_r() - p->width / 2.0f;
    float end = p->center + p->span / 2 - PILL_PAD_DEG; // where the room ends, in screen degrees
    float icon_mid = end - px_to_deg((s_wx.alert ? WX_ALERT_PX : 0) + WX_ICON_PX / 2.0f, rm);
    float a = icon_mid * (float)M_PI / 180;
    int32_t cx = (int32_t)lroundf(UI_CENTER + rm * cosf(a)), cy = (int32_t)lroundf(UI_CENTER + rm * sinf(a));
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &s_wx_img;
    lv_area_t area = {cx - WX_ICON_PX / 2, cy - WX_ICON_PX / 2, cx + WX_ICON_PX / 2 - 1, cy + WX_ICON_PX / 2 - 1};
    lv_draw_image(layer, &d, &area);
    if (s_wx.alert) {
        float ea = (end - px_to_deg(WX_ALERT_PX / 2.0f, rm)) * (float)M_PI / 180;
        lv_point_precise_t c = {UI_CENTER + rm * cosf(ea), UI_CENTER + rm * sinf(ea)};
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.text = "!";
        ld.font = &lv_font_montserrat_28;
        ld.color = UI_COLOR_BAD;
        ld.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t la = {(int32_t)c.x - 10, (int32_t)c.y - 15, (int32_t)c.x + 10, (int32_t)c.y + 15};
        lv_draw_label(layer, &ld, &la);
    }
}

// The wave icon between the tide and the waves in the tide bubble.
static void draw_tide_wave_icon(lv_layer_t *layer)
{
    const pill_t *p = &s_pills[PILL_TIDE];
    if (p->hidden || !p->text2[0]) return;
    float rm = pill_outer_r() - p->width / 2.0f, a = p->mid_angle * (float)M_PI / 180;
    int32_t cx = (int32_t)lroundf(UI_CENTER + rm * cosf(a)), cy = (int32_t)lroundf(UI_CENTER + rm * sinf(a));
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &s_wave_img;
    lv_area_t area = {cx - WAVE_ICON_PX / 2, cy - WAVE_ICON_PX / 2, cx + WAVE_ICON_PX / 2 - 1, cy + WAVE_ICON_PX / 2 - 1};
    lv_draw_image(layer, &d, &area);
}

// Works out what the map shows from the latest weather and the time.
static void update_weather(void)
{
    static uint32_t last_gen;
    static time_t last_min;
    static weather_data_t *w;
    time_t now = time(NULL);
    if (weather_generation() == last_gen && now / 60 == last_min) return;
    last_gen = weather_generation();
    last_min = now / 60;
    if (!w) w = heap_caps_malloc(sizeof(*w), MALLOC_CAP_SPIRAM);
    if (!w || !clock_is_set()) return;
    weather_get(w);
    settings_t cfg;
    settings_get(&cfg);

    bool station_fresh = w->have_station && now - w->station.when < WX_STALE_S;
    bool obs_fresh = w->have_obs && now - w->obs.when < WX_STALE_S;
    float rain = station_fresh && w->station.has_rain ? w->station.rain_rate_in_hr : -1;
    wx_icon_t ic;
    wx_icon_decide(cfg.center_lat, cfg.center_lon, now, w->have_obs ? &w->obs : NULL, rain,
                   w->alert_flag && w->alerts_src.state == WX_OK, &ic);
    ic.moon_illum = roundf(ic.moon_illum * 50) / 50; // redraw for 2% steps, not every minute
    bool grey = (w->have_station || w->have_obs) && !station_fresh && !obs_fresh;
    if (!s_wx.icon_on || memcmp(&ic, &s_wx.icon, sizeof(ic)) != 0 || grey != s_wx.grey) {
        wx_icon_render(&ic, grey, s_wx_argb, WX_ICON_PX); // drawn straight from the buffer: the image cache is off
    }
    s_wx.icon = ic;
    s_wx.grey = grey;
    s_wx.alert = ic.alert;
    s_wx.icon_on = true;
    pill_set_icon_room(&s_pills[PILL_TIME], WX_ICON_GAP_PX + WX_ICON_PX + (ic.alert ? WX_ALERT_PX : 0));

    // Wind: the station's 10-minute average, or else the buoy's.
    s_wx.wind_on = false;
    if (w->have_station && w->station.has_wind && now - w->station.when < WIND_STALE_S) {
        s_wx.wind_on = true;
        s_wx.wind_dir = w->station.wind_dir_avg10;
        s_wx.wind_mph = w->station.wind_avg10_mph;
        s_wx.gust_mph = w->station.gust10_mph;
    } else if (w->have_buoy && w->buoy.wind_mph.ok && w->buoy.wind_dir.ok && now - w->buoy.wind_mph.when < WIND_STALE_S) {
        s_wx.wind_on = true;
        s_wx.wind_dir = w->buoy.wind_dir.v;
        s_wx.wind_mph = w->buoy.wind_mph.v;
        s_wx.gust_mph = w->buoy.gust_mph.ok ? w->buoy.gust_mph.v : 0;
    }
    // Waves: the triangle if the buoy gives their direction; if it only gives
    // height and period, those go in the tide bubble instead.
    const buoy_t *bu = &w->buoy;
    bool height = w->have_buoy && bu->wave_ft.ok && now - bu->wave_ft.when < WAVE_STALE_S;
    bool dir = height && bu->wave_dir.ok && now - bu->wave_dir.when < WAVE_STALE_S;
    bool period = height && bu->dom_period_s.ok && now - bu->dom_period_s.when < WAVE_STALE_S;
    s_wx.wave_on = dir;
    s_wx.wave_dir = bu->wave_dir.v;
    s_wx.wave_ft = bu->wave_ft.v;
    s_wx.wave_s = period ? bu->dom_period_s.v : 0;
    char waves[32] = "";
    if (height && !dir) {
        if (period) snprintf(waves, sizeof(waves), "%.1f ft  %.0f s", bu->wave_ft.v, bu->dom_period_s.v);
        else snprintf(waves, sizeof(waves), "%.1f ft", bu->wave_ft.v);
    }
    pill_set_text2(&s_pills[PILL_TIDE], waves, WAVE_ICON_GAP * 2 + WAVE_ICON_PX);
    if (s_overlay) lv_obj_invalidate(s_overlay);
}

// Tappable pill under the point, or NULL.
static pill_t *pill_at(lv_point_t pt)
{
    float dx = pt.x - UI_CENTER, dy = pt.y - UI_CENTER, r = hypotf(dx, dy);
    float ang = atan2f(dy, dx) * 180.0f / (float)M_PI;
    for (int i = 0; i < PILL_COUNT; i++) {
        pill_t *p = &s_pills[i];
        if (!p->on_tap || p->hidden) continue;
        if (r < pill_outer_r() - p->width - 12) continue; // a little slop inwards for fingers
        float cap = px_to_deg(p->width / 2.0f, pill_outer_r() - p->width / 2.0f);
        float diff = fabsf(fmodf(ang - p->center + 540, 360) - 180);
        if (diff <= p->span / 2 + cap + 2) return p;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// View geometry
// ---------------------------------------------------------------------------

static inline double merc_scale(double lat)
{
    return 1.0 / cos(lat * M_PI / 180.0); // Mercator metres per ground metre
}

// Recompute the view for the current zoom level and centre, keeping the
// centre inside the pan area.
static void update_view(void)
{
    const map_manifest_t *m = map_tiles_manifest();
    if (m) {
        s_view.nlevels = m->nlevels;
        if (s_zoom >= m->nlevels) s_zoom = m->nlevels - 1;
        const map_level_t *l = &m->levels[s_zoom];
        s_view.merc_px = l->merc_px;
        s_view.m_per_px = l->m_per_px;
        s_view.radius_nm = l->radius_nm;
        s_view.hull_m_per_px = m->levels[HULL_TRUE_LEVEL < m->nlevels ? HULL_TRUE_LEVEL : m->nlevels - 1].m_per_px;
    } else {
        s_view.nlevels = sizeof(k_default_radii) / sizeof(k_default_radii[0]);
        if (s_zoom >= s_view.nlevels) s_zoom = s_view.nlevels - 1;
        float r = k_default_radii[s_zoom];
        s_view.m_per_px = 2 * r * 1852.0f / UI_SIZE;
        s_view.merc_px = s_view.m_per_px * s_view.scale;
        s_view.radius_nm = r;
        s_view.hull_m_per_px = 2 * k_default_radii[HULL_TRUE_LEVEL] * 1852.0f / UI_SIZE;
    }

    double half = UI_CENTER * s_view.merc_px;
    double lim = fmax(0, s_view.pan_half - half);
    s_view.cx = fmin(fmax(s_view.cx, s_view.pan_x - lim), s_view.pan_x + lim);
    s_view.cy = fmin(fmax(s_view.cy, s_view.pan_y - lim), s_view.pan_y + lim);
    s_view.bbox = (geo_bbox_t){s_view.cx - half, s_view.cy - half, s_view.cx + half, s_view.cy + half};

    char buf[16];
    snprintf(buf, sizeof(buf), "%g nm", s_view.radius_nm);
    pill_set_text(&s_pills[PILL_ZOOM], buf);
    pill_set_disabled(&s_pills[PILL_ZOOM_IN], s_zoom == 0);
    pill_set_disabled(&s_pills[PILL_ZOOM_OUT], s_zoom >= s_view.nlevels - 1);

    lv_obj_invalidate(s_maplayer);
    lv_obj_invalidate(s_overlay);
}

// Back to the configured position, e.g. after a new chart or centre.
static void view_go_home(void)
{
    settings_t cfg;
    settings_get(&cfg);
    geo_mercator(cfg.center_lat, cfg.center_lon, &s_view.home_x, &s_view.home_y);
    const map_manifest_t *m = map_tiles_manifest();
    double lat = m ? m->center_lat : cfg.center_lat;
    if (m) geo_mercator(m->center_lat, m->center_lon, &s_view.pan_x, &s_view.pan_y);
    else {
        s_view.pan_x = s_view.home_x;
        s_view.pan_y = s_view.home_y;
    }
    s_view.scale = merc_scale(lat);
    s_view.pan_half = (m ? m->pan_nm : DEFAULT_PAN_NM) * 1852.0 * s_view.scale;
    s_view.cx = s_view.home_x;
    s_view.cy = s_view.home_y;
    update_view();
}

// ---------------------------------------------------------------------------
// Chart tiles
// ---------------------------------------------------------------------------

// Does the w x h rectangle at (x, y) touch the round screen?
static bool on_screen(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t nx = LV_CLAMP(x, UI_CENTER, x + w), ny = LV_CLAMP(y, UI_CENTER, y + h);
    int32_t dx = nx - UI_CENTER, dy = ny - UI_CENTER;
    return dx * dx + dy * dy <= UI_CENTER * UI_CENTER;
}

static void map_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const map_manifest_t *m = map_tiles_manifest();
    map_tiles_frame_begin();
    if (m && map_tiles_state() == MAP_TILES_READY) {
        const map_level_t *l = &m->levels[s_zoom];
        double tile_m = MAP_TILE_PX * l->merc_px;
        // Screen position of the grid's top-left corner; every tile is a whole number of pixels from it.
        int32_t ox = (int32_t)lround((l->origin_x - s_view.bbox.x_min) / l->merc_px);
        int32_t oy = (int32_t)lround((s_view.bbox.y_max - l->origin_y) / l->merc_px);
        int c0 = (int)floor((s_view.bbox.x_min - l->origin_x) / tile_m);
        int c1 = (int)floor((s_view.bbox.x_max - l->origin_x) / tile_m);
        int r0 = (int)floor((l->origin_y - s_view.bbox.y_max) / tile_m);
        int r1 = (int)floor((l->origin_y - s_view.bbox.y_min) / tile_m);
        for (int r = r0 - 1; r <= r1 + 1; r++) {
            for (int c = c0 - 1; c <= c1 + 1; c++) {
                if (c < 0 || r < 0 || c >= l->cols || r >= l->rows) continue;
                int32_t x = ox + c * MAP_TILE_PX, y = oy + r * MAP_TILE_PX;
                bool visible = r >= r0 && r <= r1 && c >= c0 && c <= c1 && on_screen(x, y, MAP_TILE_PX, MAP_TILE_PX);
                if (!visible) {
                    map_tiles_prefetch(s_zoom, c, r);
                    continue;
                }
                const lv_image_dsc_t *img = map_tiles_get(s_zoom, c, r);
                if (!img) continue;
                lv_draw_image_dsc_t d;
                lv_draw_image_dsc_init(&d);
                d.src = img;
                lv_area_t a = {x, y, x + MAP_TILE_PX - 1, y + MAP_TILE_PX - 1};
                lv_draw_image(layer, &d, &a);
            }
        }
    }
    map_tiles_frame_end();
}

static void tile_poll_cb(lv_timer_t *t)
{
    if (!map_tiles_poll()) return;
    if (map_tiles_manifest() != s_shown_manifest) {
        s_shown_manifest = map_tiles_manifest();
        view_go_home();
    }
    lv_obj_invalidate(s_maplayer);
}

static void set_zoom(int z)
{
    if (z < 0 || z >= s_view.nlevels || z == s_zoom) return;
    s_zoom = z;
    settings_t cfg;
    settings_get(&cfg);
    cfg.zoom_index = (uint8_t)z;
    settings_set(&cfg);
    update_view();
}

// ---------------------------------------------------------------------------
// Ship drawing
// ---------------------------------------------------------------------------

static inline lv_point_precise_t rot(float lx, float ly, float s, float c, float x0, float y0)
{
    // Rotate a hull-frame point (x = starboard, y = aft) clockwise by the heading.
    return (lv_point_precise_t){x0 + lx * c - ly * s, y0 + lx * s + ly * c};
}

static void draw_line(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_color_t color, int w, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1 = a;
    d.p2 = b;
    d.color = color;
    d.width = w;
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(layer, &d);
}

// LVGL's software renderer only dashes horizontal / vertical lines, so dash by hand.
static void draw_dashed_line(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_color_t color, int w,
                             lv_opa_t opa)
{
    float dx = b.x - a.x, dy = b.y - a.y, len = hypotf(dx, dy);
    if (len < 1) return;
    dx /= len;
    dy /= len;
    for (float t = 0; t < len; t += DASH_PX + DASH_GAP_PX) {
        float e = fminf(t + DASH_PX, len);
        draw_line(layer, (lv_point_precise_t){a.x + dx * t, a.y + dy * t}, (lv_point_precise_t){a.x + dx * e, a.y + dy * e},
                  color, w, opa);
    }
}

static void draw_triangle(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c,
                          lv_color_t color, lv_opa_t opa)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0] = a;
    d.p[1] = b;
    d.p[2] = c;
    d.color = color;
    d.opa = opa;
    lv_draw_triangle(layer, &d);
}

static bool vessel_passes_filter(const vessel_t *v)
{
    const vessel_filter_t *f = &s_filter;
    if (f->min_len_ft) {
        if ((v->fields & AIS_F_DIM) && v->dim_a + v->dim_b > 0) {
            if ((v->dim_a + v->dim_b) * 3.28084f < f->min_len_ft) return false;
        } else if (!f->include_unknown) {
            return false;
        }
    }
    if (f->min_speed_kn) {
        if (v->fields & AIS_F_SOG) {
            if (v->sog_kn < f->min_speed_kn) return false;
        } else if (!f->include_unknown) {
            return false;
        }
    }
    return true;
}

// A vessel goes dim once it has missed a couple of its expected reports.
static bool vessel_stale(const vessel_t *v, int64_t now_us)
{
    bool slow = !(v->fields & AIS_F_SOG) || v->sog_kn < SLOW_KN;
    return now_us - v->last_heard_us > (int64_t)(slow ? STALE_SLOW_S : STALE_MOVING_S) * 1000000;
}

// Label priority: higher wins label space. Stale vessels always lose.
static float vessel_priority(const vessel_t *v, int64_t now_us)
{
    float p;
    if (s_filter.order == ORDER_SPEED) p = (v->fields & AIS_F_SOG) ? v->sog_kn : -1;
    else p = ((v->fields & AIS_F_DIM) && v->dim_a + v->dim_b) ? v->dim_a + v->dim_b : -1;
    if (vessel_stale(v, now_us)) p -= 100000;
    return p;
}

// Drawing order: stale vessels at the bottom, then those at rest, then vessels
// under way on top, so a moving vessel is never hidden under a stationary one.
static float vessel_layer(const vessel_t *v, float prio, int64_t now_us)
{
    int tier = vessel_stale(v, now_us) ? 0 : ((v->fields & AIS_F_SOG) && v->sog_kn >= HEADING_MIN_KN) ? 2 : 1;
    return tier * 1000000.0f + prio;
}

static int placed_cmp(const void *a, const void *b)
{
    float la = ((const placed_t *)a)->layer, lb = ((const placed_t *)b)->layer;
    return (la > lb) - (la < lb);
}

static int label_cmp(const void *a, const void *b)
{
    float pa = (*(const placed_t *const *)a)->prio, pb = (*(const placed_t *const *)b)->prio;
    return (pb > pa) - (pb < pa); // highest first
}

static void draw_label(lv_layer_t *layer, const char *text, const lv_area_t *bg, lv_opa_t opa)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_black();
    r.bg_opa = opa * 55 / 100;
    r.radius = 4;
    lv_draw_rect(layer, &r, bg);

    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.text = text;
    l.font = &lv_font_montserrat_14;
    l.color = lv_color_white();
    l.opa = opa;
    l.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a = {bg->x1, bg->y1 + 1, bg->x2, bg->y2};
    lv_draw_label(layer, &l, &a);
}

// Label text for a vessel per the label options; empty if none.
static void label_text(const vessel_t *v, char *text, size_t cap)
{
    char name[24] = "";
    if (s_filter.label_name) {
        if (v->name[0]) strlcpy(name, v->name, sizeof(name));
        else snprintf(name, sizeof(name), "%lu", (unsigned long)v->mmsi);
    }
    bool speed = s_filter.label_speed && (v->fields & AIS_F_SOG);
    if (name[0] && speed) snprintf(text, cap, "%s\n%.1f kn", name, v->sog_kn);
    else if (speed) snprintf(text, cap, "%.1f kn", v->sog_kn);
    else strlcpy(text, name, cap);
}

static void draw_vessel(lv_layer_t *layer, placed_t *pl, int64_t now_us)
{
    const vessel_t *v = pl->v;
    float x, y;
    geo_project(&s_view.bbox, UI_SIZE, UI_SIZE, v->lat, v->lon, &x, &y);
    float dx = x - UI_CENTER, dy = y - UI_CENTER;
    pl->shown = false;
    if (dx * dx + dy * dy > (float)(UI_CENTER + 40) * (UI_CENTER + 40)) return; // off the round screen

    bool stale = vessel_stale(v, now_us);
    lv_opa_t opa = stale ? LV_OPA_50 : LV_OPA_COVER;
    lv_color_t color = ui_ship_color(v->ship_type);
    lv_color_t outline = lv_color_hex(0x101010);
    // A light rim outside the dark outline: light-dark-colour reads against
    // white water, tan land and blue shallows alike.
    lv_color_t halo = lv_color_white();
    lv_opa_t halo_opa = (lv_opa_t)(opa * 85 / 100);

    bool moving = (v->fields & AIS_F_SOG) && v->sog_kn >= HEADING_MIN_KN;
    bool has_dir = (v->fields & AIS_F_HEADING) || ((v->fields & AIS_F_COG) && moving);
    float dir = (v->fields & AIS_F_HEADING) ? v->heading : v->cog_deg;

    // Size from the reported dimensions, with a floor so small boats stay
    // visible and touchable. Hulls keep the size they have at the 0.5 nm zoom
    // when zoomed further out (so they look oversized there), and are only
    // true to scale at that zoom and closer.
    float hull_m_per_px = fminf(s_view.m_per_px, s_view.hull_m_per_px);
    float len = MIN_SHIP_LEN, wid = MIN_SHIP_WIDTH;
    float off_x = 0, off_y = 0; // hull centre relative to the GPS antenna, hull frame
    if (v->fields & AIS_F_DIM) {
        float lm = v->dim_a + v->dim_b;
        float wm = v->dim_c + v->dim_d ? (float)(v->dim_c + v->dim_d) : lm * 0.3f;
        float scaled_len = lm / hull_m_per_px;
        len = fmaxf(scaled_len, MIN_SHIP_LEN);
        wid = fmaxf(wm / hull_m_per_px, MIN_SHIP_WIDTH);
        if (wid > len * 0.6f) wid = len * 0.6f;
        if (scaled_len > MIN_SHIP_LEN) {
            off_y = -((float)v->dim_a - v->dim_b) / 2 / hull_m_per_px;
            off_x = ((float)v->dim_d - v->dim_c) / 2 / hull_m_per_px;
        }
    }

    // Course-over-ground vector: where the vessel will be in VECTOR_MINUTES. It
    // differs from the hull's heading when current or wind sets the vessel
    // sideways. Dashed, so it doesn't read as part of the hull.
    if ((v->fields & AIS_F_COG) && moving) {
        float vlen = v->sog_kn * 1852.0f * VECTOR_MINUTES / 60.0f / s_view.m_per_px;
        if (vlen > 160) vlen = 160;
        float s = sinf(v->cog_deg * (float)M_PI / 180), c = cosf(v->cog_deg * (float)M_PI / 180);
        lv_point_precise_t a = {x, y}, b = {x + s * vlen, y - c * vlen};
        draw_dashed_line(layer, a, b, halo, 2 + 2 * HALO_PX, halo_opa);
        draw_dashed_line(layer, a, b, color, 2, opa);
    }

    float extent;
    if (has_dir) {
        float s = sinf(dir * (float)M_PI / 180), c = cosf(dir * (float)M_PI / 180);
        float hl = len / 2, hw = wid / 2;
        float shoulder = -hl + fminf(wid * 1.2f, len * 0.4f);
        lv_point_precise_t bow = rot(off_x, off_y - hl, s, c, x, y);
        lv_point_precise_t sb_sh = rot(off_x + hw, off_y + shoulder, s, c, x, y);
        lv_point_precise_t sb_st = rot(off_x + hw, off_y + hl, s, c, x, y);
        lv_point_precise_t pt_st = rot(off_x - hw, off_y + hl, s, c, x, y);
        lv_point_precise_t pt_sh = rot(off_x - hw, off_y + shoulder, s, c, x, y);
        lv_point_precise_t poly[] = {bow, sb_sh, sb_st, pt_st, pt_sh, bow};
        for (int i = 0; i < 5; i++) draw_line(layer, poly[i], poly[i + 1], halo, 2 + 2 * HALO_PX, halo_opa);
        draw_triangle(layer, bow, sb_sh, sb_st, color, opa);
        draw_triangle(layer, bow, sb_st, pt_st, color, opa);
        draw_triangle(layer, bow, pt_st, pt_sh, color, opa);
        for (int i = 0; i < 5; i++) draw_line(layer, poly[i], poly[i + 1], outline, 2, opa);
        extent = hypotf(hl + fabsf(off_y), hw + fabsf(off_x));
    } else {
        // No heading known (stationary): draw a dot.
        float d = fmaxf(wid, 14);
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = halo;
        r.bg_opa = halo_opa;
        r.radius = LV_RADIUS_CIRCLE;
        float hd = d + 2 * HALO_PX;
        lv_area_t ha = {(int32_t)(x - hd / 2), (int32_t)(y - hd / 2), (int32_t)(x + hd / 2), (int32_t)(y + hd / 2)};
        lv_draw_rect(layer, &r, &ha);
        r.bg_color = color;
        r.bg_opa = opa;
        r.radius = LV_RADIUS_CIRCLE;
        r.border_color = outline;
        r.border_width = 2;
        r.border_opa = opa;
        lv_area_t a = {(int32_t)(x - d / 2), (int32_t)(y - d / 2), (int32_t)(x + d / 2), (int32_t)(y + d / 2)};
        lv_draw_rect(layer, &r, &a);
        extent = d / 2 + HALO_PX;
    }

    pl->shown = true;
    pl->x = x;
    pl->y = y;
    pl->extent = extent;
    pl->opa = opa;

    hit_t *h = &s_hits[s_nhits++];
    h->mmsi = v->mmsi;
    h->x = (int16_t)x;
    h->y = (int16_t)y;
    h->r = (int16_t)fmaxf(extent, HIT_RADIUS_MIN);
}

static void draw_labels(lv_layer_t *layer, int n)
{
    int nplaced = 0;
    // Most important first, so they get the space when labels would overlap.
    for (int i = 0; i < n; i++) s_label_order[i] = &s_placed[i];
    qsort(s_label_order, n, sizeof(s_label_order[0]), label_cmp);
    for (int i = 0; i < n; i++) {
        const placed_t *pl = s_label_order[i];
        if (!pl->shown || !pl->labelled) continue;
        char text[48];
        label_text(pl->v, text, sizeof(text));
        if (!text[0]) continue;
        lv_point_t size;
        lv_text_get_size(&size, text, &lv_font_montserrat_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int32_t cx = (int32_t)pl->x, top = (int32_t)(pl->y + pl->extent + 4);
        lv_area_t bg = {cx - size.x / 2 - 4, top, cx + size.x / 2 + 4, top + size.y + 3};
        bool clear = true;
        for (int j = 0; j < nplaced && clear; j++) {
            const lv_area_t *o = &s_label_areas[j];
            clear = bg.x2 < o->x1 || bg.x1 > o->x2 || bg.y2 < o->y1 || bg.y1 > o->y2;
        }
        if (!clear) continue;
        s_label_areas[nplaced++] = bg;
        draw_label(layer, text, &bg, pl->opa);
    }
}

static void overlay_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    int64_t now = esp_timer_get_time();
    s_nhits = 0;
    int n = 0;
    for (int i = 0; i < s_nvessels; i++) {
        const vessel_t *v = &s_vessels[i];
        if (!(v->fields & AIS_F_POS)) continue;
        bool pass = vessel_passes_filter(v);
        if (!pass && s_filter.mode == FILTER_HIDE_VESSEL) continue;
        float prio = vessel_priority(v, now);
        s_placed[n++] = (placed_t){.v = v, .prio = prio, .layer = vessel_layer(v, prio, now), .labelled = pass};
    }
    // Bottom layer first.
    qsort(s_placed, n, sizeof(s_placed[0]), placed_cmp);
    for (int i = 0; i < n; i++) draw_vessel(layer, &s_placed[i], now);
    draw_labels(layer, n);
    draw_wave(layer);
    draw_wind(layer);
    draw_pills(layer);
    draw_time_icon(layer);
    draw_tide_wave_icon(layer);
}

static void set_pressed_pill(pill_t *hit)
{
    for (int i = 0; i < PILL_COUNT; i++) {
        bool want = &s_pills[i] == hit;
        if (s_pills[i].pressed != want) {
            s_pills[i].pressed = want;
            lv_obj_invalidate(s_overlay);
        }
    }
}

// Pill highlight while pressed, and one-finger dragging of the chart.
static void overlay_press_cb(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_drag.active = false;
        s_drag.start = p;
        s_drag.cx = s_view.cx;
        s_drag.cy = s_view.cy;
        set_pressed_pill(pill_at(p));
        break;
    case LV_EVENT_PRESSING: {
        int32_t dx = p.x - s_drag.start.x, dy = p.y - s_drag.start.y;
        if (!s_drag.active && (LV_ABS(dx) > DRAG_SLOP_PX || LV_ABS(dy) > DRAG_SLOP_PX)) {
            s_drag.active = true;
            set_pressed_pill(NULL);
        }
        if (s_drag.active) {
            s_view.cx = s_drag.cx - dx * s_view.merc_px;
            s_view.cy = s_drag.cy + dy * s_view.merc_px; // screen y runs down, Mercator y up
            update_view();
        }
        break;
    }
    default:
        set_pressed_pill(NULL);
        break;
    }
}

static void overlay_click_cb(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (s_drag.active) return; // the end of a drag, not a tap
    pill_t *pill = pill_at(p);
    if (pill) {
        if (!pill->disabled) pill->on_tap();
        return;
    }
    int best = -1;
    int32_t best_d2 = INT32_MAX;
    for (int i = 0; i < s_nhits; i++) {
        int32_t dx = p.x - s_hits[i].x, dy = p.y - s_hits[i].y;
        int32_t d2 = dx * dx + dy * dy;
        if (d2 <= s_hits[i].r * s_hits[i].r && d2 < best_d2) {
            best = i;
            best_d2 = d2;
        }
    }
    if (best >= 0) ui_show_detail(s_hits[best].mmsi);
}

// ---------------------------------------------------------------------------
// Border text and status
// ---------------------------------------------------------------------------

static void set_text_if_changed(lv_obj_t *obj, char *last, size_t cap, const char *text)
{
    if (strcmp(last, text) == 0) return;
    strlcpy(last, text, cap);
    lv_label_set_text(obj, text);
}

static void update_border_text(void)
{
    static char last_msg[160];
    char buf[160];
    time_t now = time(NULL);

    if (clock_is_set()) {
        ui_format_time(now, buf, sizeof(buf), false);
    } else {
        strcpy(buf, "--:--");
    }
    pill_set_text(&s_pills[PILL_TIME], buf);

    tide_status_t ts;
    tide_get_status(&ts);
    tide_event_t next;
    if (ts.state == TIDE_NO_STATION) {
        strcpy(buf, "Tide: no station set");
    } else if (clock_is_set() && tide_next(now, &next)) {
        // Level now, rising / falling, then the next high or low.
        char t[16], level[16] = "";
        float h;
        ui_format_time(next.t, t, sizeof(t), false);
        if (tide_level(now, &h)) snprintf(level, sizeof(level), "%.1f ft  ", h);
        snprintf(buf, sizeof(buf), "%s%s  %s  %.1f ft", level, next.high ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, t,
                 next.height_ft);
    } else if (ts.state == TIDE_ERROR) {
        strcpy(buf, "Tide unavailable");
    } else {
        strcpy(buf, "Tide: loading...");
    }
    pill_set_text(&s_pills[PILL_TIDE], buf);

    // Vessel count while either AIS source (network feed, RTL-SDR) is live.
    nmea_status_t ns;
    nmea_client_get_status(&ns);
    sdr_status_t ss;
    sdr_get_status(&ss);
    bool sdr_live = ss.state == SDR_READY || ss.state == SDR_CAPTURING;
    if (ns.state == NMEA_STATE_CONNECTED || sdr_live) {
        snprintf(buf, sizeof(buf), "%d vessel%s", s_nvessels, s_nvessels == 1 ? "" : "s");
    } else if ((ns.state == NMEA_STATE_NOT_CONFIGURED || ns.state == NMEA_STATE_OFF) && ss.state != SDR_TESTING) {
        strcpy(buf, "#f5b13f AIS not set#");
    } else {
        strcpy(buf, "#f2545b AIS offline#");
    }
    pill_set_text(&s_pills[PILL_STATUS], buf);

    // Centre message for setup problems.
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    const char *ip = ws.state == WIFI_MGR_AP ? ws.ap_ip : ws.sta_ip;
    buf[0] = '\0';
    if (ws.state == WIFI_MGR_AP) {
        snprintf(buf, sizeof(buf), "Setup mode: join Wi-Fi \"%s\"\npassword %s\nthen open http://%s/", ws.ap_ssid,
                 ws.ap_pass, ws.ap_ip);
    } else if (map_tiles_state() == MAP_TILES_NO_CARD) {
        strcpy(buf, "Insert a microSD card with a chart,\nthen restart the display");
    } else if (map_tiles_state() == MAP_TILES_LOADING) {
        strcpy(buf, "Loading chart...");
    } else if (map_tiles_state() == MAP_TILES_ERROR) {
        strcpy(buf, "The chart on the microSD card can't be read.\nRebuild it and upload it again.");
    } else if (map_tiles_state() == MAP_TILES_NONE) {
        snprintf(buf, sizeof(buf), "No chart on the microSD card%s%s%s", ip[0] ? "\nUpload one at http://" : "", ip,
                 ip[0] ? "/" : "");
    }
    set_text_if_changed(s_msg, last_msg, sizeof(last_msg), buf);
    if (buf[0]) lv_obj_set_hidden(s_msg, false);
    else lv_obj_set_hidden(s_msg, true);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    EventBits_t bits = xEventGroupClearBits(s_ev, SETTINGS_EV_MAP | SETTINGS_EV_CENTER | SETTINGS_EV_DISPLAY);
    if (bits & SETTINGS_EV_MAP) map_tiles_reload();
    if (bits & SETTINGS_EV_DISPLAY) {
        settings_t cfg;
        settings_get(&cfg);
        bsp_display_brightness_set(cfg.brightness);
    }
    if (bits & SETTINGS_EV_CENTER) view_go_home();

    s_nvessels = vessel_db_snapshot(s_vessels, VESSEL_MAX);
    if (lv_screen_active() == s_scr) lv_obj_invalidate(s_overlay);
    update_border_text();
    update_weather();
}

static void zoom_in(void) { set_zoom(s_zoom - 1); }
static void zoom_out(void) { set_zoom(s_zoom + 1); }

// ---------------------------------------------------------------------------

lv_obj_t *ui_map_create(void)
{
    s_vessels = heap_caps_malloc(VESSEL_MAX * sizeof(vessel_t), MALLOC_CAP_SPIRAM);
    s_hits = heap_caps_malloc(VESSEL_MAX * sizeof(hit_t), MALLOC_CAP_SPIRAM);
    s_placed = heap_caps_malloc(VESSEL_MAX * sizeof(placed_t), MALLOC_CAP_SPIRAM);
    s_label_order = heap_caps_malloc(VESSEL_MAX * sizeof(placed_t *), MALLOC_CAP_SPIRAM);
    s_label_areas = heap_caps_malloc(VESSEL_MAX * sizeof(lv_area_t), MALLOC_CAP_SPIRAM);
    s_ev = settings_subscribe();
    settings_t cfg;
    settings_get(&cfg);
    s_zoom = cfg.zoom_index;
    s_filter = cfg.filter;
    bsp_display_brightness_set(cfg.brightness);

    s_wx_img = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = WX_ICON_PX, .h = WX_ICON_PX,
                   .stride = WX_ICON_PX * 4},
        .data_size = sizeof(s_wx_argb),
        .data = (const uint8_t *)s_wx_argb,
    };

    wx_wave_render(s_wave_argb, WAVE_ICON_PX);
    s_wave_img = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = WAVE_ICON_PX, .h = WAVE_ICON_PX,
                   .stride = WAVE_ICON_PX * 4},
        .data_size = sizeof(s_wave_argb),
        .data = (const uint8_t *)s_wave_argb,
    };

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x16324f), 0);
    lv_obj_set_scrollable(s_scr, false);

    s_maplayer = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_maplayer);
    lv_obj_set_size(s_maplayer, UI_SIZE, UI_SIZE);
    lv_obj_set_clickable(s_maplayer, false);
    lv_obj_add_event_cb(s_maplayer, map_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_overlay = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, UI_SIZE, UI_SIZE);
    lv_obj_set_clickable(s_overlay, true);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_add_event_cb(s_overlay, overlay_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_press_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_press_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_press_cb, LV_EVENT_PRESS_LOST, NULL);

    s_msg = lv_label_create(s_scr);
    lv_obj_set_width(s_msg, 520);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_msg, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_msg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_msg, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(s_msg, 12, 0);
    lv_obj_set_style_radius(s_msg, 12, 0);
    lv_obj_align(s_msg, LV_ALIGN_CENTER, 0, 150);
    lv_obj_set_clickable(s_msg, false);

    // Pills around the edge. Angles: 0 = 3 o'clock, increasing clockwise.
    pill_create(&s_pills[PILL_TIME], &lv_font_montserrat_28, 270, 0, LV_ARCLABEL_DIR_CLOCKWISE, ui_weather_open);
    pill_create(&s_pills[PILL_TIDE], &lv_font_montserrat_24, 90, 0, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE, ui_tide_open);
    s_pills[PILL_TIDE].lbl2 = pill_label(&s_pills[PILL_TIDE], &lv_font_montserrat_24, 90, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE);
    pill_create(&s_pills[PILL_STATUS], &lv_font_montserrat_24, 180, 0, LV_ARCLABEL_DIR_CLOCKWISE, ui_vessels_open);
    // Zoom on the right edge: + above the current radius, - below.
    pill_create(&s_pills[PILL_ZOOM_IN], &lv_font_montserrat_28, -21, 9, LV_ARCLABEL_DIR_CLOCKWISE, zoom_in);
    pill_create(&s_pills[PILL_ZOOM], &lv_font_montserrat_24, 0, 0, LV_ARCLABEL_DIR_CLOCKWISE, NULL);
    pill_create(&s_pills[PILL_ZOOM_OUT], &lv_font_montserrat_28, 21, 9, LV_ARCLABEL_DIR_CLOCKWISE, zoom_out);
    pill_set_text(&s_pills[PILL_ZOOM_IN], LV_SYMBOL_PLUS);
    pill_set_text(&s_pills[PILL_ZOOM_OUT], LV_SYMBOL_MINUS);

    ui_vessels_create(s_scr);
    ui_tide_create(s_scr);
    ui_weather_create(s_scr);

    view_go_home();
    lv_timer_create(refresh_timer_cb, 1000, NULL);
    lv_timer_create(tile_poll_cb, TILE_POLL_MS, NULL);
    return s_scr;
}

void ui_map_set_filter(const vessel_filter_t *f)
{
    s_filter = *f;
    lv_obj_invalidate(s_overlay);
}

void ui_map_on_show(void)
{
    s_nvessels = vessel_db_snapshot(s_vessels, VESSEL_MAX);
    lv_obj_invalidate(s_overlay);
}
