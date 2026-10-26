// Map screen: chart tiles (draggable), AIS targets, and pills around the
// round display's border for the time, tide, vessel count, zoom and recentre.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "adsb.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "geo.h"
#include "map_tiles.h"
#include "met_db.h"
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
#define WIND_TAIL_R     356     // px from the centre: the wind and wave arrows, just inside the bubbles
#define WIND_HEAD_R     302
#define WIND_TIP_R      264
#define WIND_SHAFT_W    48
#define WIND_HEAD_W     78
#define WX_OVERLAP_DEG  16      // wind and wave arrows this close in direction share one label
#define ARROW_TEXT_COLOR lv_color_hex(0x111111) // figures on the wind and wave arrows: dark, to read on their fills
#define WAVE_ICON_PX    26      // the wave icon in the tide bubble (buoy without wave direction)

// Wi-Fi icon before the time: signal bars online, struck through offline,
// "?" in place of the inner bars at a captive portal.
#define NET_ICON_PX     36
#define NET_ICON_GAP_PX 6
#define WAVE_ICON_GAP   10
#define WIND_COLOR      lv_color_hex(0xF5D33F)
#define WAVE_COLOR      lv_color_hex(0x3FA7F5)

// Zoom levels when there is no chart.
static const float k_default_radii[] = {0.5f, 1.0f, 2.0f, 4.0f, 10.0f};

typedef struct {
    uint32_t mmsi;          // for an ADS-B aircraft, its adsb id
    int16_t x, y, r;
    int16_t met;            // a weather station: its index in s_met; -1 for a target
    bool aircraft;          // an ADS-B aircraft
} hit_t;

// Where a vessel ended up on screen, for placing its label afterwards.
typedef struct {
    const vessel_t *v;
    const adsb_target_t *ac;    // instead of v: an ADS-B aircraft
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
    float lead_deg;         // room before the text for an icon (Wi-Fi in the time bubble)
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
static met_station_t s_met[MET_MAX];   // weather stations heard over AIS
static int s_nmet;
static adsb_target_t *s_ac;             // aircraft from dump1090, within range
static int s_nac;
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

typedef enum { NET_ICON_NONE, NET_ICON_BARS, NET_ICON_OFFLINE, NET_ICON_PORTAL } net_icon_t;
static struct {
    net_icon_t kind;
    int bars;               // 0-3, for NET_ICON_BARS
} s_net;

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
        // Any lead icon, text, icon, second text, in reading order (which runs
        // backwards in screen angles along the bottom of the screen).
        float t1 = text_deg(p->lbl, p->text, p), t2 = text_deg(p->lbl2, p->text2, p);
        p->span = fmaxf(p->min_span, p->lead_deg + t1 + p->mid_deg + t2 + 2 * PILL_PAD_DEG);
        float start = p->center - p->dir * (p->span / 2 - PILL_PAD_DEG - p->lead_deg);
        lv_arclabel_set_angle_start(p->lbl, start + p->dir * t1 / 2 - 60);
        p->mid_angle = start + p->dir * (t1 + p->mid_deg / 2);
        lv_arclabel_set_angle_start(p->lbl2, start + p->dir * (t1 + p->mid_deg + t2 / 2) - 60);
    } else {
        float t = p->icon_deg || p->lead_deg ? text_deg(p->lbl, p->text, p) : (float)lv_arclabel_get_text_angle(p->lbl);
        p->span = fmaxf(p->min_span, t + 2 * PILL_PAD_DEG + p->lead_deg + p->icon_deg);
        lv_arclabel_set_angle_start(p->lbl, p->center + (p->lead_deg - p->icon_deg) / 2 - 60);
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

static void pill_set_lead_room(pill_t *p, float px)
{
    float deg = px ? px_to_deg(px, pill_outer_r() - p->width / 2.0f) : 0;
    if (fabsf(deg - p->lead_deg) < 0.01f) return;
    p->lead_deg = deg;
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

// A dark rim, then the coloured edge on it: clear on light water and land alike.
static void outline(lv_layer_t *layer, const lv_point_precise_t *pts, int n, lv_color_t color)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.round_start = d.round_end = 1;
    for (int pass = 0; pass < 2; pass++) {
        d.color = pass ? color : lv_color_black();
        d.width = pass ? 2 : 6;
        d.opa = pass ? LV_OPA_90 : LV_OPA_60;
        for (int i = 0; i < n; i++) {
            d.p1 = pts[i];
            d.p2 = pts[(i + 1) % n];
            lv_draw_line(layer, &d);
        }
    }
}

// Text centred on a point, upright, with a dark shadow so it reads on the colour.
static void text_at_color(lv_layer_t *layer, const char *text, const lv_font_t *font, lv_point_precise_t c,
                          lv_color_t color)
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
        d.color = i ? color : lv_color_white(); // a light shadow: the text is dark
        d.opa = i ? LV_OPA_COVER : LV_OPA_50;
        lv_draw_label(layer, &d, &a);
    }
}

// An arrow pointing in from the rim along bearing b, translucent, outlined.
static void draw_arrow(lv_layer_t *layer, float b, lv_color_t color, lv_opa_t opa)
{
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
    shaft.p2 = head;
    shaft.width = WIND_SHAFT_W;
    shaft.color = color;
    shaft.opa = opa;
    lv_draw_line(layer, &shaft);
    fill_tri(layer, pts[2], pts[3], pts[4], color, opa); // the head
    outline(layer, pts, 7, color);
}

// Where an arrow's label goes: on its shaft.
static lv_point_precise_t arrow_label_at(float b)
{
    return at_bearing(b, (WIND_TAIL_R + WIND_HEAD_R) / 2.0f + 2);
}

typedef struct {
    char text[12];
    const lv_font_t *font;
    lv_color_t color;
} label_line_t;

// Lines centred, one under the other, on c.
static void draw_label_lines(lv_layer_t *layer, const label_line_t *l, int n, lv_point_precise_t c)
{
    float total = 0;
    for (int i = 0; i < n; i++) total += lv_font_get_line_height(l[i].font) - 2;
    float y = c.y - total / 2;
    for (int i = 0; i < n; i++) {
        float h = lv_font_get_line_height(l[i].font) - 2;
        text_at_color(layer, l[i].text, l[i].font, (lv_point_precise_t){c.x, y + h / 2}, l[i].color);
        y += h;
    }
}

// The wind's figures: the speed (bigger on its own), and any gust.
static int wind_lines(label_line_t *l)
{
    int n = 0;
    bool gust = s_wx.wind_mph >= 1 && s_wx.gust_mph - s_wx.wind_mph >= GUST_SHOW_MPH;
    l[n] = (label_line_t){.font = gust ? &lv_font_montserrat_20 : &lv_font_montserrat_24, .color = ARROW_TEXT_COLOR};
    snprintf(l[n++].text, sizeof(l->text), "%.0f", s_wx.wind_mph < 1 ? 0 : s_wx.wind_mph);
    if (gust) {
        l[n] = (label_line_t){.font = &lv_font_montserrat_14, .color = ARROW_TEXT_COLOR};
        snprintf(l[n++].text, sizeof(l->text), "G%.0f", s_wx.gust_mph);
    }
    return n;
}

// The waves' figures: height, and period if known.
static int wave_lines(label_line_t *l)
{
    int n = 0;
    l[n] = (label_line_t){.font = &lv_font_montserrat_16, .color = ARROW_TEXT_COLOR};
    snprintf(l[n++].text, sizeof(l->text), "%.1f ft", s_wx.wave_ft);
    if (s_wx.wave_s > 0) {
        l[n] = (label_line_t){.font = &lv_font_montserrat_14, .color = ARROW_TEXT_COLOR};
        snprintf(l[n++].text, sizeof(l->text), "%.0f s", s_wx.wave_s);
    }
    return n;
}

