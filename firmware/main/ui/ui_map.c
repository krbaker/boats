// Map screen: chart background, AIS targets, zoom buttons, and time / tide /
// status text curved around the round display's border.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "geo.h"
#include "map_images.h"
#include "nmea_client.h"
#include "settings.h"
#include "tide.h"
#include "ui.h"
#include "vessel_db.h"
#include "wifi_mgr.h"

#define RING_WIDTH      52      // dark band around the edge holding the border text
#define RING_TEXT_R     (UI_CENTER - RING_WIDTH / 2)
#define MIN_SHIP_LEN    18      // px; ships are never drawn smaller than this
#define MIN_SHIP_WIDTH  8
#define STALE_S         180     // dim targets not heard for this long
#define HIT_RADIUS_MIN  22
#define VECTOR_MINUTES  6       // course/speed vector length

// Radii used for ship scaling when no chart has been uploaded.
static const float k_default_radii[] = {0.5f, 1.0f, 2.0f, 4.0f};

typedef struct {
    uint32_t mmsi;
    int16_t x, y, r;
} hit_t;

typedef struct {
    geo_bbox_t bbox;
    float m_per_px;
    float radius_nm;
    int nlevels;
} view_t;

static lv_obj_t *s_scr, *s_img, *s_overlay, *s_msg;
static lv_obj_t *s_time_lbl, *s_tide_lbl, *s_status_lbl, *s_zoom_lbl;
static lv_obj_t *s_btn_in, *s_btn_out;

static vessel_t *s_vessels;
static int s_nvessels;
static hit_t *s_hits;
static int s_nhits;
static int s_zoom;
static view_t s_view;
static EventGroupHandle_t s_ev;

// ---------------------------------------------------------------------------
// View geometry
// ---------------------------------------------------------------------------

