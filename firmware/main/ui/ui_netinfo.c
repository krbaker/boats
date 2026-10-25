// Network and settings screen (opened by holding a finger on the screen for
// 10 s): the Network tab here, the settings tabs in ui_settings.c.

#include <stdio.h>
#include <string.h>

#include "ais_fwd.h"
#include "log_save.h"
#include "nmea_client.h"
#include "sdr_probe.h"
#include "tide.h"
#include "ui.h"
#include "vessel_db.h"
#include "wifi_mgr.h"

#define CONFIRM_TIMEOUT_MS 4000
#define SAVE_RESULT_MS     5000    // how long the save button shows how it went
#define TABS_W             540     // the tab box; its corners just fit inside the round screen
#define TABS_H             560
#define TABS_Y             (-16)   // offset of its centre from the screen's

static lv_obj_t *s_scr, *s_tabs, *s_info, *s_ap_box, *s_ap_ssid, *s_ap_pass, *s_ap_url, *s_ap_btn, *s_ap_lbl;
static lv_obj_t *s_pass_cap, *s_pass_btn, *s_save_btn, *s_save_lbl;
static bool s_saving;            // a save started here hasn't finished
static uint32_t s_saved_at;      // lv_tick it finished, 0 = not showing a result
static uint32_t s_pass_confirm_since; // 0 = not awaiting confirmation of a new AP password

static void back_cb(lv_event_t *e)
{
    ui_show_map();
}

static void set_pass_confirm(bool on)
{
    s_pass_confirm_since = on ? lv_tick_get() | 1 : 0;
    lv_label_set_text(s_pass_cap, on ? "Tap " LV_SYMBOL_REFRESH " again for a new password" : "Password");
    lv_obj_set_style_text_color(s_pass_cap, on ? UI_COLOR_BAD : UI_COLOR_MUTED, 0);
    lv_obj_set_style_bg_color(s_pass_btn, on ? UI_COLOR_BAD : UI_COLOR_PANEL, 0);
}

// A new access-point password (it restarts the AP, so confirm first).
static void new_pass_cb(lv_event_t *e)
{
    if (!s_pass_confirm_since) {
        set_pass_confirm(true);
        return;
    }
    set_pass_confirm(false);
    wifi_mgr_new_ap_password();
}

// The access point (alongside the station) on or off.
static void ap_cb(lv_event_t *e)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    wifi_mgr_ap(!w.ap_active);
}

static void refresh(void);

// The log to the microSD card, in the background.
static void save_cb(lv_event_t *e)
{
    if (s_saving) return;
    s_saving = true;
    s_saved_at = 0;
    log_save_start();
    refresh();
}

static void refresh_save_btn(void)
{
    char msg[64], bl[80] = "Save logs";
    log_save_state_t st = log_save_state(msg, sizeof(msg));
    if (s_saving && st != LOG_SAVE_BUSY) {
        s_saving = false;
        s_saved_at = lv_tick_get() | 1;
    }
    if (s_saved_at && lv_tick_elaps(s_saved_at) > SAVE_RESULT_MS) s_saved_at = 0;
    if (s_saving) strcpy(bl, "Saving...");
    else if (s_saved_at && st == LOG_SAVE_OK) strcpy(bl, "#3ccf7a Saved to card#");
    else if (s_saved_at) snprintf(bl, sizeof(bl), "#f2545b %s#", msg);
    if (strcmp(lv_label_get_text(s_save_lbl), bl) != 0) lv_label_set_text(s_save_lbl, bl);
}

static const char *net_text(const wifi_mgr_status_t *w)
{
    switch (w->net) {
    case WIFI_NET_ONLINE: return "#3ccf7a online#";
    case WIFI_NET_PORTAL: return "#f5b13f captive portal (sign-in needed)#";
    case WIFI_NET_OFFLINE: return "#f2545b offline#";
    default: return "checking...";
    }
}