// Wind and wave arrows, the same shape and at the same distance from the
// edge. Translucent, so they can overlap; their labels go on top of both, and
// when the arrows overlap the two labels become one: wind, then waves.
static void draw_wind_and_waves(lv_layer_t *layer)
{
    bool wind = s_wx.wind_on, wave = s_wx.wave_on;
    if (!wind && !wave) return;
    bool calm = wind && s_wx.wind_mph < 1;
    if (wave) draw_arrow(layer, s_wx.wave_dir, WAVE_COLOR, LV_OPA_40);
    if (calm) { // a small circle where the arrow would be
        lv_point_precise_t c = arrow_label_at(s_wx.wind_dir);
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.center = (lv_point_t){(int32_t)c.x, (int32_t)c.y};
        d.start_angle = 0;
        d.end_angle = 360;
        d.radius = 23; // the dark rim
        d.width = 26;
        d.color = lv_color_black();
        d.opa = LV_OPA_40;
        lv_draw_arc(layer, &d);
        d.radius = 20;
        d.width = 20;
        d.color = WIND_COLOR;
        d.opa = LV_OPA_50;
        lv_draw_arc(layer, &d);
    } else if (wind) {
        draw_arrow(layer, s_wx.wind_dir, WIND_COLOR, LV_OPA_50);
    }

    label_line_t l[4];
    int n = 0;
    bool shared = wind && wave && fabsf(fmodf(s_wx.wind_dir - s_wx.wave_dir + 540, 360) - 180) < WX_OVERLAP_DEG;
    if (shared) {
        n = wind_lines(l);
        n += wave_lines(l + n);
        // Between the two directions, so it sits on both.
        float mid = s_wx.wave_dir + (fmodf(s_wx.wind_dir - s_wx.wave_dir + 540, 360) - 180) / 2;
        draw_label_lines(layer, l, n, arrow_label_at(mid));
        return;
    }
    if (wave) {
        n = wave_lines(l);
        draw_label_lines(layer, l, n, arrow_label_at(s_wx.wave_dir));
    }
    if (wind) {
        n = wind_lines(l);
        draw_label_lines(layer, l, n, arrow_label_at(s_wx.wind_dir));
    }
}

