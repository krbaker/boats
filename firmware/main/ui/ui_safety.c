// AIS safety broadcasts (type 14): a popup over any screen when one arrives.
// Distress (an emergency beacon going off, or MAYDAY) is red; notices and free
// text amber; beacon tests are only listed (the Vessels overlay's Messages
// tab). A closed popup stays closed until its sender's text changes.

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "geo.h"
#include "settings.h"
#include "ui.h"
#include "vessel_db.h"

#define POPUP_W         520
#define POPUP_Y         150
#define POLL_MS         1000
#define MAX_DISMISSED   32
#define NOTICE_FRESH_S  (15 * 60)   // a notice older than this when first seen doesn't pop up

static lv_obj_t *s_popup;
static uint32_t s_popup_id;
static bool s_popup_distress;
static uint32_t s_dismissed[MAX_DISMISSED];
static int s_ndismissed;
static uint32_t s_gen;
static safety_msg_t s_msgs[SAFETY_MSG_MAX];

// A message's identity: its sender and text. An AIS-SART's is the beacon and
// whether it's in distress, so its position reports ("AIS-SART detected") and
// its own "SART ACTIVE" are one popup, closed once.
uint32_t ui_safety_msg_id(const safety_msg_t *m)
{
    uint32_t h = 2166136261u ^ m->mmsi;
    if (m->mmsi / 1000000 == 970) return (h * 16777619u ^ (uint32_t)m->kind) | 1;
    for (const char *p = m->text; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    return h | 1; // never 0: that means "no popup"
}

static bool dismissed(uint32_t id)
{
    for (int i = 0; i < s_ndismissed; i++) {
        if (s_dismissed[i] == id) return true;
    }
    return false;
}

static void popup_close(void)
{
    if (s_popup) lv_obj_delete(s_popup);
    s_popup = NULL;
    s_popup_id = 0;
}

static void check(bool force);

static void x_cb(lv_event_t *e)
{
    if (s_ndismissed == MAX_DISMISSED) { // forget the oldest
        memmove(s_dismissed, s_dismissed + 1, sizeof(s_dismissed) - sizeof(s_dismissed[0]));
        s_ndismissed--;
    }
    s_dismissed[s_ndismissed++] = s_popup_id;
    popup_close();
    check(true); // there may be another
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

const char *ui_safety_sender_kind(uint32_t mmsi)
{
    switch (mmsi / 1000000) {
    case 970: return "AIS-SART";
    case 972: return "Man overboard device";
    case 974: return "EPIRB";
    default: return NULL;
    }
}

// A 406 MHz beacon ID in the text, decoded (lines, the last without "\n"); the
// raw text is always shown too, since this is only a guess.
bool ui_safety_guess(const safety_msg_t *m, char *buf, size_t len)
{
    ais_beacon_id_t id;
    if (!ais_beacon_id_parse(m->text, &id)) return false;
    vessel_t v;
    bool known = id.mmsi && vessel_db_get(id.mmsi, &v);
    ais_beacon_id_describe(&id, known ? v.name : NULL, buf, len);
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
    return true;
}

// "WENATCHEE (366123456), 1.2 nm 045°" or just the MMSI and what it is.
void ui_safety_sender(const safety_msg_t *m, char *buf, size_t len)
{
    vessel_t v;
    bool known = vessel_db_get(m->mmsi, &v);
    const char *kind = ui_safety_sender_kind(m->mmsi);
    int n;
    if (known && v.name[0]) n = snprintf(buf, len, "%s (%lu)", v.name, (unsigned long)m->mmsi);
    else n = snprintf(buf, len, "%s %lu", kind ? kind : "MMSI", (unsigned long)m->mmsi);
    if (known && (v.fields & AIS_F_POS) && n < (int)len) {
        settings_t cfg;
        settings_get(&cfg);
        double d = geo_distance_nm(cfg.center_lat, cfg.center_lon, v.lat, v.lon);
        snprintf(buf + n, len - n, ", %.1f nm %03.0f\xc2\xb0", d,
                 geo_bearing_deg(cfg.center_lat, cfg.center_lon, v.lat, v.lon));
    }
}

static void popup_open(const safety_msg_t *m, uint32_t id)
{
    popup_close();
    bool distress = m->kind == AIS_SAFETY_DISTRESS;
    lv_color_t accent = distress ? UI_COLOR_BAD : UI_COLOR_WARN;
    s_popup_id = id;
    s_popup_distress = distress;
    s_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, POPUP_W, LV_SIZE_CONTENT);
    lv_obj_align(s_popup, LV_ALIGN_TOP_MID, 0, POPUP_Y);
    lv_obj_set_scrollable(s_popup, false);
    lv_obj_set_clickable(s_popup, true); // touches on it don't reach the screen underneath

    lv_obj_t *box = lv_obj_create(s_popup);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(box, 14, 0);
    lv_obj_set_style_pad_right(box, 64, 0); // room for the X
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, distress ? lv_color_hex(0x4a1c22) : lv_color_hex(0x40331a), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, accent, 0);
    lv_obj_set_style_border_width(box, distress ? 4 : 2, 0);
    lv_obj_set_style_shadow_width(box, 24, 0);
    lv_obj_set_style_shadow_color(box, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_60, 0);
    lv_obj_set_scrollable(box, false);

    char buf[96];
    label(box, distress ? LV_SYMBOL_WARNING " DISTRESS" : "Safety message", &lv_font_montserrat_28, accent);
    ui_safety_sender(m, buf, sizeof(buf));
    label(box, buf, &lv_font_montserrat_16, UI_COLOR_TEXT);
    label(box, m->text, &lv_font_montserrat_24, UI_COLOR_TEXT);
    char guess[256];
    if (ui_safety_guess(m, guess, sizeof(guess))) {
        label(box, UI_SAFETY_GUESS_TITLE, &lv_font_montserrat_16, UI_COLOR_MUTED);
        label(box, guess, &lv_font_montserrat_16, UI_COLOR_TEXT);
    }
    char t[32] = "";
    if (m->last_wall) ui_format_time(m->last_wall, t, sizeof(t), false);
    if (m->count > 1) snprintf(buf, sizeof(buf), "%s%sheard %lu times", t, t[0] ? ", " : "", (unsigned long)m->count);
    else snprintf(buf, sizeof(buf), "%s", t);
    if (buf[0]) label(box, buf, &lv_font_montserrat_16, UI_COLOR_MUTED);

    lv_obj_t *x = ui_round_button(s_popup, LV_SYMBOL_CLOSE, 52, 52);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_event_cb(x, x_cb, LV_EVENT_CLICKED, NULL);
}