static void update_view(void)
{
    settings_t cfg;
    settings_get(&cfg);
    const map_manifest_t *m = map_images_state() == MAP_IMAGES_READY ? map_images_manifest() : NULL;

    if (m) {
        s_view.nlevels = m->nlevels;
        if (s_zoom >= m->nlevels) s_zoom = m->nlevels - 1;
        const map_level_t *l = &m->levels[s_zoom];
        s_view.bbox = l->bbox;
        s_view.m_per_px = l->m_per_px;
        s_view.radius_nm = l->radius_nm;
    } else {
        s_view.nlevels = sizeof(k_default_radii) / sizeof(k_default_radii[0]);
        if (s_zoom >= s_view.nlevels) s_zoom = s_view.nlevels - 1;
        float r = k_default_radii[s_zoom];
        double dlat = r / 60.0, dlon = dlat / cos(cfg.center_lat * M_PI / 180.0);
        s_view.bbox = (geo_bbox_t){cfg.center_lat - dlat, cfg.center_lat + dlat, cfg.center_lon - dlon, cfg.center_lon + dlon};
        s_view.m_per_px = 2 * r * 1852.0f / UI_SIZE;
        s_view.radius_nm = r;
    }

    const lv_image_dsc_t *img = m ? map_images_level(s_zoom) : NULL;
    if (img) {
        lv_image_set_src(s_img, img);
        lv_obj_set_hidden(s_img, false);
    } else {
        lv_obj_set_hidden(s_img, true);
    }

    lv_label_set_text_fmt(s_zoom_lbl, "%g nm", s_view.radius_nm);
    lv_obj_set_state(s_btn_in, LV_STATE_DISABLED, s_zoom == 0);
    lv_obj_set_state(s_btn_out, LV_STATE_DISABLED, s_zoom >= s_view.nlevels - 1);
    lv_obj_invalidate(s_overlay);
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

static void draw_label(lv_layer_t *layer, const char *text, int32_t cx, int32_t top, lv_opa_t opa)
{
    const lv_font_t *font = &lv_font_montserrat_14;
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    lv_area_t bg = {cx - size.x / 2 - 4, top, cx + size.x / 2 + 4, top + size.y + 3};
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = lv_color_black();
    r.bg_opa = opa * 55 / 100;
    r.radius = 4;
    lv_draw_rect(layer, &r, &bg);

    lv_draw_label_dsc_t l;
    lv_draw_label_dsc_init(&l);
    l.text = text;
    l.font = font;
    l.color = lv_color_white();
    l.opa = opa;
    l.align = LV_TEXT_ALIGN_CENTER;
    lv_area_t a = {bg.x1, top + 1, bg.x2, top + size.y + 1};
    lv_draw_label(layer, &l, &a);
}

static void draw_vessel(lv_layer_t *layer, const vessel_t *v, int64_t now_us)
{
    float x, y;
    geo_project(&s_view.bbox, UI_SIZE, UI_SIZE, v->lat, v->lon, &x, &y);
    float dx = x - UI_CENTER, dy = y - UI_CENTER;
    if (dx * dx + dy * dy > (float)(UI_CENTER + 40) * (UI_CENTER + 40)) return; // off the round screen

    bool stale = now_us - v->last_heard_us > (int64_t)STALE_S * 1000000;
    lv_opa_t opa = stale ? LV_OPA_50 : LV_OPA_COVER;
    lv_color_t color = ui_ship_color(v->ship_type);
    lv_color_t outline = lv_color_hex(0x101010);

    bool moving = (v->fields & AIS_F_SOG) && v->sog_kn >= 0.5f;
    bool has_dir = (v->fields & AIS_F_HEADING) || ((v->fields & AIS_F_COG) && moving);
    float dir = (v->fields & AIS_F_HEADING) ? v->heading : v->cog_deg;

    // Size from the reported dimensions, scaled to the chart, with a floor so
    // small boats stay visible and touchable.
    float len = MIN_SHIP_LEN, wid = MIN_SHIP_WIDTH;
    float off_x = 0, off_y = 0; // hull centre relative to the GPS antenna, hull frame
    if (v->fields & AIS_F_DIM) {
        float lm = v->dim_a + v->dim_b;
        float wm = v->dim_c + v->dim_d ? (float)(v->dim_c + v->dim_d) : lm * 0.3f;
        float true_len = lm / s_view.m_per_px;
        len = fmaxf(true_len, MIN_SHIP_LEN);
        wid = fmaxf(wm / s_view.m_per_px, MIN_SHIP_WIDTH);
        if (wid > len * 0.6f) wid = len * 0.6f;
        if (true_len > MIN_SHIP_LEN) {
            off_y = -((float)v->dim_a - v->dim_b) / 2 / s_view.m_per_px;
            off_x = ((float)v->dim_d - v->dim_c) / 2 / s_view.m_per_px;
        }
    }

    // Course/speed vector.
    if ((v->fields & AIS_F_COG) && moving) {
        float vlen = v->sog_kn * 1852.0f * VECTOR_MINUTES / 60.0f / s_view.m_per_px;
        if (vlen > 160) vlen = 160;
        float s = sinf(v->cog_deg * (float)M_PI / 180), c = cosf(v->cog_deg * (float)M_PI / 180);
        draw_line(layer, (lv_point_precise_t){x, y}, (lv_point_precise_t){x + s * vlen, y - c * vlen}, color, 2, opa);
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
        draw_triangle(layer, bow, sb_sh, sb_st, color, opa);
        draw_triangle(layer, bow, sb_st, pt_st, color, opa);
        draw_triangle(layer, bow, pt_st, pt_sh, color, opa);
        lv_point_precise_t poly[] = {bow, sb_sh, sb_st, pt_st, pt_sh, bow};
        for (int i = 0; i < 5; i++) draw_line(layer, poly[i], poly[i + 1], outline, 2, opa);
        extent = hypotf(hl + fabsf(off_y), hw + fabsf(off_x));
    } else {
        // No heading known (stationary): draw a dot.
        float d = fmaxf(wid, 14);
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = color;
        r.bg_opa = opa;
        r.radius = LV_RADIUS_CIRCLE;
        r.border_color = outline;
        r.border_width = 2;
        r.border_opa = opa;
        lv_area_t a = {(int32_t)(x - d / 2), (int32_t)(y - d / 2), (int32_t)(x + d / 2), (int32_t)(y + d / 2)};
        lv_draw_rect(layer, &r, &a);
        extent = d / 2;
    }

    char text[48];
    const char *name = v->name[0] ? v->name : NULL;
    char mmsi[16];
    if (!name) {
        snprintf(mmsi, sizeof(mmsi), "%lu", (unsigned long)v->mmsi);
        name = mmsi;
    }
    if (v->fields & AIS_F_SOG) {
        snprintf(text, sizeof(text), "%s\n%.1f kn", name, v->sog_kn);
    } else {
        snprintf(text, sizeof(text), "%s", name);
    }
    draw_label(layer, text, (int32_t)x, (int32_t)(y + extent + 4), opa);

    hit_t *h = &s_hits[s_nhits++];
    h->mmsi = v->mmsi;
    h->x = (int16_t)x;
    h->y = (int16_t)y;
    h->r = (int16_t)fmaxf(extent, HIT_RADIUS_MIN);
}

static void draw_own_position(lv_layer_t *layer)
{
    settings_t cfg;
    settings_get(&cfg);
    float x, y;
    geo_project(&s_view.bbox, UI_SIZE, UI_SIZE, cfg.center_lat, cfg.center_lon, &x, &y);
    lv_color_t c = lv_color_hex(0xff2d6f);
    draw_line(layer, (lv_point_precise_t){x - 12, y}, (lv_point_precise_t){x - 4, y}, c, 3, LV_OPA_COVER);
    draw_line(layer, (lv_point_precise_t){x + 4, y}, (lv_point_precise_t){x + 12, y}, c, 3, LV_OPA_COVER);
    draw_line(layer, (lv_point_precise_t){x, y - 12}, (lv_point_precise_t){x, y - 4}, c, 3, LV_OPA_COVER);
    draw_line(layer, (lv_point_precise_t){x, y + 4}, (lv_point_precise_t){x, y + 12}, c, 3, LV_OPA_COVER);
}

static void overlay_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    int64_t now = esp_timer_get_time();
    draw_own_position(layer);
    s_nhits = 0;
    // Stale targets first so live ones draw on top.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s_nvessels; i++) {
            const vessel_t *v = &s_vessels[i];
            if (!(v->fields & AIS_F_POS)) continue;
            bool stale = now - v->last_heard_us > (int64_t)STALE_S * 1000000;
            if (stale == (pass == 0)) draw_vessel(layer, v, now);
        }
    }
}