// The icon (and "!") in the room pill_set_icon_room() left after the time:
// midway between the time and the end of the bubble.
static void draw_time_icon(lv_layer_t *layer)
{
    const pill_t *p = &s_pills[PILL_TIME];
    if (!s_wx.icon_on || p->hidden) return;
    float rm = pill_outer_r() - p->width / 2.0f;
    float edge = p->center + p->span / 2 + px_to_deg(p->width / 2.0f, rm); // the bubble's end (its round cap)
    float text_end = p->center + p->span / 2 - PILL_PAD_DEG - p->icon_deg;
    float mid = (text_end + edge) / 2;
    float group = px_to_deg(WX_ICON_PX + (s_wx.alert ? WX_ALERT_PX : 0), rm);
    float icon_mid = mid - group / 2 + px_to_deg(WX_ICON_PX / 2.0f, rm);
    float end = mid + group / 2; // where the "!" ends
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

// Where the Wi-Fi icon's room starts and ends, in screen degrees.
static void net_icon_room(float *start, float *end)
{
    const pill_t *p = &s_pills[PILL_TIME];
    *start = p->center - p->span / 2 + PILL_PAD_DEG;
    *end = *start + p->lead_deg;
}

static void draw_net_icon(lv_layer_t *layer)
{
    const pill_t *p = &s_pills[PILL_TIME];
    if (s_net.kind == NET_ICON_NONE || p->hidden || !p->lead_deg) return;
    float rm = pill_outer_r() - p->width / 2.0f, start, end;
    net_icon_room(&start, &end);
    // Midway between the bubble's end (its round cap) and the start of the time.
    float edge = start - PILL_PAD_DEG - px_to_deg(p->width / 2.0f, rm);
    float a = ((edge + end) / 2) * (float)M_PI / 180;
    int32_t cx = (int32_t)lroundf(UI_CENTER + rm * cosf(a)), cy = (int32_t)lroundf(UI_CENTER + rm * sinf(a));

    // A quarter-circle fan opening upwards from a point near the bottom.
    lv_point_t o = {cx, cy + 12};
    static const int16_t radii[] = {4, 11, 18, 25}; // the dot, then three bars
    bool portal = s_net.kind == NET_ICON_PORTAL;
    lv_color_t on = portal ? UI_COLOR_WARN : s_net.kind == NET_ICON_OFFLINE ? UI_COLOR_MUTED : lv_color_white();
    for (int i = portal ? 2 : 0; i < 4; i++) {
        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.center = o;
        d.radius = radii[i];
        d.width = 4;
        d.start_angle = i == 0 ? 0 : 225;
        d.end_angle = i == 0 ? 360 : 315;
        d.rounded = 1;
        d.color = on;
        d.opa = s_net.kind == NET_ICON_BARS && i > s_net.bars ? LV_OPA_30 : LV_OPA_COVER;
        lv_draw_arc(layer, &d);
    }
    if (portal) {
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.text = "?";
        ld.font = &lv_font_montserrat_16;
        ld.color = UI_COLOR_WARN;
        ld.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t la = {o.x - 8, o.y - 15, o.x + 8, o.y + 3};
        lv_draw_label(layer, &ld, &la);
    } else if (s_net.kind == NET_ICON_OFFLINE) {
        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.p1 = (lv_point_precise_t){o.x - 15, o.y + 2};
        ld.p2 = (lv_point_precise_t){o.x + 15, o.y - 26};
        ld.width = 3;
        ld.round_start = ld.round_end = 1;
        ld.color = UI_COLOR_BAD;
        lv_draw_line(layer, &ld);
    }
}

// The Wi-Fi icon from the manager's status.
static void update_net_icon(void)
{
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    net_icon_t kind;
    int bars = 0;
    if (ws.state == WIFI_MGR_STA_CONNECTED && ws.net == WIFI_NET_PORTAL) {
        kind = NET_ICON_PORTAL;
    } else if (ws.state != WIFI_MGR_STA_CONNECTED || ws.net == WIFI_NET_OFFLINE) {
        kind = NET_ICON_OFFLINE;
    } else {
        kind = NET_ICON_BARS;
        bars = ws.rssi >= -60 ? 3 : ws.rssi >= -70 ? 2 : ws.rssi >= -80 ? 1 : 0;
    }
    if (kind != s_net.kind || bars != s_net.bars) {
        s_net.kind = kind;
        s_net.bars = bars;
        if (s_overlay) lv_obj_invalidate(s_overlay);
    }
    pill_set_lead_room(&s_pills[PILL_TIME], kind == NET_ICON_NONE ? 0 : NET_ICON_PX + NET_ICON_GAP_PX);
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
static void draw_dashes(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_color_t color, int w,
                        lv_opa_t opa, float dash, float gap)
{
    float dx = b.x - a.x, dy = b.y - a.y, len = hypotf(dx, dy);
    if (len < 1) return;
    dx /= len;
    dy /= len;
    for (float t = 0; t < len; t += dash + gap) {
        float e = fminf(t + dash, len);
        draw_line(layer, (lv_point_precise_t){a.x + dx * t, a.y + dy * t}, (lv_point_precise_t){a.x + dx * e, a.y + dy * e},
                  color, w, opa);
    }
}

static void draw_dashed_line(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_color_t color, int w,
                             lv_opa_t opa)
{
    draw_dashes(layer, a, b, color, w, opa, DASH_PX, DASH_GAP_PX);
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

static bool is_beacon(uint32_t mmsi);
static bool beacon_active(const vessel_t *v);

static bool vessel_passes_filter(const vessel_t *v)
{
    const vessel_filter_t *f = &s_filter;
    if (v->cls == AIS_CLASS_ATON || vessel_is_aircraft(v) || is_beacon(v->mmsi)) return true; // the filters are for vessels
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
    if (v->cls == AIS_CLASS_ATON) return -200000; // marks never take label space from vessels
    if (is_beacon(v->mmsi)) return 1000000;       // beacons always get theirs
    if (s_filter.order == ORDER_SPEED) p = (v->fields & AIS_F_SOG) ? v->sog_kn : -1;
    else p = ((v->fields & AIS_F_DIM) && v->dim_a + v->dim_b) ? v->dim_a + v->dim_b : -1;
    if (vessel_stale(v, now_us)) p -= 100000;
    return p;
}

// Drawing order: stale vessels at the bottom, then those at rest, then vessels
// under way on top, so a moving vessel is never hidden under a stationary one.
static float vessel_layer(const vessel_t *v, float prio, int64_t now_us)
{
    // Marks at the very bottom; aircraft, then beacons, on top of everything.
    if (is_beacon(v->mmsi)) return 4000000.0f + prio;
    if (v->cls == AIS_CLASS_ATON) return -1000000.0f + prio;
    if (vessel_is_aircraft(v)) return 3000000.0f + prio;
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

// Label text for a vessel per the label options; empty if none. Marks only
// ever show their name; aircraft always show their altitude.
static void label_text(const vessel_t *v, char *text, size_t cap)
{
    if (is_beacon(v->mmsi)) { // always labelled, whatever the options
        uint32_t p = v->mmsi / 1000000;
        snprintf(text, cap, "%s%s", p == 970 ? "SART" : p == 972 ? "MOB" : "EPIRB", beacon_active(v) ? "" : " (test)");
        return;
    }
    if (v->cls == AIS_CLASS_ATON) {
        strlcpy(text, s_filter.label_name ? v->name : "", cap);
        return;
    }
    if (vessel_is_aircraft(v)) {
        char name[24] = "", line2[32] = "";
        if (s_filter.label_name) {
            if (v->name[0]) strlcpy(name, v->name, sizeof(name));
            else snprintf(name, sizeof(name), "%s %lu", v->cls == AIS_CLASS_SAR ? "SAR" : "Aircraft", (unsigned long)v->mmsi);
        }
        int n = 0;
        if (v->fields & AIS_F_ALT) n = snprintf(line2, sizeof(line2), "%.0f ft", v->alt_m * 3.28084f);
        if (s_filter.label_speed && (v->fields & AIS_F_SOG))
            snprintf(line2 + n, sizeof(line2) - n, "%s%.0f kn", n ? ", " : "", v->sog_kn);
        snprintf(text, cap, "%s%s%s", name, name[0] && line2[0] ? "\n" : "", line2);
        return;
    }
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

// Where a target was drawn: for its label, and to find it by touch.
static void place(placed_t *pl, float x, float y, float extent, lv_opa_t opa)
{
    pl->shown = true;
    pl->x = x;
    pl->y = y;
    pl->extent = extent;
    pl->opa = opa;

    hit_t *h = &s_hits[s_nhits++];
    h->met = -1;
    h->aircraft = pl->ac != NULL;
    h->mmsi = pl->ac ? pl->ac->id : pl->v->mmsi;
    h->x = (int16_t)x;
    h->y = (int16_t)y;
    h->r = (int16_t)fmaxf(extent, HIT_RADIUS_MIN);
}

// ---------------------------------------------------------------------------
// Aids to navigation and SAR aircraft
// ---------------------------------------------------------------------------

// A small icon made of convex shapes. Each is drawn in three passes over the
// whole icon (white halo, dark outline, fill), so the fills cover the outlines
// where shapes meet and the icon reads as one piece with one rim. An overlay
// (a band of colour) is only filled, over what's under it.
typedef enum { PART_POLY, PART_RECT, PART_CIRCLE } part_kind_t;

typedef struct {
    part_kind_t kind;       // rectangles and circles are drawn as such (never rotated); p[] outlines them too
    lv_point_precise_t p[8];
    int n;
    float x1, y1, x2, y2;   // PART_RECT / PART_CIRCLE bounds
    lv_color_t color;
    bool overlay;
} part_t;

#define MAX_PARTS 6

static void part_rect(part_t *o, float x1, float y1, float x2, float y2, lv_color_t c, bool overlay)
{
    *o = (part_t){.kind = PART_RECT, .p = {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}}, .n = 4,
                  .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2, .color = c, .overlay = overlay};
}

// Apex up (or down) at y_apex, base at y_base, w wide there.
static void part_cone(part_t *o, float y_apex, float y_base, float w, lv_color_t c)
{
    *o = (part_t){.p = {{0, y_apex}, {w / 2, y_base}, {-w / 2, y_base}}, .n = 3, .color = c};
}

static void part_circle(part_t *o, float cx, float cy, float r, lv_color_t c, bool overlay)
{
    *o = (part_t){.kind = PART_CIRCLE, .n = 8, .x1 = cx - r, .y1 = cy - r, .x2 = cx + r, .y2 = cy + r, .color = c,
                  .overlay = overlay};
    for (int i = 0; i < 8; i++) {
        float a = i * (float)M_PI / 4 + (float)M_PI / 8;
        o->p[i] = (lv_point_precise_t){cx + r * cosf(a), cy + r * sinf(a)};
    }
}

// A bar from (x1,y1) to (x2,y2), w wide (the arms of the special mark's X).
static void part_bar(part_t *o, float x1, float y1, float x2, float y2, float w, lv_color_t c)
{
    float dx = x2 - x1, dy = y2 - y1, l = hypotf(dx, dy), nx = -dy / l * w / 2, ny = dx / l * w / 2;
    *o = (part_t){.p = {{x1 + nx, y1 + ny}, {x2 + nx, y2 + ny}, {x2 - nx, y2 - ny}, {x1 - nx, y1 - ny}}, .n = 4, .color = c};
}

// Move every part, and turn the polygons: (s, c) is the sine and cosine of the
// direction (rectangles and circles only ever move).
static void parts_place(part_t *parts, int n, float s, float c, float x, float y)
{
    for (int i = 0; i < n; i++) {
        part_t *q = &parts[i];
        for (int k = 0; k < q->n; k++) q->p[k] = rot(q->p[k].x, q->p[k].y, s, c, x, y);
        q->x1 += x;
        q->x2 += x;
        q->y1 += y;
        q->y2 += y;
    }
}

// A rectangle or circle part, grown by `grow` px all round.
static void draw_box(lv_layer_t *layer, const part_t *q, float grow, lv_color_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = color;
    r.bg_opa = opa;
    r.radius = q->kind == PART_CIRCLE ? LV_RADIUS_CIRCLE : (int32_t)grow;
    lv_area_t a = {(int32_t)lroundf(q->x1 - grow), (int32_t)lroundf(q->y1 - grow), (int32_t)lroundf(q->x2 + grow) - 1,
                   (int32_t)lroundf(q->y2 + grow) - 1};
    lv_draw_rect(layer, &r, &a);
}

#define PART_OUTLINE_PX 1.0f    // the dark rim outside each part...
#define PART_HALO_PX    3.0f    // ...and the white one outside that

// Virtual marks (no physical mark there) are only a dashed outline in their colour.
static void draw_parts(lv_layer_t *layer, const part_t *parts, int n, bool outline_only, lv_opa_t opa)
{
    lv_color_t halo = lv_color_white(), dark = lv_color_hex(0x101010);
    lv_opa_t halo_opa = (lv_opa_t)(opa * 85 / 100);
    if (outline_only) {
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < n; i++) {
                const part_t *q = &parts[i];
                if (q->overlay) continue;
                for (int k = 0; k < q->n; k++) {
                    lv_point_precise_t a = q->p[k], b = q->p[(k + 1) % q->n];
                    if (pass == 0) draw_dashes(layer, a, b, halo, 2 + 2 * HALO_PX, halo_opa, 3, 3);
                    else draw_dashes(layer, a, b, q->color, 2, opa, 3, 3);
                }
            }
        }
        return;
    }
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < n; i++) {
            const part_t *q = &parts[i];
            if (q->overlay && pass < 2) continue;
            lv_color_t c = pass == 0 ? halo : pass == 1 ? dark : q->color;
            lv_opa_t o = pass == 0 ? halo_opa : opa;
            float grow = pass == 0 ? PART_OUTLINE_PX + PART_HALO_PX : pass == 1 ? PART_OUTLINE_PX : 0;
            if (q->kind != PART_POLY) {
                draw_box(layer, q, grow, c, o);
                continue;
            }
            if (pass < 2) {
                for (int k = 0; k < q->n; k++) draw_line(layer, q->p[k], q->p[(k + 1) % q->n], c, (int)(2 * grow), o);
            }
            for (int k = 1; k + 1 < q->n; k++) draw_triangle(layer, q->p[0], q->p[k], q->p[k + 1], c, o);
            // Paint over the seams where the fan's triangles meet.
            for (int k = 2; k + 1 < q->n; k++) draw_line(layer, q->p[0], q->p[k], c, 1, o);
        }
    }
}

// Mark colours, IALA region B (the Americas): green to port, red to starboard
// coming in from sea.
#define MARK_GREEN      lv_color_hex(0x2E9E4F)
#define MARK_RED        lv_color_hex(0xD8343A)
#define MARK_YELLOW     lv_color_hex(0xF2C230)
#define MARK_BLACK      lv_color_hex(0x1A1A1A)
#define MARK_MAGENTA    lv_color_hex(0xC0289A)
#define SAR_COLOR       lv_color_hex(0xFF8A1F)