// The newest distress message not yet closed, else the newest notice; a
// distress message takes over from a notice that's showing.
static void check(bool force)
{
    uint32_t gen = vessel_db_safety_generation();
    if (!force && gen == s_gen) return;
    s_gen = gen;
    int n = vessel_db_safety_msgs(s_msgs, SAFETY_MSG_MAX);
    int64_t now = esp_timer_get_time();
    const safety_msg_t *distress = NULL, *notice = NULL;
    uint32_t distress_id = 0, notice_id = 0;
    for (int i = 0; i < n; i++) { // newest first
        const safety_msg_t *m = &s_msgs[i];
        uint32_t id = ui_safety_msg_id(m);
        if (dismissed(id)) continue;
        if (m->kind == AIS_SAFETY_DISTRESS && !distress) {
            distress = m;
            distress_id = id;
        } else if (m->kind == AIS_SAFETY_TEXT && !notice && now - m->first_us < (int64_t)NOTICE_FRESH_S * 1000000) {
            notice = m;
            notice_id = id;
        }
    }
    const safety_msg_t *show = distress ? distress : notice;
    uint32_t id = distress ? distress_id : notice_id;
    if (!show) return;
    if (s_popup && s_popup_id == id) { // the same one: just bring its time and count up to date
        popup_open(show, id);
        return;
    }
    if (s_popup && s_popup_distress && !distress) return; // a notice never covers distress
    popup_open(show, id);
}

static void timer_cb(lv_timer_t *t)
{
    check(false);
}

void ui_safety_init(void)
{
    lv_timer_create(timer_cb, POLL_MS, NULL);
}