static void refresh(void)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    nmea_status_t n;
    nmea_client_get_status(&n);
    tide_status_t t;
    tide_get_status(&t);

    if (s_pass_confirm_since && lv_tick_elaps(s_pass_confirm_since) > CONFIRM_TIMEOUT_MS) set_pass_confirm(false);

    char buf[768];
    int len = 0;
    if (w.state == WIFI_MGR_STA_CONNECTED) {
        len += snprintf(buf + len, sizeof(buf) - len,
                        "Wi-Fi: #3ccf7a %s#  (%d dBm)\nIP address: #3fa7f5 %s#  /  %s\nGateway: %s\nMAC: %s\n"
                        "Internet: %s\nSettings: http://%s/\n",
                        w.sta_ssid, w.rssi, w.sta_ip, w.sta_netmask, w.sta_gw, w.sta_mac, net_text(&w), w.sta_ip);
    } else if (w.state == WIFI_MGR_STA_CONNECTING) {
        len += snprintf(buf + len, sizeof(buf) - len, "Wi-Fi: #f5b13f connecting to %s...#\nMAC: %s\n", w.sta_ssid,
                        w.sta_mac);
    } else {
        len += snprintf(buf + len, sizeof(buf) - len, "Wi-Fi: #f5b13f no network set up#\n");
    }

    lv_obj_set_hidden(s_ap_box, !w.ap_active);
    if (w.ap_active) {
        lv_label_set_text(s_ap_ssid, w.ap_ssid);
        if (strcmp(lv_label_get_text(s_ap_pass), w.ap_pass) != 0) lv_label_set_text(s_ap_pass, w.ap_pass);
        lv_label_set_text_fmt(s_ap_url, "then open  http://%s/", w.ap_ip);
    }
    // With no network set up the AP stays on: it's the only way in.
    char bl[40] = "Enable AP";
    if (w.ap_active && w.ap_left_s) snprintf(bl, sizeof(bl), "Disable AP (%u min left)", (w.ap_left_s + 59) / 60);
    else if (w.ap_active) strcpy(bl, "AP on");
    if (strcmp(lv_label_get_text(s_ap_lbl), bl) != 0) lv_label_set_text(s_ap_lbl, bl);
    if (w.ap_active && !w.ap_left_s) lv_obj_add_state(s_ap_btn, LV_STATE_DISABLED);
    else lv_obj_remove_state(s_ap_btn, LV_STATE_DISABLED);
    refresh_save_btn();

    len += snprintf(buf + len, sizeof(buf) - len, "\nAIS: %s%s%s", nmea_state_str(n.state),
                    n.host[0] ? " to " : "", n.host);
    if (n.host[0]) len += snprintf(buf + len, sizeof(buf) - len, ":%u", n.port);
    if (n.state != NMEA_STATE_CONNECTED && n.last_error[0]) {
        len += snprintf(buf + len, sizeof(buf) - len, "\n(%s)", n.last_error);
    }
    len += snprintf(buf + len, sizeof(buf) - len, "\n%lu messages, %d vessels", (unsigned long)n.stats.reports,
                    vessel_db_count());
    sdr_status_t r;
    sdr_get_status(&r);
    if (r.state != SDR_OFF) {
        len += snprintf(buf + len, sizeof(buf) - len, "\nRTL-SDR: %s", sdr_state_str(r.state));
        if (r.state == SDR_READY || r.state == SDR_CAPTURING) {
            len += snprintf(buf + len, sizeof(buf) - len, ", %lu packets, CPU %.0f%%",
                            (unsigned long)(r.demod.packets[0] + r.demod.packets[1]), r.cpu_pct);
        }
    }
    // Retransmit: one line, plus the first problem if a destination isn't sending.
    int on = 0, sending = 0;
    char problem[64] = "";
    for (int i = 0; i < FWD_DEST_MAX; i++) {
        fwd_status_t fs;
        ais_fwd_get_status(i, &fs);
        if (fs.state == FWD_STATE_OFF) continue;
        on++;
        if (fs.state == FWD_STATE_SENDING) sending++;
        else if (!problem[0]) snprintf(problem, sizeof(problem), "%d: %s", i + 1, fs.error[0] ? fs.error : ais_fwd_state_str(fs.state));
    }
    if (on) {
        len += snprintf(buf + len, sizeof(buf) - len, "\nRetransmit: sending to %d of %d", sending, on);
        if (problem[0]) len += snprintf(buf + len, sizeof(buf) - len, "\n(%s)", problem);
    }
    if (t.station[0]) {
        len += snprintf(buf + len, sizeof(buf) - len, "\nTide station: %s %s", t.station, t.station_name);
    }
    if (strcmp(lv_label_get_text(s_info), buf) != 0) lv_label_set_text(s_info, buf);
}

static void timer_cb(lv_timer_t *tm)
{
    if (lv_screen_active() != s_scr) return;
    refresh();
    ui_settings_refresh();
}

static lv_obj_t *centered_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