// The shapes of an aid to navigation of AIS type t, around its position (0, 0),
// screen up. Buoys (20-30) look like the beacons of the same kind (9-19).
// Returns how many; 0 for a light (drawn as a star instead).
static int mark_parts(uint8_t t, part_t *o)
{
    const lv_color_t g = MARK_GREEN, r = MARK_RED, y = MARK_YELLOW, k = MARK_BLACK, w = lv_color_white();
    uint8_t kind = t >= AIS_ATON_BUOY_N && t <= AIS_ATON_BUOY_N + 10 ? t - AIS_ATON_BUOY_N + AIS_ATON_BEACON_N : t;
    switch (kind) {
    case 9: // north cardinal: black over yellow, cones both up
        part_rect(&o[0], -5, -6, 5, 6, k, false);
        part_rect(&o[1], -5, 0, 5, 6, y, true);
        part_cone(&o[2], -14, -9, 9, k);
        part_cone(&o[3], -21, -16, 9, k);
        return 4;
    case 10: // east: black-yellow-black, cones base to base
        part_rect(&o[0], -5, -6, 5, 6, k, false);
        part_rect(&o[1], -5, -2, 5, 2, y, true);
        part_cone(&o[2], -9, -14, 9, k);
        part_cone(&o[3], -21, -16, 9, k);
        return 4;
    case 11: // south: yellow over black, cones both down
        part_rect(&o[0], -5, -6, 5, 6, y, false);
        part_rect(&o[1], -5, 0, 5, 6, k, true);
        part_cone(&o[2], -9, -14, 9, k);
        part_cone(&o[3], -16, -21, 9, k);
        return 4;
    case 12: // west: yellow-black-yellow, cones point to point
        part_rect(&o[0], -5, -6, 5, 6, y, false);
        part_rect(&o[1], -5, -2, 5, 2, k, true);
        part_cone(&o[2], -14, -9, 9, k);
        part_cone(&o[3], -16, -21, 9, k);
        return 4;
    case 13: // port hand: green can
        part_rect(&o[0], -6, -6, 6, 6, g, false);
        return 1;
    case 14: // starboard hand: red nun
        part_cone(&o[0], -8, 6, 14, r);
        return 1;
    case 15: // preferred channel, port hand shape: green can, red band
        part_rect(&o[0], -6, -6, 6, 6, g, false);
        part_rect(&o[1], -6, -2, 6, 2, r, true);
        return 2;
    case 16: { // preferred channel, starboard hand shape: red nun, green band
        part_cone(&o[0], -8, 6, 14, r);
        float w1 = 7 * (-1 + 8) / 14.0f, w2 = 7 * (3 + 8) / 14.0f; // the cone's half-width at y = -1 and 3
        o[1] = (part_t){.p = {{-w1, -1}, {w1, -1}, {w2, 3}, {-w2, 3}}, .n = 4, .color = g, .overlay = true};
        return 2;
    }
    case 17: // isolated danger: black, red band, two black balls
        part_rect(&o[0], -5, -6, 5, 6, k, false);
        part_rect(&o[1], -5, -2, 5, 2, r, true);
        part_circle(&o[2], 0, -11, 3, k, false);
        part_circle(&o[3], 0, -18.5f, 3, k, false);
        return 4;
    case 18: // safe water: red and white, a red ball
        part_circle(&o[0], 0, 0, 7, r, false);
        o[1] = (part_t){.n = 7, .color = w, .overlay = true};
        for (int i = 0; i < 7; i++) {
            float a = -(float)M_PI / 2 + i * (float)M_PI / 6;
            o[1].p[i] = (lv_point_precise_t){7 * cosf(a), 7 * sinf(a)};
        }
        part_circle(&o[2], 0, -12, 3, r, false);
        return 3;
    case 19: // special: yellow, an X
        part_rect(&o[0], -6, -6, 6, 6, y, false);
        part_bar(&o[1], -4.5f, -18, 4.5f, -10, 2.5f, y);
        part_bar(&o[2], 4.5f, -18, -4.5f, -10, 2.5f, y);
        return 3;
    case 2: // RACON: a magenta ring
        part_circle(&o[0], 0, 0, 8, MARK_MAGENTA, false);
        part_circle(&o[1], 0, 0, 4.5f, w, true);
        return 2;
    case 3: // fixed structure
        part_rect(&o[0], -6, -6, 6, 6, k, false);
        part_rect(&o[1], -2, -2, 2, 2, w, true);
        return 2;
    case 5: case 6: case 7: case 8: // lights
        return 0;
    default: // anything else, or unknown: the chart symbol for an AIS aid, a diamond with a dot
        o[0] = (part_t){.p = {{0, -8}, {8, 0}, {0, 8}, {-8, 0}}, .n = 4, .color = MARK_MAGENTA};
        part_circle(&o[1], 0, 0, 2.2f, w, true);
        return 2;
    }
}

// A light: a magenta star of eight rays.
static void draw_light(lv_layer_t *layer, float x, float y, bool virt, lv_opa_t opa)
{
    lv_opa_t halo_opa = (lv_opa_t)(opa * 85 / 100);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 8; i++) {
            float a = i * (float)M_PI / 4, l = i & 1 ? 6.5f : 9.5f;
            lv_point_precise_t c = {x, y}, e = {x + l * cosf(a), y + l * sinf(a)};
            if (pass == 0) draw_line(layer, c, e, lv_color_white(), 2 + 2 * HALO_PX, halo_opa);
            else if (virt) draw_dashes(layer, c, e, MARK_MAGENTA, 2, opa, 3, 2);
            else draw_line(layer, c, e, MARK_MAGENTA, 2, opa);
        }
    }
}

static void draw_aton(lv_layer_t *layer, placed_t *pl, float x, float y, lv_opa_t opa)
{
    const vessel_t *v = pl->v;
    bool virt = v->aton_flags & AIS_ATON_VIRTUAL;
    part_t parts[MAX_PARTS];
    int n = mark_parts(v->aton_type, parts);
    if (n) {
        parts_place(parts, n, 0, 1, x, y);
        draw_parts(layer, parts, n, virt, opa);
    } else {
        draw_light(layer, x, y, virt, opa);
    }
    float extent = 10;
    if (v->aton_flags & AIS_ATON_OFF_POS) { // drifted off its station: a red ring
        lv_draw_arc_dsc_t a;
        lv_draw_arc_dsc_init(&a);
        a.center = (lv_point_t){(int32_t)x, (int32_t)(y - 4)};
        a.radius = 16;
        a.width = 3;
        a.start_angle = 0;
        a.end_angle = 360;
        a.color = MARK_RED;
        a.opa = opa;
        lv_draw_arc(layer, &a);
        extent = 16;
    }
    place(pl, x, y, extent, opa);
}

// An aeroplane pointing along its course, with a course vector like a vessel's.
static void draw_aircraft(lv_layer_t *layer, placed_t *pl, float x, float y, lv_opa_t opa)
{
    const vessel_t *v = pl->v;
    lv_color_t color = SAR_COLOR;
    bool moving = (v->fields & AIS_F_SOG) && v->sog_kn >= HEADING_MIN_KN;
    if ((v->fields & AIS_F_COG) && moving) {
        float vlen = fminf(v->sog_kn * 1852.0f * VECTOR_MINUTES / 60.0f / s_view.m_per_px, 160);
        float s = sinf(v->cog_deg * (float)M_PI / 180), c = cosf(v->cog_deg * (float)M_PI / 180);
        lv_point_precise_t a = {x, y}, b = {x + s * vlen, y - c * vlen};
        draw_dashed_line(layer, a, b, lv_color_white(), 2 + 2 * HALO_PX, (lv_opa_t)(opa * 85 / 100));
        draw_dashed_line(layer, a, b, color, 2, opa);
    }
    part_t parts[3] = {
        {.p = {{0, -14}, {2.5f, -10}, {2.5f, 10}, {-2.5f, 10}, {-2.5f, -10}}, .n = 5, .color = color}, // fuselage
        {.p = {{-2, -4}, {2, -4}, {13, 1}, {13, 3.5f}, {-13, 3.5f}, {-13, 1}}, .n = 6, .color = color}, // wings
        {.p = {{-1.5f, 6}, {1.5f, 6}, {6, 10}, {6, 12}, {-6, 12}, {-6, 10}}, .n = 6, .color = color},  // tailplane
    };
    float dir = (v->fields & AIS_F_COG) ? v->cog_deg : 0;
    parts_place(parts, 3, sinf(dir * (float)M_PI / 180), cosf(dir * (float)M_PI / 180), x, y);
    draw_parts(layer, parts, 3, false, opa);
    place(pl, x, y, 14, opa);
}

