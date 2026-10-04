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
#include "settings.h"
#include "tide.h"
#include "ui.h"
#include "vessel_db.h"
#include "wifi_mgr.h"

#define PILL_EDGE_GAP   4       // px between the pills and the edge of the screen
#define PILL_PAD_DEG    4       // extra arc either side of a pill's text
#define PILL_WIDTH      (lv_font_get_line_height(&lv_font_montserrat_28) + 10)  // every pill is this thick
#define MIN_SHIP_LEN    18      // px; ships are never drawn smaller than this
#define MIN_SHIP_WIDTH  8
#define HULL_M_PER_PX   (2 * 0.5f * 1852 / UI_SIZE)  // hulls are never drawn smaller than at the 0.5 nm zoom
#define STALE_MOVING_S  180     // dim vessels not heard for this long...
#define STALE_SLOW_S    420     // ...or this long if slow / at rest: those report only every 3 minutes
#define SLOW_KN         2.0f    // AIS Class B (and anchored Class A) drop to 3-minute reports below ~2 kn
#define HEADING_MIN_KN  1.0f    // no heading / course lines below this speed
#define HIT_RADIUS_MIN  22
#define VECTOR_MINUTES  6       // course/speed vector length
#define HEADING_LINE_PX 28      // heading line drawn ahead of the bow
#define HALO_PX         2       // light rim outside each vessel's dark outline
#define DASH_PX         8
#define DASH_GAP_PX     6
#define DRAG_SLOP_PX    10      // finger movement before a touch becomes a drag
#define DEFAULT_PAN_NM  10.0f   // pan limit when there is no chart
#define TILE_POLL_MS    40

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

// ---------------------------------------------------------------------------
// Edge pills
// ---------------------------------------------------------------------------

static inline int pill_outer_r(void) { return UI_CENTER - PILL_EDGE_GAP; }
static inline float px_to_deg(float px, float r) { return px / r * 180.0f / (float)M_PI; }

static void pill_create(pill_t *p, const lv_font_t *font, float center, float min_span, lv_arclabel_dir_t dir,
                        void (*on_tap)(void))
{
    p->center = center;
    p->min_span = p->span = min_span;
    p->width = PILL_WIDTH;
    p->on_tap = on_tap;
    p->hidden = true;

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
    p->lbl = a;
}

static void pill_set_text(pill_t *p, const char *text)
{
    if (strcmp(p->text, text) == 0 && p->hidden == !text[0]) return;
    strlcpy(p->text, text, sizeof(p->text));
    lv_arclabel_set_text(p->lbl, text);
    p->hidden = !text[0];
    p->span = fmaxf(p->min_span, (float)lv_arclabel_get_text_angle(p->lbl) + 2 * PILL_PAD_DEG);
    if (s_overlay) lv_obj_invalidate(s_overlay);
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
    } else {
        s_view.nlevels = sizeof(k_default_radii) / sizeof(k_default_radii[0]);
        if (s_zoom >= s_view.nlevels) s_zoom = s_view.nlevels - 1;
        float r = k_default_radii[s_zoom];
        s_view.m_per_px = 2 * r * 1852.0f / UI_SIZE;
        s_view.merc_px = s_view.m_per_px * s_view.scale;
        s_view.radius_nm = r;
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
    float hull_m_per_px = fminf(s_view.m_per_px, HULL_M_PER_PX);
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
    // differs from the heading when current or wind sets the vessel sideways,
    // so it is dashed to keep it distinct from the heading line.
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
        // Heading line straight ahead of the bow, in line with the hull.
        bool heading = (v->fields & AIS_F_HEADING) && moving;
        lv_point_precise_t tip = rot(off_x, off_y - hl - HEADING_LINE_PX, s, c, x, y);
        for (int i = 0; i < 5; i++) draw_line(layer, poly[i], poly[i + 1], halo, 2 + 2 * HALO_PX, halo_opa);
        if (heading) draw_line(layer, bow, tip, halo, 5 + 2 * HALO_PX, halo_opa);
        draw_triangle(layer, bow, sb_sh, sb_st, color, opa);
        draw_triangle(layer, bow, sb_st, pt_st, color, opa);
        draw_triangle(layer, bow, pt_st, pt_sh, color, opa);
        for (int i = 0; i < 5; i++) draw_line(layer, poly[i], poly[i + 1], outline, 2, opa);
        if (heading) {
            draw_line(layer, bow, tip, outline, 5, opa);
            draw_line(layer, bow, tip, color, 3, opa);
        }
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
    draw_pills(layer);
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

    nmea_status_t ns;
    nmea_client_get_status(&ns);
    if (ns.state == NMEA_STATE_CONNECTED) {
        snprintf(buf, sizeof(buf), "%d vessel%s", s_nvessels, s_nvessels == 1 ? "" : "s");
    } else if (ns.state == NMEA_STATE_NOT_CONFIGURED) {
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
    pill_create(&s_pills[PILL_TIME], &lv_font_montserrat_28, 270, 0, LV_ARCLABEL_DIR_CLOCKWISE, NULL);
    pill_create(&s_pills[PILL_TIDE], &lv_font_montserrat_24, 90, 0, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE, ui_tide_open);
    pill_create(&s_pills[PILL_STATUS], &lv_font_montserrat_24, 180, 0, LV_ARCLABEL_DIR_CLOCKWISE, ui_vessels_open);
    // Zoom on the right edge: + above the current radius, - below.
    pill_create(&s_pills[PILL_ZOOM_IN], &lv_font_montserrat_28, -21, 9, LV_ARCLABEL_DIR_CLOCKWISE, zoom_in);
    pill_create(&s_pills[PILL_ZOOM], &lv_font_montserrat_24, 0, 0, LV_ARCLABEL_DIR_CLOCKWISE, NULL);
    pill_create(&s_pills[PILL_ZOOM_OUT], &lv_font_montserrat_28, 21, 9, LV_ARCLABEL_DIR_CLOCKWISE, zoom_out);
    pill_set_text(&s_pills[PILL_ZOOM_IN], LV_SYMBOL_PLUS);
    pill_set_text(&s_pills[PILL_ZOOM_OUT], LV_SYMBOL_MINUS);

    ui_vessels_create(s_scr);
    ui_tide_create(s_scr);

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
