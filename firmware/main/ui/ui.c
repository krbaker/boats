#include "ui.h"

#include <stdio.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HOLD_SHOW_MS    2000    // show the progress ring after this long
#define HOLD_TRIGGER_MS 10000   // open the network page after this long
#define HOLD_SLOP_PX    30      // finger may wander this far

static lv_obj_t *s_map_scr, *s_detail_scr, *s_net_scr, *s_aircraft_scr;
static lv_obj_t *s_hold_arc, *s_hold_label;

static void load(lv_obj_t *scr)
{
    if (lv_screen_active() != scr) {
        lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_FADE_IN, 150, 0, false);
    }
}

void ui_show_map(void)
{
    ui_map_on_show();
    load(s_map_scr);
}

void ui_show_detail(uint32_t mmsi)
{
    ui_detail_set_vessel(mmsi);
    load(s_detail_scr);
}

void ui_show_aircraft(uint32_t id)
{
    ui_aircraft_detail_set(id);
    load(s_aircraft_scr);
}

void ui_show_netinfo(void)
{
    ui_netinfo_on_show();
    load(s_net_scr);
}

// Watch for a single point held for 10 s anywhere on any screen.
static void hold_timer_cb(lv_timer_t *t)
{
    static bool holding, fired;
    static uint32_t start_ms;
    static lv_point_t start_pt;

    lv_indev_t *indev = bsp_display_get_input_dev();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (lv_indev_get_state(indev) != LV_INDEV_STATE_PRESSED) {
        holding = fired = false;
        lv_obj_set_hidden(s_hold_arc, true);
        return;
    }
    if (!holding || LV_ABS(pt.x - start_pt.x) > HOLD_SLOP_PX || LV_ABS(pt.y - start_pt.y) > HOLD_SLOP_PX) {
        holding = true;
        fired = false;
        start_ms = lv_tick_get();
        start_pt = pt;
        lv_obj_set_hidden(s_hold_arc, true);
        return;
    }
    if (fired) return;

    uint32_t held = lv_tick_elaps(start_ms);
    if (held >= HOLD_TRIGGER_MS) {
        fired = true;
        lv_obj_set_hidden(s_hold_arc, true);
        lv_indev_wait_release(indev); // swallow the release so nothing underneath gets clicked
        ui_show_netinfo();
    } else if (held >= HOLD_SHOW_MS && lv_screen_active() != s_net_scr) {
        lv_obj_set_hidden(s_hold_arc, false);
        lv_obj_set_pos(s_hold_arc, LV_CLAMP(0, start_pt.x - 80, UI_SIZE - 160), LV_CLAMP(0, start_pt.y - 80, UI_SIZE - 160));
        lv_arc_set_value(s_hold_arc, (held - HOLD_SHOW_MS) * 100 / (HOLD_TRIGGER_MS - HOLD_SHOW_MS));
        lv_label_set_text_fmt(s_hold_label, "%lu", (unsigned long)((HOLD_TRIGGER_MS - held + 999) / 1000));
    }
}

static void create_hold_indicator(void)
{
    s_hold_arc = lv_arc_create(lv_layer_top());
    lv_obj_set_size(s_hold_arc, 160, 160);
    lv_arc_set_rotation(s_hold_arc, 270);
    lv_arc_set_bg_angles(s_hold_arc, 0, 360);
    lv_arc_set_range(s_hold_arc, 0, 100);
    lv_obj_remove_style(s_hold_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_hold_arc, false);
    lv_obj_set_style_arc_width(s_hold_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_hold_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_hold_arc, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_hold_arc, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_hold_arc, UI_COLOR_ACCENT, LV_PART_INDICATOR);
    s_hold_label = lv_label_create(s_hold_arc);
    lv_obj_set_style_text_font(s_hold_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_hold_label, lv_color_white(), 0);
    lv_obj_center(s_hold_label);
    lv_obj_set_hidden(s_hold_arc, true);
}