// ---------------------------------------------------------------------------
// ADS-B aircraft (from dump1090): an icon for each kind, from its emitter category
// ---------------------------------------------------------------------------

#define ADSB_COLOR          lv_color_hex(0x6D8BB0)  // slate blue (orange stays for AIS SAR aircraft)
#define ADSB_EMERG_COLOR    lv_color_hex(0xE0242B)  // squawking 7500 / 7600 / 7700
#define ADSB_VECTOR_MINUTES 1       // aircraft are fast: a short track vector, so a busy sky doesn't bury the vessels
#define ADSB_VECTOR_MAX_PX  60

static void part_poly(part_t *o, const lv_point_precise_t *p, int n, lv_color_t c)
{
    *o = (part_t){.n = n, .color = c};
    memcpy(o->p, p, n * sizeof(p[0]));
}

#define POLY(o, c, ...)                                                                 \
    do {                                                                                \
        const lv_point_precise_t pts_[] = {__VA_ARGS__};                                \
        part_poly(o, pts_, sizeof(pts_) / sizeof(pts_[0]), c);                          \
    } while (0)

// A fuselage from the nose (y0) to the tail (y1), w wide, with a pointed nose.
static void part_fuselage(part_t *o, float y0, float y1, float w, lv_color_t c)
{
    POLY(o, c, {0, y0}, {w / 2, y0 + w}, {w / 2, y1}, {-w / 2, y1}, {-w / 2, y0 + w});
}

// Both wings (or tailplanes) as one shape: the leading edge from the root at
// (±root, y0) back to the tips at (±span, y0 + sweep), chord deep at the tips.
static void part_wings(part_t *o, float root, float y0, float span, float sweep, float chord, lv_color_t c)
{
    POLY(o, c, {-root, y0}, {root, y0}, {span, y0 + sweep}, {span, y0 + sweep + chord}, {-span, y0 + sweep + chord},
         {-span, y0 + sweep});
}

// A pair of swept wings (or tailplanes), each its own shape so the trailing
// edge sweeps back too: at the root (±root) from y0 to y1, at the tips (±span)
// from y0 + sweep, chord deep.
static void part_wing_pair(part_t *o, float root, float y0, float y1, float span, float sweep, float chord, lv_color_t c)
{
    POLY(&o[0], c, {root, y0}, {span, y0 + sweep}, {span, y0 + sweep + chord}, {root, y1});
    POLY(&o[1], c, {-root, y0}, {-root, y1}, {-span, y0 + sweep + chord}, {-span, y0 + sweep});
}

// An engine pod centred on x, from y0 to y1, and its twin on the other side.
static void part_pods(part_t *o, float x, float y0, float y1, float w, lv_color_t c)
{
    POLY(&o[0], c, {x - w / 2, y0}, {x + w / 2, y0}, {x + w / 2, y1}, {x - w / 2, y1});
    POLY(&o[1], c, {-x - w / 2, y0}, {-x + w / 2, y0}, {-x + w / 2, y1}, {-x - w / 2, y1});
}

// The shapes of an aircraft of this kind around its position, nose up (at
// most ADSB_MAX_PARTS). *turns: false for those drawn upright whatever their
// track (round ones, whose circles can't be turned). *extent: how far it
// reaches. Returns how many.
#define ADSB_MAX_PARTS 9

static int adsb_parts(adsb_kind_t kind, lv_color_t c, part_t *o, bool *turns, float *extent)
{
    *turns = true;
    switch (kind) {
    case ADSB_KIND_LIGHT: // high straight wing, propeller
        part_fuselage(&o[0], -11, 9, 4, c);
        POLY(&o[1], c, {-13, -5}, {13, -5}, {13, -1}, {-13, -1});
        POLY(&o[2], c, {-5, 6}, {5, 6}, {5, 9}, {-5, 9});
        part_bar(&o[3], -5, -11, 5, -11, 2, c);
        *extent = 14;
        return 4;
    case ADSB_KIND_SMALL: // business jet: swept wings, engines by the tail, T-tail
        part_fuselage(&o[0], -14, 11, 4.5f, c);
        part_wing_pair(&o[1], 2, -3, 2, 12, 6, 2, c);
        part_pods(&o[3], 3.8f, 3, 8, 2.6f, c);
        part_wing_pair(&o[5], 1, 8.5f, 11, 6, 3, 1.5f, c);
        *extent = 15;
        return 7;
    case ADSB_KIND_LARGE:
    case ADSB_KIND_HIGH_VORTEX: { // airliner: two engines under the wings
        float k = kind == ADSB_KIND_HIGH_VORTEX ? 1.12f : 1.0f;
        part_fuselage(&o[0], -17 * k, 15 * k, 5 * k, c);
        part_wing_pair(&o[1], 2.5f * k, -5 * k, 1.5f * k, 17 * k, 9 * k, 2.5f * k, c);
        part_pods(&o[3], 7.5f * k, -4 * k, 1 * k, 2.6f * k, c);
        part_wing_pair(&o[5], 1.5f * k, 10 * k, 13 * k, 7 * k, 4 * k, 2 * k, c);
        *extent = 19 * k;
        return 7;
    }
    case ADSB_KIND_HEAVY: // jumbo: four engines, the biggest
        part_fuselage(&o[0], -21, 18, 6.5f, c);
        part_wing_pair(&o[1], 3.2f, -6, 2.5f, 22, 12, 3, c);
        part_pods(&o[3], 8, -5.5f, -0.5f, 3, c);
        part_pods(&o[5], 15, -1, 4, 3, c);
        part_wing_pair(&o[7], 2, 12, 16, 9.5f, 4.5f, 2.5f, c);
        *extent = 24;
        return 9;
    case ADSB_KIND_FAST: // military jet: a delta
        POLY(&o[0], c, {0, -12}, {11, 8}, {-11, 8});
        part_fuselage(&o[1], -17, 11, 4, c);
        *extent = 17;
        return 2;
    case ADSB_KIND_ROTOR: // helicopter: cabin, tail boom and rotor, crossed blades
        part_circle(&o[0], 0, 0, 5.5f, c, false);
        part_bar(&o[1], 0, 3, 0, 15, 2.5f, c);
        part_bar(&o[2], -3.5f, 14, 3.5f, 14, 2, c);
        part_bar(&o[3], -11, -11, 11, 11, 2, c);
        part_bar(&o[4], 11, -11, -11, 11, 2, c);
        *extent = 16;
        return 5;
    case ADSB_KIND_GLIDER: // long thin wings
        part_fuselage(&o[0], -8, 11, 3, c);
        part_bar(&o[1], -19, -2, 19, -2, 3, c);
        part_bar(&o[2], -5, 10, 5, 10, 2, c);
        *extent = 19;
        return 3;
    case ADSB_KIND_BALLOON: // envelope, ropes and basket
        *turns = false;
        part_circle(&o[0], 0, -4, 9, c, false);
        part_bar(&o[1], -6, 2, -2, 8, 1.5f, c);
        part_bar(&o[2], 6, 2, 2, 8, 1.5f, c);
        part_rect(&o[3], -3, 8, 3, 12, c, false);
        *extent = 14;
        return 4;
    case ADSB_KIND_PARACHUTE: // canopy, lines and jumper
        *turns = false;
        POLY(&o[0], c, {-10, -1}, {-8, -7}, {-3, -10}, {3, -10}, {8, -7}, {10, -1});
        part_bar(&o[1], -9, -1, -1, 7, 1.5f, c);
        part_bar(&o[2], 9, -1, 1, 7, 1.5f, c);
        part_circle(&o[3], 0, 9, 3, c, false);
        *extent = 13;
        return 4;
    case ADSB_KIND_ULTRALIGHT: // hang glider: a wide delta and its keel
        POLY(&o[0], c, {0, -8}, {15, 5}, {-15, 5});
        part_bar(&o[1], 0, -8, 0, 8, 2, c);
        *extent = 15;
        return 2;
    case ADSB_KIND_UAV: // quadcopter
        *turns = false;
        part_bar(&o[0], -8, -8, 8, 8, 2.5f, c);
        part_bar(&o[1], 8, -8, -8, 8, 2.5f, c);
        part_rect(&o[2], -3, -3, 3, 3, c, false);
        part_circle(&o[3], -8, -8, 3.5f, c, false);
        part_circle(&o[4], 8, -8, 3.5f, c, false);
        part_circle(&o[5], 8, 8, 3.5f, c, false);
        part_circle(&o[6], -8, 8, 3.5f, c, false);
        *extent = 13;
        return 7;
    default: // no category: a plain aeroplane
        part_fuselage(&o[0], -14, 11, 5, c);
        part_wing_pair(&o[1], 2.5f, -4, 2, 13, 5, 2.5f, c);
        part_wing_pair(&o[3], 1.5f, 6.5f, 9.5f, 6, 3, 2, c);
        *extent = 14;
        return 5;
    }
}

