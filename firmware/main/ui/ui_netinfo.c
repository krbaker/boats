// Network info screen (opened by holding a finger on the screen for 10 s).

#include <stdio.h>
#include <string.h>

#include "nmea_client.h"
#include "tide.h"
#include "ui.h"
#include "vessel_db.h"
#include "wifi_mgr.h"

#define CONFIRM_TIMEOUT_MS 4000

static lv_obj_t *s_scr, *s_info, *s_ap_box, *s_ap_ssid, *s_ap_pass, *s_ap_url, *s_reset_btn, *s_reset_lbl;
static uint32_t s_confirm_since; // 0 = not awaiting confirmation

static void back_cb(lv_event_t *e)
{
    ui_show_map();
}

static void set_confirm(bool on)
{
    s_confirm_since = on ? lv_tick_get() | 1 : 0;
    lv_label_set_text(s_reset_lbl, on ? "Tap again to confirm" : "Reset to AP mode");
    lv_obj_set_style_bg_color(s_reset_btn, on ? UI_COLOR_BAD : UI_COLOR_PANEL, 0);
}

static void reset_cb(lv_event_t *e)
{
    if (!s_confirm_since) {
        set_confirm(true);
        return;
    }
    set_confirm(false);
    wifi_mgr_start_ap();
}

static void refresh(void)
{
    wifi_mgr_status_t w;
    wifi_mgr_get_status(&w);
    nmea_status_t n;
    nmea_client_get_status(&n);
    tide_status_t t;
    tide_get_status(&t);

    if (s_confirm_since && lv_tick_elaps(s_confirm_since) > CONFIRM_TIMEOUT_MS) set_confirm(false);

    char buf[512];
    int len = 0;
    if (w.state == WIFI_MGR_AP) {
        lv_obj_set_hidden(s_ap_box, false);
        lv_obj_set_hidden(s_reset_btn, true);
        lv_label_set_text(s_ap_ssid, w.ap_ssid);
        lv_label_set_text(s_ap_pass, w.ap_pass);
        lv_label_set_text_fmt(s_ap_url, "then open  http://%s/", w.ap_ip);
        len += snprintf(buf + len, sizeof(buf) - len, "#f5b13f Setup (access point) mode#\n");
    } else {
        lv_obj_set_hidden(s_ap_box, true);
        lv_obj_set_hidden(s_reset_btn, false);
        if (w.state == WIFI_MGR_STA_CONNECTED) {
            len += snprintf(buf + len, sizeof(buf) - len,
                            "Wi-Fi: #3ccf7a %s#\nIP address: #3fa7f5 %s#\nSignal: %d dBm\nSettings: http://%s/\n", w.sta_ssid,
                            w.sta_ip, w.rssi, w.sta_ip);
        } else {
            len += snprintf(buf + len, sizeof(buf) - len, "Wi-Fi: #f5b13f connecting to %s...#\n", w.sta_ssid);
        }
    }

    len += snprintf(buf + len, sizeof(buf) - len, "\nAIS: %s%s%s", nmea_state_str(n.state),
                    n.host[0] ? " to " : "", n.host);
    if (n.host[0]) len += snprintf(buf + len, sizeof(buf) - len, ":%u", n.port);
    if (n.state != NMEA_STATE_CONNECTED && n.last_error[0]) {
        len += snprintf(buf + len, sizeof(buf) - len, "\n(%s)", n.last_error);
    }
    len += snprintf(buf + len, sizeof(buf) - len, "\n%lu messages, %d vessels", (unsigned long)n.stats.reports,
                    vessel_db_count());
    if (t.station[0]) {
        len += snprintf(buf + len, sizeof(buf) - len, "\nTide station: %s %s", t.station, t.station_name);
    }
    if (w.state == WIFI_MGR_AP) {
        // Keep it short so the access-point details below fit.
        char *nl = strchr(buf, '\n');
        if (nl) nl[1] = '\0';
    }
    if (strcmp(lv_label_get_text(s_info), buf) != 0) lv_label_set_text(s_info, buf);
}

static void timer_cb(lv_timer_t *tm)
{
    if (lv_screen_active() == s_scr) refresh();
}

static lv_obj_t *centered_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, 560);
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

    lv_obj_t *title = centered_label(s_scr, &lv_font_montserrat_32, UI_COLOR_ACCENT);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Network");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 90);

    s_info = centered_label(s_scr, &lv_font_montserrat_20, UI_COLOR_TEXT);
    lv_label_set_recolor(s_info, true);
    lv_obj_set_style_text_line_space(s_info, 6, 0);
    lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, 160);

    // Access-point credentials, shown in setup mode.
    s_ap_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_ap_box);
    lv_obj_set_size(s_ap_box, 600, 330);
    lv_obj_align(s_ap_box, LV_ALIGN_TOP_MID, 0, 200);
    lv_obj_set_flex_flow(s_ap_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ap_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ap_box, 6, 0);
    lv_label_set_text(centered_label(s_ap_box, &lv_font_montserrat_20, UI_COLOR_MUTED), "Join Wi-Fi network");
    s_ap_ssid = centered_label(s_ap_box, &lv_font_montserrat_32, UI_COLOR_TEXT);
    lv_label_set_text(centered_label(s_ap_box, &lv_font_montserrat_20, UI_COLOR_MUTED), "Password");
    s_ap_pass = centered_label(s_ap_box, &lv_font_montserrat_48, UI_COLOR_WARN);
    s_ap_url = centered_label(s_ap_box, &lv_font_montserrat_24, UI_COLOR_ACCENT);
    lv_obj_set_hidden(s_ap_box, true);

    s_reset_btn = ui_round_button(s_scr, "", 300, 64);
    s_reset_lbl = lv_obj_get_child(s_reset_btn, 0);
    lv_obj_set_style_text_font(s_reset_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_reset_btn, LV_ALIGN_BOTTOM_MID, 0, -140);
    lv_obj_add_event_cb(s_reset_btn, reset_cb, LV_EVENT_CLICKED, NULL);
    set_confirm(false);

    lv_obj_t *back = ui_round_button(s_scr, LV_SYMBOL_LEFT " Map", 160, 64);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    lv_timer_create(timer_cb, 1000, NULL);
    return s_scr;
}

void ui_netinfo_on_show(void)
{
    set_confirm(false);
    refresh();
}