lv_obj_t *ui_netinfo_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, UI_COLOR_BG, 0);
    lv_obj_set_scrollable(s_scr, false);

    s_tabs = lv_tabview_create(s_scr);
    lv_obj_set_size(s_tabs, TABS_W, TABS_H);
    lv_obj_align(s_tabs, LV_ALIGN_CENTER, 0, TABS_Y);
    lv_obj_set_style_bg_color(s_tabs, UI_COLOR_BG, 0);
    lv_obj_set_style_radius(s_tabs, 24, 0);
    lv_obj_set_style_clip_corner(s_tabs, true, 0);
    lv_tabview_set_tab_bar_size(s_tabs, 52);
    // Tabs switch from the tab bar only, so drags on the slider don't swipe tabs.
    lv_obj_set_scrollable(lv_tabview_get_content(s_tabs), false);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tabs);
    lv_obj_set_style_bg_color(bar, UI_COLOR_PANEL, 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_20, 0);

    // Network: what it's connected to, the access point, saving the log.
    lv_obj_t *tab = lv_tabview_add_tab(s_tabs, "Network");
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(tab, 16, 0);
    lv_obj_set_style_pad_ver(tab, 16, 0);
    lv_obj_set_style_pad_row(tab, 14, 0);

    // Access-point credentials, at the top while the access point is on.
    s_ap_box = lv_obj_create(tab);
    lv_obj_remove_style_all(s_ap_box);
    lv_obj_set_size(s_ap_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ap_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ap_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ap_box, 6, 0);
    lv_label_set_text(centered_label(s_ap_box, &lv_font_montserrat_20, UI_COLOR_MUTED), "Join Wi-Fi network");
    s_ap_ssid = centered_label(s_ap_box, &lv_font_montserrat_32, UI_COLOR_TEXT);
    s_pass_cap = centered_label(s_ap_box, &lv_font_montserrat_20, UI_COLOR_MUTED);

    // The password with a new-password button to its right, and an empty
    // spacer as wide on its left so the password stays centred.
    lv_obj_t *row = lv_obj_create(s_ap_box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);
    lv_obj_t *spacer = lv_obj_create(row);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 56, 56);
    s_ap_pass = centered_label(row, &lv_font_montserrat_48, UI_COLOR_WARN);
    lv_obj_set_width(s_ap_pass, LV_SIZE_CONTENT);
    s_pass_btn = ui_round_button(row, LV_SYMBOL_REFRESH, 56, 56);
    lv_obj_add_event_cb(s_pass_btn, new_pass_cb, LV_EVENT_CLICKED, NULL);
    set_pass_confirm(false);

    s_ap_url = centered_label(s_ap_box, &lv_font_montserrat_24, UI_COLOR_ACCENT);
    lv_obj_set_hidden(s_ap_box, true);

    s_info = centered_label(tab, &lv_font_montserrat_20, UI_COLOR_TEXT);
    lv_label_set_recolor(s_info, true);
    lv_obj_set_style_text_line_space(s_info, 6, 0);

    // Side by side: the access point and saving the log.
    row = lv_obj_create(tab);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_ap_btn = ui_round_button(row, "", 240, 64);
    s_ap_lbl = lv_obj_get_child(s_ap_btn, 0);
    lv_obj_set_style_text_font(s_ap_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_opa(s_ap_btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_ap_btn, ap_cb, LV_EVENT_CLICKED, NULL);

    s_save_btn = ui_round_button(row, "Save logs", 240, 64);
    s_save_lbl = lv_obj_get_child(s_save_btn, 0);
    lv_obj_set_style_text_font(s_save_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_recolor(s_save_lbl, true);
    lv_obj_add_event_cb(s_save_btn, save_cb, LV_EVENT_CLICKED, NULL);

    ui_settings_build(s_tabs);
    // Each tab as wide as its name needs (equal widths cut "Retransmit" short).
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *b = lv_obj_get_child(bar, i);
        lv_obj_set_style_text_color(b, UI_COLOR_TEXT, 0);
        lv_obj_set_style_pad_hor(b, 4, 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if (l) lv_obj_set_flex_grow(b, (uint8_t)(strlen(lv_label_get_text(l)) + 3));
    }

    lv_obj_t *back = ui_round_button(s_scr, LV_SYMBOL_LEFT " Map", 160, 64);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_timer_create(timer_cb, 1000, NULL);
    return s_scr;
}

void ui_netinfo_on_show(void)
{
    set_pass_confirm(false);
    refresh();
    ui_settings_refresh();
}