static bool adsb_stale(const adsb_target_t *t, int64_t now_us)
{
    return now_us - t->pos_us > (int64_t)ADSB_STALE_S * 1000000;
}

static void draw_adsb(lv_layer_t *layer, placed_t *pl, int64_t now_us)
{
    const adsb_aircraft_t *a = &pl->ac->a;
    float x, y;
    geo_project(&s_view.bbox, UI_SIZE, UI_SIZE, a->lat, a->lon, &x, &y);
    float dx = x - UI_CENTER, dy = y - UI_CENTER;
    pl->shown = false;
    if (dx * dx + dy * dy > (float)(UI_CENTER + 40) * (UI_CENTER + 40)) return; // off the round screen

    lv_opa_t opa = adsb_stale(pl->ac, now_us) ? LV_OPA_50 : LV_OPA_COVER;
    lv_color_t color = adsb_is_emergency(a) ? ADSB_EMERG_COLOR : ADSB_COLOR;
    bool track = a->fields & ADSB_F_TRACK;
    if (track && (a->fields & ADSB_F_GS) && a->gs_kn >= HEADING_MIN_KN) {
        float vlen = fminf(a->gs_kn * 1852.0f * ADSB_VECTOR_MINUTES / 60.0f / s_view.m_per_px, ADSB_VECTOR_MAX_PX);
        float s = sinf(a->track_deg * (float)M_PI / 180), c = cosf(a->track_deg * (float)M_PI / 180);
        lv_point_precise_t p0 = {x, y}, p1 = {x + s * vlen, y - c * vlen};
        draw_dashed_line(layer, p0, p1, lv_color_white(), 2 + 2 * HALO_PX, (lv_opa_t)(opa * 85 / 100));
        draw_dashed_line(layer, p0, p1, color, 2, opa);
    }
    part_t parts[ADSB_MAX_PARTS];
    bool turns;
    float extent;
    int n = adsb_parts(adsb_kind(a), color, parts, &turns, &extent);
    float dir = turns && track ? a->track_deg * (float)M_PI / 180 : 0;
    parts_place(parts, n, sinf(dir), cosf(dir), x, y);
    draw_parts(layer, parts, n, false, opa);
    place(pl, x, y, extent, opa);
}

// Call sign (else registration, else address) per the label options, then the
// altitude, which always shows. An emergency always shows its call sign and squawk.
static void adsb_label_text(const adsb_target_t *t, char *text, size_t cap)
{
    const adsb_aircraft_t *a = &t->a;
    bool emerg = adsb_is_emergency(a);
    char name[24] = "", line2[32] = "";
    if (s_filter.label_name || emerg) {
        if (a->flight[0]) strlcpy(name, a->flight, sizeof(name));
        else if (a->reg[0]) strlcpy(name, a->reg, sizeof(name));
        else snprintf(name, sizeof(name), "%06lX", (unsigned long)a->addr);
        if (emerg && (a->fields & ADSB_F_SQUAWK)) {
            size_t k = strlen(name);
            snprintf(name + k, sizeof(name) - k, " %04u", a->squawk);
        }
    }
    int n = 0;
    if (a->fields & ADSB_F_GROUND) n = snprintf(line2, sizeof(line2), "GND");
    else if (a->fields & ADSB_F_ALT) n = snprintf(line2, sizeof(line2), "%ld ft", (long)a->alt_ft);
    if (s_filter.label_speed && (a->fields & ADSB_F_GS) && a->gs_kn >= HEADING_MIN_KN)
        snprintf(line2 + n, sizeof(line2) - n, "%s%.0f kn", n ? ", " : "", a->gs_kn);
    snprintf(text, cap, "%s%s%s", name, name[0] && line2[0] ? "\n" : "", line2);
}

// The status bubble: a boat before the vessel count and a plane before the
// aircraft count. White, upright as the text reads (tops to the rim).
#define STATUS_ICON_PX      24
#define STATUS_ICON_GAP_PX  8

static struct {
    bool boat, plane, plane_ok; // plane_ok: dump1090 is answering (else the plane is greyed)
} s_status_icons;

static void draw_pill_icon(lv_layer_t *layer, const part_t *src, int n, float angle_deg, lv_color_t color)
{
    const pill_t *p = &s_pills[PILL_STATUS];
    float rm = pill_outer_r() - p->width / 2.0f, a = angle_deg * (float)M_PI / 180;
    part_t parts[4];
    memcpy(parts, src, n * sizeof(parts[0]));
    float h = a + (float)M_PI / 2; // icon up = out towards the rim
    parts_place(parts, n, sinf(h), cosf(h), UI_CENTER + rm * cosf(a), UI_CENTER + rm * sinf(a));
    for (int i = 0; i < n; i++) {
        const part_t *q = &parts[i];
        for (int k = 1; k + 1 < q->n; k++) draw_triangle(layer, q->p[0], q->p[k], q->p[k + 1], color, LV_OPA_COVER);
        for (int k = 2; k + 1 < q->n; k++) draw_line(layer, q->p[0], q->p[k], color, 1, LV_OPA_COVER);
    }
}

static void draw_status_icons(lv_layer_t *layer)
{
    const pill_t *p = &s_pills[PILL_STATUS];
    if (p->hidden) return;
    lv_color_t w = lv_color_white();
    if (s_status_icons.boat && p->lead_deg) {
        // As far before the count as the plane is before the aircraft count.
        float rm = pill_outer_r() - p->width / 2.0f;
        float text = p->center - p->dir * (p->span / 2 - PILL_PAD_DEG - p->lead_deg);
        float at = text - p->dir * px_to_deg(STATUS_ICON_GAP_PX + STATUS_ICON_PX / 2.0f, rm);
        part_t boat[3];
        POLY(&boat[0], w, {-11, 4}, {11, 4}, {7, 9}, {-7, 9});  // hull
        POLY(&boat[1], w, {1, -11}, {1, 2}, {9, 2});           // mainsail
        POLY(&boat[2], w, {-1, -8}, {-1, 2}, {-7, 2});         // jib
        draw_pill_icon(layer, boat, 3, at, w);
    }
    if (s_status_icons.plane && p->text2[0]) {
        part_t plane[3];
        part_fuselage(&plane[0], -11, 9, 4, w);
        part_wings(&plane[1], 2, -3, 11, 4, 2.5f, w);
        part_wings(&plane[2], 1.5f, 5, 5, 3, 2, w);
        draw_pill_icon(layer, plane, 3, p->mid_angle, s_status_icons.plane_ok ? w : UI_COLOR_MUTED);
    }
}

// Emergency beacons: AIS-SART (970...), man overboard (972...), EPIRB (974...).
static bool is_beacon(uint32_t mmsi)
{
    uint32_t p = mmsi / 1000000;
    return p == 970 || p == 972 || p == 974;
}

#define BEACON_ACTIVE_S 600     // a distress message this recent: the beacon is going off
static uint32_t s_distress_mmsi[SAFETY_MSG_MAX];
static int s_ndistress;

// Which beacons have sent a distress message lately (with each new snapshot).
static void update_distress(void)
{
    static safety_msg_t msgs[SAFETY_MSG_MAX];
    int n = vessel_db_safety_msgs(msgs, SAFETY_MSG_MAX);
    int64_t now = esp_timer_get_time();
    s_ndistress = 0;
    for (int i = 0; i < n; i++) {
        if (msgs[i].kind == AIS_SAFETY_DISTRESS && now - msgs[i].last_us < (int64_t)BEACON_ACTIVE_S * 1000000)
            s_distress_mmsi[s_ndistress++] = msgs[i].mmsi;
    }
}