void ui_start(void)
{
    bsp_display_lock(0);
    s_map_scr = ui_map_create();
    s_detail_scr = ui_detail_create();
    s_net_scr = ui_netinfo_create();
    s_aircraft_scr = ui_aircraft_detail_create();
    create_hold_indicator();
    ui_safety_init();
    ui_map_on_show();
    lv_screen_load(s_map_scr);
    lv_timer_create(hold_timer_cb, 100, NULL);
    bsp_display_unlock();
}

// ---------------------------------------------------------------------------
// Firmware update screen
// ---------------------------------------------------------------------------

static lv_obj_t *s_updating;

void ui_updating_begin(void)
{
    if (s_updating) return;
    bsp_display_lock(0);
    s_updating = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_updating);
    lv_obj_set_size(s_updating, UI_SIZE, UI_SIZE);
    lv_obj_set_style_bg_color(s_updating, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_updating, LV_OPA_COVER, 0);
    lv_obj_t *lbl = lv_label_create(s_updating);
    lv_label_set_text(lbl, "Updating firmware...\n\nDon't turn off the power");
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
    lv_obj_center(lbl);
    bsp_display_unlock();
    esp_lv_adapter_refresh_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(100));    // let the new frame reach the panel
    esp_lv_adapter_pause(1000);        // then draw nothing until the restart
}

void ui_updating_end(void)
{
    if (!s_updating) return;
    esp_lv_adapter_resume();
    bsp_display_lock(0);
    lv_obj_delete(s_updating);
    s_updating = NULL;
    bsp_display_unlock();
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

lv_obj_t *ui_round_button(lv_obj_t *parent, const char *text, int32_t w, int32_t h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, UI_COLOR_MUTED, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
    lv_obj_center(lbl);
    return btn;
}

lv_obj_t *ui_overlay_create(lv_obj_t *parent, lv_event_cb_t close_cb)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_SIZE, UI_SIZE);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, UI_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_90, 0);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_20, 0);
    lv_obj_set_clickable(o, true); // swallow touches meant for the map underneath
    lv_obj_set_scrollable(o, false);
    lv_obj_set_hidden(o, true);

    lv_obj_t *close = ui_round_button(o, LV_SYMBOL_CLOSE, 72, 72);
    lv_obj_align(close, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);
    return o;
}

// Strong, fairly dark colours that don't occur on NOAA charts (the chart's
// magenta, light blue and tan would hide pleasure craft, ferries and pilots).
lv_color_t ui_ship_color(uint8_t t)
{
    if (t == 30) return lv_color_hex(0xf06a00);                         // fishing: orange
    if (t == 36 || t == 37) return lv_color_hex(0x8a2be2);              // sailing / pleasure: violet
    if (t >= 60 && t <= 69) return lv_color_hex(0x1f4fe0);              // passenger: royal blue
    if (t >= 70 && t <= 79) return lv_color_hex(0x1f9d3a);              // cargo: green
    if (t >= 80 && t <= 89) return lv_color_hex(0xd42020);              // tanker: red
    if (t == 31 || t == 32 || t == 52) return lv_color_hex(0x00a6a6);   // tug / towing: teal
    if ((t >= 40 && t <= 59)) return lv_color_hex(0xffb000);            // HSC, pilot, SAR, law enf.: amber
    return lv_color_hex(0x6e6e6e);                                      // other / unknown: grey
}

void ui_format_time(time_t t, char *buf, size_t len, bool seconds)
{
    struct tm tm;
    localtime_r(&t, &tm);
    int h = tm.tm_hour % 12;
    if (h == 0) h = 12;
    if (seconds) {
        snprintf(buf, len, "%d:%02d:%02d %s", h, tm.tm_min, tm.tm_sec, tm.tm_hour < 12 ? "AM" : "PM");
    } else {
        snprintf(buf, len, "%d:%02d %s", h, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    }
}
