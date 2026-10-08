// Captive-portal popup: when the Wi-Fi network wants a web sign-in, offer the
// login hotspot (see wifi_mgr.h), then show what to join and how long is left.
// Opens by itself once per portal; the Wi-Fi icon by the time and the Network
// screen open it again.

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "wifi_mgr.h"

#define POPUP_W         540
#define POPUP_Y         140
#define START_WAIT_MS   8000    // give up on "Starting..." after this

static lv_obj_t *s_popup, *s_title, *s_body, *s_creds, *s_ssid, *s_pass, *s_state, *s_btn, *s_btn_lbl;
static uint32_t s_starting;     // lv_tick when Start was pressed, 0 = not waiting
static bool s_start_failed;
static char s_auto_ssid[33];    // network the popup has opened by itself for

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_recolor(l, true);
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
}

static void popup_close(void)
{
    if (s_popup) lv_obj_delete(s_popup);
    s_popup = NULL;
    s_starting = 0;
}

static void x_cb(lv_event_t *e)
{
    popup_close();
}

void ui_portal_start(void)
{
    s_starting = lv_tick_get() | 1;
    s_start_failed = false;
    wifi_mgr_portal_assist(true);
    ui_portal_open();
}

static void btn_cb(lv_event_t *e)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    if (w.portal_assist) wifi_mgr_portal_assist(false);
    else ui_portal_start();
}

static void refresh(void)
{
    if (!s_popup) return;
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    if (w.state == WIFI_MGR_AP) { // setup mode has its own instructions
        popup_close();
        return;
    }
    if (w.portal_assist) s_starting = 0;
    if (s_starting && lv_tick_elaps(s_starting) > START_WAIT_MS) {
        s_starting = 0;
        s_start_failed = true;
    }

    char buf[256];
    unsigned min = (w.portal_assist_left_s + 59) / 60;
    if (w.portal_assist) {
        set_text(s_title, w.net == WIFI_NET_ONLINE ? "Signed in" : "Login hotspot on");
        set_text(s_body, w.net == WIFI_NET_ONLINE
                             ? "The display is online. You can close this."
                             : "Join this network from your phone. The sign-in page should open by itself; "
                               "if not, open any http:// web page.");
        set_text(s_ssid, w.ap_ssid);
        set_text(s_pass, w.ap_pass);
        if (w.net == WIFI_NET_ONLINE) {
            snprintf(buf, sizeof(buf), "#3ccf7a Online.#  Hotspot off in %u min", min);
        } else {
            snprintf(buf, sizeof(buf), "Waiting for sign-in...  %u min left", min);
        }
        set_text(s_state, buf);
        set_text(s_btn_lbl, "Stop hotspot");
    } else {
        set_text(s_title, "Wi-Fi sign-in needed");
        if (s_starting) {
            strcpy(buf, "Starting the login hotspot...");
        } else if (s_start_failed) {
            strcpy(buf, "#f2545b The login hotspot didn't start.# Make sure the display is connected to the "
                        "Wi-Fi network, then try again.");
        } else {
            snprintf(buf, sizeof(buf),
                     "\"%s\" wants a sign-in on a web page before the display can reach the internet.\n"
                     "Start the login hotspot, join it from your phone and sign in there.",
                     w.sta_ssid);
        }
        set_text(s_body, buf);
        set_text(s_state, "");
        set_text(s_btn_lbl, "Start login hotspot");
    }
    lv_obj_set_hidden(s_creds, !w.portal_assist);
    lv_obj_set_hidden(s_state, !w.portal_assist);
    if (s_starting) lv_obj_add_state(s_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_btn, LV_STATE_DISABLED);
}

void ui_portal_open(void)
{
    if (s_popup) {
        refresh();
        return;
    }
    // A plain holder for the box and, over its corner, the X.
    s_popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_popup);
    lv_obj_set_size(s_popup, POPUP_W, LV_SIZE_CONTENT);
    lv_obj_align(s_popup, LV_ALIGN_TOP_MID, 0, POPUP_Y);
    lv_obj_set_scrollable(s_popup, false);
    lv_obj_set_clickable(s_popup, true); // touches on it don't reach the screen underneath

    lv_obj_t *box = lv_obj_create(s_popup);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_style_pad_top(box, 20, 0);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, UI_COLOR_WARN, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_shadow_width(box, 24, 0);
    lv_obj_set_style_shadow_color(box, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(box, LV_OPA_60, 0);
    lv_obj_set_scrollable(box, false);

    s_title = label(box, &lv_font_montserrat_24, UI_COLOR_WARN);
    lv_obj_set_style_pad_hor(s_title, 56, 0); // clear of the X
    s_body = label(box, &lv_font_montserrat_16, UI_COLOR_TEXT);

    s_creds = lv_obj_create(box);
    lv_obj_remove_style_all(s_creds);
    lv_obj_set_size(s_creds, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_creds, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_creds, 4, 0);
    lv_label_set_text(label(s_creds, &lv_font_montserrat_16, UI_COLOR_MUTED), "Join Wi-Fi network");
    s_ssid = label(s_creds, &lv_font_montserrat_28, UI_COLOR_TEXT);
    lv_label_set_text(label(s_creds, &lv_font_montserrat_16, UI_COLOR_MUTED), "Password");
    s_pass = label(s_creds, &lv_font_montserrat_48, UI_COLOR_WARN);
    s_state = label(box, &lv_font_montserrat_20, UI_COLOR_TEXT);

    s_btn = ui_round_button(box, "", 320, 64);
    s_btn_lbl = lv_obj_get_child(s_btn, 0);
    lv_obj_set_style_text_font(s_btn_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_opa(s_btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *x = ui_round_button(s_popup, LV_SYMBOL_CLOSE, 52, 52);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_add_event_cb(x, x_cb, LV_EVENT_CLICKED, NULL);

    refresh();
}

// Open by itself the first time a network shows a portal. Once it's been
// online, a portal again (the sign-in expired) opens it again.
static void timer_cb(lv_timer_t *t)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    if (w.net == WIFI_NET_ONLINE && !w.portal_assist) s_auto_ssid[0] = '\0';
    if (w.state == WIFI_MGR_STA_CONNECTED && w.net == WIFI_NET_PORTAL && !w.portal_assist &&
        strcmp(s_auto_ssid, w.sta_ssid) != 0) {
        strlcpy(s_auto_ssid, w.sta_ssid, sizeof(s_auto_ssid));
        ui_portal_open();
    }
    refresh();
}

void ui_portal_init(void)
{
    lv_timer_create(timer_cb, 1000, NULL);
}