// Going off (not just being tested): its position reports say so (navigational
// status 14, "AIS-SART active"), or it sent a distress message lately.
static bool beacon_active(const vessel_t *v)
{
    if ((v->fields & AIS_F_NAVSTAT) && v->nav_status == 14) return true;
    for (int i = 0; i < s_ndistress; i++) {
        if (s_distress_mmsi[i] == v->mmsi) return true;
    }
    return false;
}

// The chart symbol for an AIS-SART: a circle with a cross in it. Red when going
// off; smaller and grey in test.
static void draw_beacon(lv_layer_t *layer, placed_t *pl, float x, float y, lv_opa_t opa)
{
    bool active = beacon_active(pl->v);
    float r = active ? 13 : 9;
    lv_color_t c = active ? MARK_RED : lv_color_hex(0x9AA5B1);
    for (int pass = 0; pass < 3; pass++) {
        lv_color_t col = pass == 0 ? lv_color_white() : pass == 1 ? lv_color_hex(0x101010) : c;
        int w = pass == 0 ? 3 + 2 * HALO_PX + 2 : pass == 1 ? 5 : 3;
        lv_opa_t o = pass == 0 ? (lv_opa_t)(opa * 85 / 100) : opa;
        lv_draw_arc_dsc_t a;
        lv_draw_arc_dsc_init(&a);
        a.center = (lv_point_t){(int32_t)x, (int32_t)y};
        a.radius = (int32_t)(r + w / 2.0f);
        a.width = w;
        a.start_angle = 0;
        a.end_angle = 360;
        a.color = col;
        a.opa = o;
        lv_draw_arc(layer, &a);
        float k = r * 0.7071f; // the cross, corner to corner
        draw_line(layer, (lv_point_precise_t){x - k, y - k}, (lv_point_precise_t){x + k, y + k}, col, w, o);
        draw_line(layer, (lv_point_precise_t){x - k, y + k}, (lv_point_precise_t){x + k, y - k}, col, w, o);
    }
    place(pl, x, y, r + 4, opa);
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
    if (is_beacon(v->mmsi)) {
        draw_beacon(layer, pl, x, y, opa);
        return;
    }
    if (v->cls == AIS_CLASS_ATON) {
        draw_aton(layer, pl, x, y, opa);
        return;
    }
    if (vessel_is_aircraft(v)) {
        draw_aircraft(layer, pl, x, y, opa);
        return;
    }
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

    place(pl, x, y, extent, opa);
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
        if (pl->ac) adsb_label_text(pl->ac, text, sizeof(text));
        else label_text(pl->v, text, sizeof(text));
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

// ---------------------------------------------------------------------------
// Weather stations heard over AIS: a station circle and a wind barb
// ---------------------------------------------------------------------------

#define BARB_STAFF_PX   36      // the staff, from the circle's edge out towards where the wind comes from
#define BARB_FULL_PX    15      // a 10 kn barb (5 kn: half), 50 kn: a pennant
#define BARB_STEP_PX    6
#define STATION_R       6

// Each line twice: dark and wider under, light over, to read on any background.
static void barb_line(lv_layer_t *layer, lv_point_precise_t a, lv_point_precise_t b, lv_opa_t opa, int pass)
{
    draw_line(layer, a, b, pass ? lv_color_white() : lv_color_hex(0x101010), pass ? 2 : 5, opa);
}

static void draw_barb(lv_layer_t *layer, float x, float y, const ais_met_t *m, lv_opa_t opa)
{
    // Speeds to the nearest 5 kn, as barbs are drawn; under 2.5 kn is calm: a ring round the station.
    int kn = (int)lroundf(m->wind_kn / 5.0f) * 5;
    float d = m->wind_dir * (float)M_PI / 180;
    lv_point_precise_t u = {sinf(d), -cosf(d)};                  // out from the station, into the wind
    float side = m->lat < 0 ? -1.0f : 1.0f;                     // barbs on the right in the north, left in the south
    lv_point_precise_t r = {cosf(d) * side, sinf(d) * side};
    for (int pass = 0; pass < 2; pass++) {
        if (kn == 0) {
            lv_draw_arc_dsc_t a;
            lv_draw_arc_dsc_init(&a);
            a.center = (lv_point_t){(int32_t)x, (int32_t)y};
            a.radius = STATION_R + 6 + (pass ? 0 : 2);
            a.width = pass ? 2 : 5;
            a.start_angle = 0;
            a.end_angle = 360;
            a.color = pass ? lv_color_white() : lv_color_hex(0x101010);
            a.opa = opa;
            lv_draw_arc(layer, &a);
            continue;
        }
        float len = STATION_R + BARB_STAFF_PX;
        lv_point_precise_t base = {x + u.x * STATION_R, y + u.y * STATION_R}, tip = {x + u.x * len, y + u.y * len};
        barb_line(layer, base, tip, opa, pass);
        // Feathers from the far end inwards: pennants (50), barbs (10), a half (5).
        float at = len;
        int left = kn;
        // A feather leaves the staff at `at` and slants out at 70°, away from the station.
        #define FEATHER(l) (lv_point_precise_t){x + u.x * at + (r.x * 0.94f + u.x * 0.34f) * (l), \
                                                 y + u.y * at + (r.y * 0.94f + u.y * 0.34f) * (l)}
        while (left >= 50) {
            lv_point_precise_t p0 = {x + u.x * at, y + u.y * at}, p1 = {x + u.x * (at - 8), y + u.y * (at - 8)};
            lv_point_precise_t tipf = FEATHER(BARB_FULL_PX);
            if (pass) draw_triangle(layer, p0, tipf, p1, lv_color_white(), opa);
            barb_line(layer, p0, tipf, opa, pass);
            barb_line(layer, tipf, p1, opa, pass);
            at -= 8 + BARB_STEP_PX / 2;
            left -= 50;
        }
        for (; left >= 10; left -= 10, at -= BARB_STEP_PX) {
            barb_line(layer, (lv_point_precise_t){x + u.x * at, y + u.y * at}, FEATHER(BARB_FULL_PX), opa, pass);
        }
        if (left >= 5) {
            if (at > len - 1) at -= BARB_STEP_PX; // a lone half barb sits a step in from the end
            barb_line(layer, (lv_point_precise_t){x + u.x * at, y + u.y * at}, FEATHER(BARB_FULL_PX / 2.0f), opa, pass);
        }
        #undef FEATHER
    }
}