static void overlay_click_cb(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
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

static lv_obj_t *make_arclabel(lv_obj_t *parent, const lv_font_t *font, int start, int size, lv_arclabel_dir_t dir)
{
    lv_obj_t *a = lv_arclabel_create(parent);
    lv_obj_set_size(a, UI_SIZE, UI_SIZE);
    lv_obj_center(a);
    lv_obj_set_clickable(a, false);
    lv_obj_set_style_text_font(a, font, 0);
    lv_obj_set_style_text_color(a, lv_color_white(), 0);
    lv_arclabel_set_radius(a, RING_TEXT_R);
    lv_arclabel_set_angle_start(a, start);
    lv_arclabel_set_angle_size(a, size);
    lv_arclabel_set_dir(a, dir);
    lv_arclabel_set_text_vertical_align(a, LV_ARCLABEL_TEXT_ALIGN_CENTER);
    lv_arclabel_set_text_horizontal_align(a, LV_ARCLABEL_TEXT_ALIGN_CENTER);
    lv_arclabel_set_recolor(a, true);
    lv_arclabel_set_text(a, "");
    return a;
}

static void set_text_if_changed(lv_obj_t *obj, char *last, size_t cap, const char *text, bool arc)
{
    if (strcmp(last, text) == 0) return;
    strlcpy(last, text, cap);
    if (arc) lv_arclabel_set_text(obj, text);
    else lv_label_set_text(obj, text);
}

static void update_border_text(void)
{
    static char last_time[32], last_tide[96], last_status[64], last_msg[160];
    char buf[160];
    time_t now = time(NULL);

    if (clock_is_set()) {
        ui_format_time(now, buf, sizeof(buf), false);
    } else {
        strcpy(buf, "--:--");
    }
    set_text_if_changed(s_time_lbl, last_time, sizeof(last_time), buf, true);

    tide_status_t ts;
    tide_get_status(&ts);
    tide_event_t next;
    if (ts.state == TIDE_NO_STATION) {
        strcpy(buf, "Tide: no station set");
    } else if (clock_is_set() && tide_next(now, &next)) {
        char t[16];
        ui_format_time(next.t, t, sizeof(t), false);
        snprintf(buf, sizeof(buf), "%s %s %s  %.1f ft", next.high ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                 next.high ? "High" : "Low", t, next.height_ft);
    } else if (ts.state == TIDE_ERROR) {
        strcpy(buf, "Tide unavailable");
    } else {
        strcpy(buf, "Tide: loading...");
    }
    set_text_if_changed(s_tide_lbl, last_tide, sizeof(last_tide), buf, true);

    nmea_status_t ns;
    nmea_client_get_status(&ns);
    if (ns.state == NMEA_STATE_CONNECTED) {
        snprintf(buf, sizeof(buf), "%d vessel%s", s_nvessels, s_nvessels == 1 ? "" : "s");
    } else if (ns.state == NMEA_STATE_NOT_CONFIGURED) {
        strcpy(buf, "#f5b13f AIS not set#");
    } else {
        strcpy(buf, "#f2545b AIS offline#");
    }
    set_text_if_changed(s_status_lbl, last_status, sizeof(last_status), buf, true);

    // Centre message for setup problems.
    wifi_mgr_status_t ws;
    wifi_mgr_get_status(&ws);
    const char *ip = ws.state == WIFI_MGR_AP ? ws.ap_ip : ws.sta_ip;
    buf[0] = '\0';
    if (ws.state == WIFI_MGR_AP) {
        snprintf(buf, sizeof(buf), "Setup mode: join Wi-Fi \"%s\"\npassword %s\nthen open http://%s/", ws.ap_ssid,
                 ws.ap_pass, ws.ap_ip);
    } else if (map_images_state() == MAP_IMAGES_LOADING) {
        strcpy(buf, "Loading chart...");
    } else if (map_images_state() == MAP_IMAGES_ERROR) {
        strcpy(buf, "Chart could not be loaded");
    } else if (map_images_state() == MAP_IMAGES_NONE) {
        snprintf(buf, sizeof(buf), "No chart uploaded%s%s%s", ip[0] ? "\nUpload one at http://" : "", ip, ip[0] ? "/" : "");
    }
    set_text_if_changed(s_msg, last_msg, sizeof(last_msg), buf, false);
    if (buf[0]) lv_obj_set_hidden(s_msg, false);
    else lv_obj_set_hidden(s_msg, true);
}

static void refresh_timer_cb(lv_timer_t *t)
{
    EventBits_t bits = xEventGroupClearBits(s_ev, SETTINGS_EV_MAP | SETTINGS_EV_CENTER | SETTINGS_EV_DISPLAY);
    if (bits & SETTINGS_EV_MAP) map_images_reload();
    if (bits & SETTINGS_EV_DISPLAY) {
        settings_t cfg;
        settings_get(&cfg);
        bsp_display_brightness_set(cfg.brightness);
    }
    if (map_images_poll() || (bits & SETTINGS_EV_CENTER)) update_view();

    s_nvessels = vessel_db_snapshot(s_vessels, VESSEL_MAX);
    if (lv_screen_active() == s_scr) lv_obj_invalidate(s_overlay);
    update_border_text();
}

static void zoom_in_cb(lv_event_t *e) { set_zoom(s_zoom - 1); }
static void zoom_out_cb(lv_event_t *e) { set_zoom(s_zoom + 1); }

// ---------------------------------------------------------------------------

lv_obj_t *ui_map_create(void)
{
    s_vessels = heap_caps_malloc(VESSEL_MAX * sizeof(vessel_t), MALLOC_CAP_SPIRAM);
    s_hits = heap_caps_malloc(VESSEL_MAX * sizeof(hit_t), MALLOC_CAP_SPIRAM);
    s_ev = settings_subscribe();
    settings_t cfg;
    settings_get(&cfg);
    s_zoom = cfg.zoom_index;
    bsp_display_brightness_set(cfg.brightness);

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x16324f), 0);
    lv_obj_set_scrollable(s_scr, false);

    s_img = lv_image_create(s_scr);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_set_size(s_img, UI_SIZE, UI_SIZE);
    lv_obj_set_hidden(s_img, true);

    s_overlay = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, UI_SIZE, UI_SIZE);
    lv_obj_set_clickable(s_overlay, true);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_add_event_cb(s_overlay, overlay_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_SHORT_CLICKED, NULL);

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

    // Dark band around the edge for the border text.
    lv_obj_t *ring = lv_obj_create(s_scr);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, UI_SIZE, UI_SIZE);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, RING_WIDTH, 0);
    lv_obj_set_style_border_color(ring, lv_color_black(), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_60, 0);
    lv_obj_set_clickable(ring, false);

    // Angles: 0 = 3 o'clock, increasing clockwise.
    s_time_lbl = make_arclabel(s_scr, &lv_font_montserrat_32, 230, 80, LV_ARCLABEL_DIR_CLOCKWISE);
    s_tide_lbl = make_arclabel(s_scr, &lv_font_montserrat_24, 35, 110, LV_ARCLABEL_DIR_COUNTER_CLOCKWISE);
    s_status_lbl = make_arclabel(s_scr, &lv_font_montserrat_20, 155, 60, LV_ARCLABEL_DIR_CLOCKWISE);

    // Zoom controls on the right, inside the ring.
    s_btn_in = ui_round_button(s_scr, LV_SYMBOL_PLUS, 68, 68);
    lv_obj_set_pos(s_btn_in, 656, 290);
    lv_obj_add_event_cb(s_btn_in, zoom_in_cb, LV_EVENT_CLICKED, NULL);
    s_btn_out = ui_round_button(s_scr, LV_SYMBOL_MINUS, 68, 68);
    lv_obj_set_pos(s_btn_out, 656, 442);
    lv_obj_add_event_cb(s_btn_out, zoom_out_cb, LV_EVENT_CLICKED, NULL);
    s_zoom_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_zoom_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_zoom_lbl, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_zoom_lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_zoom_lbl, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(s_zoom_lbl, 6, 0);
    lv_obj_set_style_pad_ver(s_zoom_lbl, 2, 0);
    lv_obj_set_style_radius(s_zoom_lbl, 6, 0);
    lv_obj_align(s_zoom_lbl, LV_ALIGN_LEFT_MID, 656, 0);
    lv_obj_set_width(s_zoom_lbl, 68);
    lv_obj_set_style_text_align(s_zoom_lbl, LV_TEXT_ALIGN_CENTER, 0);

    map_images_reload();
    update_view();
    lv_timer_create(refresh_timer_cb, 1000, NULL);
    return s_scr;
}

void ui_map_on_show(void)
{
    s_nvessels = vessel_db_snapshot(s_vessels, VESSEL_MAX);
    lv_obj_invalidate(s_overlay);
}