static void draw_met_stations(lv_layer_t *layer, int64_t now)
{
    for (int i = 0; i < s_nmet; i++) {
        const ais_met_t *m = &s_met[i].m;
        if (!(m->fields & AIS_MET_POS)) continue;
        float x, y;
        geo_project(&s_view.bbox, UI_SIZE, UI_SIZE, m->lat, m->lon, &x, &y);
        float dx = x - UI_CENTER, dy = y - UI_CENTER;
        if (dx * dx + dy * dy > (float)(UI_CENTER + 40) * (UI_CENTER + 40)) continue;
        lv_opa_t opa = now - s_met[i].last_us > (int64_t)MET_STALE_S * 1000000 ? LV_OPA_50 : LV_OPA_COVER;
        if (m->fields & AIS_MET_WIND) draw_barb(layer, x, y, m, opa);
        // The station: an open circle (filled dark, so it shows on light water too).
        lv_draw_rect_dsc_t c;
        lv_draw_rect_dsc_init(&c);
        c.radius = LV_RADIUS_CIRCLE;
        c.bg_color = UI_COLOR_BG;
        c.bg_opa = opa;
        c.border_color = lv_color_white();
        c.border_width = 2;
        c.border_opa = opa;
        c.outline_color = lv_color_hex(0x101010);
        c.outline_width = 1;
        c.outline_opa = opa;
        lv_area_t a = {(int32_t)x - STATION_R, (int32_t)y - STATION_R, (int32_t)x + STATION_R, (int32_t)y + STATION_R};
        lv_draw_rect(layer, &c, &a);

        hit_t *h = &s_hits[s_nhits++];
        *h = (hit_t){.mmsi = s_met[i].mmsi, .x = (int16_t)x, .y = (int16_t)y, .r = HIT_RADIUS_MIN, .met = (int16_t)i};
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
    // ADS-B aircraft above the vessels, with an emergency on top of everything.
    // Their labels give way to the vessels' (the lowest aircraft's first), but
    // an emergency's always shows.
    for (int i = 0; i < s_nac; i++) {
        const adsb_target_t *t = &s_ac[i];
        bool emerg = adsb_is_emergency(&t->a);
        float prio = emerg ? 1000000.0f
                           : -100000.0f - ((t->a.fields & ADSB_F_ALT) ? t->a.alt_ft / 1000.0f : 0) -
                                 (adsb_stale(t, now) ? 1000 : 0);
        s_placed[n++] = (placed_t){.ac = t, .prio = prio, .layer = (emerg ? 4000000.0f : 3000000.0f) + prio, .labelled = true};
    }
    // Bottom layer first: the weather stations, then the targets.
    draw_met_stations(layer, now);
    qsort(s_placed, n, sizeof(s_placed[0]), placed_cmp);
    for (int i = 0; i < n; i++) {
        if (s_placed[i].ac) draw_adsb(layer, &s_placed[i], now);
        else draw_vessel(layer, &s_placed[i], now);
    }
    draw_labels(layer, n);
    draw_wind_and_waves(layer);
    draw_pills(layer);
    draw_time_icon(layer);
    draw_net_icon(layer);
    draw_tide_wave_icon(layer);
    draw_status_icons(layer);
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
    if (best < 0) return;
    if (s_hits[best].met >= 0) ui_met_open(&s_met[s_hits[best].met]);
    else if (s_hits[best].aircraft) ui_show_aircraft(s_hits[best].mmsi);
    else ui_show_detail(s_hits[best].mmsi);
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

    // Vessel count (after a boat) while either AIS source (network feed,
    // RTL-SDR) is live; then, while ADS-B is on, the aircraft count (after a
    // plane), which takes in AIS SAR aircraft too.
    settings_t cfg;
    settings_get(&cfg);
    adsb_status_t as;
    adsb_get_status(&as);
    s_status_icons.plane = cfg.adsb_enabled && cfg.adsb_url[0];
    s_status_icons.plane_ok = as.state == ADSB_OK;
    int nvessels = 0, nplanes = s_nac;
    for (int i = 0; i < s_nvessels; i++) {
        const vessel_t *v = &s_vessels[i];
        if (v->cls == AIS_CLASS_ATON) continue; // not the marks
        if (s_status_icons.plane && vessel_is_aircraft(v)) nplanes++;
        else nvessels++;
    }
    nmea_status_t ns;
    nmea_client_get_status(&ns);
    sdr_status_t ss;
    sdr_get_status(&ss);
    bool sdr_live = ss.state == SDR_READY || ss.state == SDR_CAPTURING;
    s_status_icons.boat = ns.state == NMEA_STATE_CONNECTED || sdr_live;
    if (s_status_icons.boat) {
        snprintf(buf, sizeof(buf), "%d", nvessels);
    } else if ((ns.state == NMEA_STATE_NOT_CONFIGURED || ns.state == NMEA_STATE_OFF) && ss.state != SDR_TESTING) {
        strcpy(buf, "#f5b13f AIS not set#");
    } else {
        strcpy(buf, "#f2545b AIS offline#");
    }
    pill_set_text(&s_pills[PILL_STATUS], buf);
    pill_set_lead_room(&s_pills[PILL_STATUS], s_status_icons.boat ? STATUS_ICON_PX : 0); // the gap after it comes out of the padding
    char planes[8] = "";
    if (s_status_icons.plane) snprintf(planes, sizeof(planes), "%d", nplanes);
    pill_set_text2(&s_pills[PILL_STATUS], planes, 2 * STATUS_ICON_GAP_PX + STATUS_ICON_PX);

    // Centre message for setup problems.
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    const char *ip = ws.sta_ip[0] || !ws.ap_active ? ws.sta_ip : ws.ap_ip;
    buf[0] = '\0';
    if (map_tiles_state() == MAP_TILES_NO_CARD) {
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
    s_nmet = met_db_snapshot(s_met, MET_MAX);
    s_nac = adsb_snapshot(s_ac, ADSB_MAX);
    update_distress();
    if (lv_screen_active() == s_scr) lv_obj_invalidate(s_overlay);
    update_border_text();
    update_net_icon();
    update_weather();
}

// Tapping the status bubble: the vessel list, or with ADS-B on, a choice of
// that or the aircraft list.
static lv_obj_t *s_chooser;

static void chooser_cb(lv_event_t *e)
{
    lv_obj_set_hidden(s_chooser, true);
    void (*open)(void) = lv_event_get_user_data(e);
    if (open) open();
}

static void status_tap(void)
{
    if (s_status_icons.plane) lv_obj_set_hidden(s_chooser, false);
    else ui_vessels_open();
}

static void chooser_create(void)
{
    s_chooser = lv_obj_create(s_scr); // dims the map; a tap outside the box closes it
    lv_obj_remove_style_all(s_chooser);
    lv_obj_set_size(s_chooser, UI_SIZE, UI_SIZE);
    lv_obj_set_style_bg_color(s_chooser, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_chooser, LV_OPA_50, 0);
    lv_obj_set_scrollable(s_chooser, false);
    lv_obj_add_event_cb(s_chooser, chooser_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_chooser, true);

    lv_obj_t *box = lv_obj_create(s_chooser);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(box, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 24, 0);
    lv_obj_set_style_border_color(box, UI_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_set_style_pad_row(box, 16, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(box, false);
    lv_obj_center(box);
    lv_obj_add_event_cb(ui_round_button(box, "Vessels", 260, 72), chooser_cb, LV_EVENT_CLICKED, ui_vessels_open);
    lv_obj_add_event_cb(ui_round_button(box, "Aircraft", 260, 72), chooser_cb, LV_EVENT_CLICKED, ui_aircraft_open);
}

static void zoom_in(void) { set_zoom(s_zoom - 1); }
static void zoom_out(void) { set_zoom(s_zoom + 1); }

// ---------------------------------------------------------------------------

lv_obj_t *ui_map_create(void)
{
    s_vessels = heap_caps_malloc(VESSEL_MAX * sizeof(vessel_t), MALLOC_CAP_SPIRAM);
    s_ac = heap_caps_malloc(ADSB_MAX * sizeof(adsb_target_t), MALLOC_CAP_SPIRAM);
    s_hits = heap_caps_malloc((VESSEL_MAX + MET_MAX + ADSB_MAX) * sizeof(hit_t), MALLOC_CAP_SPIRAM);
    s_placed = heap_caps_malloc((VESSEL_MAX + ADSB_MAX) * sizeof(placed_t), MALLOC_CAP_SPIRAM);
    s_label_order = heap_caps_malloc((VESSEL_MAX + ADSB_MAX) * sizeof(placed_t *), MALLOC_CAP_SPIRAM);
    s_label_areas = heap_caps_malloc((VESSEL_MAX + ADSB_MAX) * sizeof(lv_area_t), MALLOC_CAP_SPIRAM);
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
    pill_create(&s_pills[PILL_STATUS], &lv_font_montserrat_24, 180, 0, LV_ARCLABEL_DIR_CLOCKWISE, status_tap);
    s_pills[PILL_STATUS].lbl2 = pill_label(&s_pills[PILL_STATUS], &lv_font_montserrat_24, 180, LV_ARCLABEL_DIR_CLOCKWISE);
    // Zoom on the right edge: + above the current radius, - below.
    pill_create(&s_pills[PILL_ZOOM_IN], &lv_font_montserrat_28, -21, 9, LV_ARCLABEL_DIR_CLOCKWISE, zoom_in);
    pill_create(&s_pills[PILL_ZOOM], &lv_font_montserrat_24, 0, 0, LV_ARCLABEL_DIR_CLOCKWISE, NULL);
    pill_create(&s_pills[PILL_ZOOM_OUT], &lv_font_montserrat_28, 21, 9, LV_ARCLABEL_DIR_CLOCKWISE, zoom_out);
    pill_set_text(&s_pills[PILL_ZOOM_IN], LV_SYMBOL_PLUS);
    pill_set_text(&s_pills[PILL_ZOOM_OUT], LV_SYMBOL_MINUS);

    ui_vessels_create(s_scr);
    ui_tide_create(s_scr);
    ui_weather_create(s_scr);
    ui_aircraft_create(s_scr);
    chooser_create();

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
    s_nmet = met_db_snapshot(s_met, MET_MAX);
    s_nac = adsb_snapshot(s_ac, ADSB_MAX);
    update_distress();
    lv_obj_invalidate(s_overlay);
}
