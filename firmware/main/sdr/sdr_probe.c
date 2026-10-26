/*
 * RTL-SDR bring-up test (RTL_SDR_PLAN.md, steps 1-2): find an RTL2832U dongle
 * on the USB-A port, report what it is and how it connected, tune an R820T to
 * the AIS channels, stream samples, and measure USB throughput and the signal
 * power on each AIS channel. Results go to the serial log only.
 *
 * The RTL2832U register access and baseband set-up are ported from librtlsdr
 * (rtl-sdr-blog fork): Copyright (C) 2012-2014 Steve Markgraf, (C) 2012
 * Dimitri Stolnikov.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */

#include "sdr_probe.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "ais_demod.h"
#include "ais_fwd.h"
#include "map_store.h"
#include "r82xx.h"
#include "rx_stats.h"
#include "settings.h"
#include "vessel_db.h"

static const char *TAG = "sdr_probe";

#define RTL_VID             0x0bda
#define RTL_XTAL_HZ         28800000
#define BULK_EP             0x81
#define BULK_XFER_BYTES     (16 * 1024)
#define BULK_XFERS          4
#define CTRL_TIMEOUT_MS     300
#define RUN_MS              10000
#define AIS_CENTER_HZ       AIS_DEMOD_CENTER_HZ  // channel A at -25 kHz, B at +25 kHz
#define AIS_RATE            AIS_DEMOD_RATE
#define CAP_RING            (4 * 1024 * 1024)  // PSRAM: ~7 s of slack if the card stalls
#define CAP_CHUNK           (32 * 1024)        // written through an internal-RAM bounce buffer this big...
#define CAP_CHUNK_MIN       (4 * 1024)         // ...or smaller, down to this, when internal RAM is fragmented
#define CAP_DIR             MAP_ROOT "/sdr"
#define CAP_LOG             (512 * 1024)       // PSRAM: AIS sentences heard while recording
#define DEC_RING            (2 * 1024 * 1024)  // PSRAM: ~3.6 s of samples
#define DEC_CHUNK           (16 * 1024)
#define CPU_PERIOD_US       2000000
#define STALL_US            3000000   // no samples this long: restart the stream
#define RETRY_US            10000000  // between attempts to start the stream

// librtlsdr register blocks
enum { DEMODB = 0, USBB = 1, SYSB = 2, IICB = 6 };
#define USB_SYSCTL          0x2000
#define USB_EPA_CTL         0x2148
#define USB_EPA_MAXPKT      0x2158
#define DEMOD_CTL           0x3000
#define DEMOD_CTL_1         0x300b
#define GPO                 0x3001
#define GPOE                0x3003
#define GPD                 0x3004
#define BIAS_TEE_GPIO       0                  // as on the RTL-SDR Blog V3 (dongles without a bias-tee ignore it)
#define R820T_I2C_ADDR      0x34
#define R828D_I2C_ADDR      0x74
#define R82XX_CHECK_VAL     0x69

static const int k_fir_default[16] = {
    -54, -36, -41, -40, -32, -14, 14, 53,       // 8 bit signed
    101, 156, 215, 273, 327, 372, 404, 421,     // 12 bit signed
};

static usb_host_client_handle_t s_client;
static usb_device_handle_t s_dev;
static uint8_t s_new_addr;
static bool s_gone;
static TaskHandle_t s_probe_task;
static EventGroupHandle_t s_events;
static r82xx_t s_r82xx;
static SemaphoreHandle_t s_ctrl_done;
static usb_transfer_t *s_ctrl;

// Streaming statistics, updated from the transfer callback.
static volatile uint64_t s_bytes;
static volatile uint32_t s_xfer_errors, s_short_xfers;
static volatile bool s_streaming;
static volatile uint32_t s_hist_min = 255, s_hist_max;
static volatile uint64_t s_sum, s_count;
static volatile uint64_t s_clip_total, s_byte_total;
// Per-buffer power while the gain sweep measures a step.
#define MEAS_MAX 64
static volatile bool s_meas_on;
static volatile int s_meas_n;
static float s_meas_pw[MEAS_MAX];

// Gain: what's applied now, and the "best fixed gain" control.
static const int16_t k_gains[] = {0,   9,   14,  27,  37,  77,  87,  125, 144, 157, 166, 197, 207, 229, 254,
                                  280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496};
#define NGAINS ((int)(sizeof(k_gains) / sizeof(k_gains[0])))
#define SWEEP_SETTLE_MS   150
#define SWEEP_MEASURE_MS  600
#define FLOOR_MAX_DBFS    (-22.0f) // noise floor ceiling: ~9 ADC counts rms, ~20 dB of headroom above the noise
#define CLIP_OK           1e-4f   // fraction of samples clipped that's acceptable when choosing
#define GUARD_WINDOW_US   5000000LL
#define GUARD_CLIP        1e-3f   // more than this over a window: one step down
#define GUARD_RECOVER_US  (300 * 1000000LL)  // clean this long: one step back up
static volatile int16_t s_gain_now;     // tenths of a dB, SDR_GAIN_AUTO for the tuner AGC
static volatile int s_sweep_step;       // 1..NGAINS while sweeping, else 0
static volatile int s_gain_steps_down;  // clip guard backoff below the chosen gain
static int s_target_idx = -1, s_idx;
static usb_transfer_t *s_xfers[BULK_XFERS];

// What the web page sees.
static volatile sdr_state_t s_state = SDR_OFF;
static char s_tuner[104];        // the dongle's USB name and its tuner, for the status
static volatile int s_cap_req;  // seconds asked for, picked up by the probe task
static volatile int s_cap_seconds;
static volatile uint64_t s_cap_bytes, s_cap_total, s_cap_dropped, s_cap_file_bytes;
static char s_cap_error[80];

typedef struct {
    uint8_t *buf;
    uint32_t size;
    volatile uint32_t head, tail;   // bytes written / read, ever (wrapping)
    volatile uint64_t dropped;
} ring_t;

// Capture: the transfer callback fills the ring, the probe task drains it to the card.
static ring_t s_cap_ring;
static volatile bool s_cap_on;
// ...and the sentences heard meanwhile (network feed and SDR decoder), kept in
// RAM and written alongside when the recording ends.
static char *s_cap_log;
static size_t s_cap_log_len;
static volatile uint32_t s_cap_log_lines, s_cap_log_dropped;
static int64_t s_cap_t0;
static portMUX_TYPE s_cap_log_mux = portMUX_INITIALIZER_UNLOCKED;

// Decoding: the transfer callback fills the ring, the decoder task drains it
// through the demodulator into the vessel table.
static ring_t s_dec_ring;
static volatile bool s_dec_on, s_dec_reset;
static TaskHandle_t s_dec_task;
static volatile float s_ppm;
static volatile bool s_ppm_changed;
static ais_demod_t *s_demod;
static ais_parser_t s_parser;
static volatile float s_cpu_pct;

// ---------------------------------------------------------------------------
// USB plumbing
// ---------------------------------------------------------------------------

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        s_new_addr = msg->new_dev.address;
        xTaskNotifyGive(s_probe_task);
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        s_gone = true;
        s_streaming = false;
        ESP_LOGW(TAG, "device unplugged");
        xTaskNotifyGive(s_probe_task);
    }
}

// Transfer callbacks run here, so this task must never block on a transfer.
static void event_task(void *arg)
{
    for (;;) usb_host_client_handle_events(s_client, portMAX_DELAY);
}

static void ctrl_cb(usb_transfer_t *t)
{
    xSemaphoreGive(s_ctrl_done);
}

// Synchronous vendor control transfer. Returns bytes moved, or -1.
static int ctrl(bool in, uint16_t value, uint16_t index, uint8_t *data, uint16_t len)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ctrl->data_buffer;
    setup->bmRequestType = (in ? USB_BM_REQUEST_TYPE_DIR_IN : USB_BM_REQUEST_TYPE_DIR_OUT) |
                           USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
    setup->bRequest = 0;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = len;
    if (!in) memcpy(s_ctrl->data_buffer + sizeof(usb_setup_packet_t), data, len);
    // IN data stages are sized in whole max-size packets (64 bytes on endpoint 0).
    s_ctrl->num_bytes = sizeof(usb_setup_packet_t) + (in ? (len + 63) / 64 * 64 : len);
    s_ctrl->device_handle = s_dev;
    s_ctrl->bEndpointAddress = 0;
    s_ctrl->callback = ctrl_cb;
    s_ctrl->timeout_ms = CTRL_TIMEOUT_MS;
    if (usb_host_transfer_submit_control(s_client, s_ctrl) != ESP_OK) return -1;
    if (xSemaphoreTake(s_ctrl_done, pdMS_TO_TICKS(CTRL_TIMEOUT_MS + 200)) != pdTRUE) return -1;
    if (s_ctrl->status != USB_TRANSFER_STATUS_COMPLETED) return -1;
    int got = s_ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
    if (in && data) memcpy(data, s_ctrl->data_buffer + sizeof(usb_setup_packet_t), len);
    return got;
}

// ---------------------------------------------------------------------------
// RTL2832U registers (from librtlsdr)
// ---------------------------------------------------------------------------

static int write_reg(uint8_t block, uint16_t addr, uint16_t val, uint8_t len)
{
    uint8_t d[2] = {len == 1 ? (uint8_t)val : (uint8_t)(val >> 8), (uint8_t)val};
    return ctrl(false, addr, (block << 8) | 0x10, d, len);
}

static uint16_t read_reg(uint8_t block, uint16_t addr, uint8_t len)
{
    uint8_t d[2] = {0};
    ctrl(true, addr, block << 8, d, len);
    return d[0] | (d[1] << 8);
}

static void set_gpio(int gpio, bool on)
{
    uint8_t bit = 1 << gpio;
    write_reg(SYSB, GPD, read_reg(SYSB, GPD, 1) & ~bit, 1);   // output
    write_reg(SYSB, GPOE, read_reg(SYSB, GPOE, 1) | bit, 1);
    uint8_t v = read_reg(SYSB, GPO, 1);
    write_reg(SYSB, GPO, on ? v | bit : v & ~bit, 1);
}

static uint16_t demod_read_reg(uint8_t page, uint16_t addr, uint8_t len)
{
    uint8_t d[2] = {0};
    ctrl(true, (addr << 8) | 0x20, page, d, len);
    return (d[1] << 8) | d[0];
}

static int demod_write_reg(uint8_t page, uint16_t addr, uint16_t val, uint8_t len)
{
    uint8_t d[2] = {len == 1 ? (uint8_t)val : (uint8_t)(val >> 8), (uint8_t)val};
    int r = ctrl(false, (addr << 8) | 0x20, 0x10 | page, d, len);
    demod_read_reg(0x0a, 0x01, 1);
    return r == len ? 0 : -1;
}

static uint8_t i2c_read_reg(uint8_t i2c_addr, uint8_t reg)
{
    uint8_t data = 0;
    ctrl(false, i2c_addr, (IICB << 8) | 0x10, &reg, 1);
    ctrl(true, i2c_addr, IICB << 8, &data, 1);
    return data;
}

static void set_i2c_repeater(bool on)
{
    demod_write_reg(1, 0x01, on ? 0x18 : 0x10, 1);
}

static int tuner_i2c_write(uint8_t addr, const uint8_t *buf, int len)
{
    return ctrl(false, addr, (IICB << 8) | 0x10, (uint8_t *)buf, len);
}

static int tuner_i2c_read(uint8_t addr, uint8_t *buf, int len)
{
    return ctrl(true, addr, IICB << 8, buf, len);
}

// The frequency the RTL2832U mixes down from (the tuner's IF).
static void set_if_freq(uint32_t hz)
{
    int32_t v = -(int32_t)(((uint64_t)hz << 22) / RTL_XTAL_HZ);
    demod_write_reg(1, 0x19, (v >> 16) & 0x3f, 1);
    demod_write_reg(1, 0x1a, (v >> 8) & 0xff, 1);
    demod_write_reg(1, 0x1b, v & 0xff, 1);
}

static void set_fir(void)
{
    uint8_t fir[20];
    for (int i = 0; i < 8; i++) fir[i] = (uint8_t)k_fir_default[i];
    for (int i = 0; i < 8; i += 2) {
        int v0 = k_fir_default[8 + i], v1 = k_fir_default[8 + i + 1];
        fir[8 + i * 3 / 2] = v0 >> 4;
        fir[8 + i * 3 / 2 + 1] = (v0 << 4) | ((v1 >> 8) & 0x0f);
        fir[8 + i * 3 / 2 + 2] = v1;
    }
    for (int i = 0; i < 20; i++) demod_write_reg(1, 0x1c + i, fir[i], 1);
}

static void init_baseband(void)
{
    write_reg(USBB, USB_SYSCTL, 0x09, 1);           // initialise USB
    write_reg(USBB, USB_EPA_MAXPKT, 0x0002, 2);
    write_reg(USBB, USB_EPA_CTL, 0x1002, 2);
    write_reg(SYSB, DEMOD_CTL_1, 0x22, 1);          // power on the demodulator
    write_reg(SYSB, DEMOD_CTL, 0xe8, 1);
    demod_write_reg(1, 0x01, 0x14, 1);              // soft reset
    demod_write_reg(1, 0x01, 0x10, 1);
    demod_write_reg(1, 0x15, 0x00, 1);              // no spectrum inversion / ACI
    demod_write_reg(1, 0x16, 0x0000, 2);
    for (int i = 0; i < 6; i++) demod_write_reg(1, 0x16 + i, 0x00, 1);  // DDC shift, IF frequency
    set_fir();
    demod_write_reg(0, 0x19, 0x05, 1);              // SDR mode, no DAGC
    demod_write_reg(1, 0x93, 0xf0, 1);              // FSM state-holding register
    demod_write_reg(1, 0x94, 0x0f, 1);
    demod_write_reg(1, 0x11, 0x00, 1);              // AGC off
    demod_write_reg(1, 0x04, 0x00, 1);              // RF and IF AGC loop off
    demod_write_reg(0, 0x61, 0x60, 1);              // PID filter off
    demod_write_reg(0, 0x06, 0x80, 1);              // default ADC_I/ADC_Q datapath
    demod_write_reg(1, 0xb1, 0x1b, 1);              // zero-IF, DC and IQ correction
    demod_write_reg(0, 0x0d, 0x83, 1);              // no 4.096 MHz clock output
}

static uint32_t set_sample_rate(uint32_t rate)
{
    uint32_t ratio = (uint32_t)(((uint64_t)RTL_XTAL_HZ << 22) / rate) & 0x0ffffffc;
    uint32_t real_ratio = ratio | ((ratio & 0x08000000) << 1);
    demod_write_reg(1, 0x9f, ratio >> 16, 2);
    demod_write_reg(1, 0xa1, ratio & 0xffff, 2);
    demod_write_reg(1, 0x3f, 0, 1);                 // no sample-rate ppm correction
    demod_write_reg(1, 0x3e, 0, 1);
    demod_write_reg(1, 0x01, 0x14, 1);              // soft reset
    demod_write_reg(1, 0x01, 0x10, 1);
    return (uint32_t)(((uint64_t)RTL_XTAL_HZ << 22) / real_ratio);
}

static void reset_buffer(void)
{
    write_reg(USBB, USB_EPA_CTL, 0x1002, 2);
    write_reg(USBB, USB_EPA_CTL, 0x0000, 2);
}

// ---------------------------------------------------------------------------
// R820T tuning (librtlsdr's open sequence for an R820T, then the AIS settings)
// ---------------------------------------------------------------------------

static bool tune_r820t(uint32_t freq, uint32_t rate)
{
    demod_write_reg(1, 0xb1, 0x1a, 1);  // zero-IF off: the R820T has a low IF
    demod_write_reg(0, 0x08, 0x4d, 1);  // in-phase ADC input only
    demod_write_reg(1, 0x15, 0x01, 1);  // spectrum inversion

    r82xx_t *tuner = &s_r82xx;
    memset(tuner, 0, sizeof(*tuner));
    tuner->i2c_write = tuner_i2c_write;
    tuner->i2c_read = tuner_i2c_read;
    tuner->i2c_addr = R820T_I2C_ADDR;
    tuner->xtal_hz = RTL_XTAL_HZ;

    set_i2c_repeater(true);
    int if_hz = -1;
    const char *fail = NULL;
    if (r82xx_init(tuner) < 0) fail = "init";
    else if ((if_hz = r82xx_set_bandwidth(tuner, rate)) < 0) fail = "bandwidth";
    else if (r82xx_set_freq(tuner, freq) < 0) fail = "frequency (PLL did not lock)";
    set_i2c_repeater(false);
    bool ok = !fail;
    if (fail) ESP_LOGE(TAG, "tuner set-up failed: %s", fail);
    if (if_hz >= 0) set_if_freq(if_hz);
    if (ok) {
        ESP_LOGI(TAG, "tuned to %.3f MHz (IF %.3f MHz, filter cal %u)", freq / 1e6, if_hz / 1e6, tuner->fil_cal_code);
    }
    return ok;
}

static void set_gain(int tenth_db)
{
    set_i2c_repeater(true);
    bool manual = tenth_db != SDR_GAIN_AUTO;
    if (r82xx_set_gain(&s_r82xx, manual, tenth_db) < 0) ESP_LOGE(TAG, "setting the tuner gain failed");
    set_i2c_repeater(false);
    s_gain_now = tenth_db;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

// Finds the best fixed gain: step up through the gains measuring the noise
// floor (median buffer power, so AIS bursts don't count) and clipping, and pick
// the highest gain whose floor stays under FLOOR_MAX_DBFS without clipping.
// The R820T's noise figure keeps improving as its LNA gain rises (at low gain
// its own mixer and IF noise dominate), so more gain helps until headroom for
// strong signals runs out. (Picking the "knee" where the floor starts rising
// chose ~28 dB and heard half as much as 40 dB.) Returns an index into
// k_gains, or -1 if interrupted.
static int gain_sweep(void)
{
    float floor_db[NGAINS], clip[NGAINS];
    int n = 0;
    ESP_LOGI(TAG, "choosing the gain:");
    for (int i = 0; i < NGAINS && !s_gone; i++) {
        s_sweep_step = i + 1;
        set_gain(k_gains[i]);
        vTaskDelay(pdMS_TO_TICKS(SWEEP_SETTLE_MS));
        uint64_t c0 = s_clip_total, b0 = s_byte_total;
        s_meas_n = 0;
        s_meas_on = true;
        vTaskDelay(pdMS_TO_TICKS(SWEEP_MEASURE_MS));
        s_meas_on = false;
        uint64_t bytes = s_byte_total - b0;
        if (!s_meas_n || !bytes) break;  // no samples: the stream has stopped
        float pw[MEAS_MAX];
        int m = s_meas_n;
        memcpy(pw, s_meas_pw, m * sizeof(float));
        qsort(pw, m, sizeof(float), cmp_float);
        floor_db[i] = 10 * log10f(pw[m / 2] / (128.0f * 128.0f) + 1e-12f);
        clip[i] = (float)(s_clip_total - c0) / bytes;
        n = i + 1;
        ESP_LOGI(TAG, "  %4.1f dB: floor %6.1f dBFS, clipped %.4f%%", k_gains[i] / 10.0, floor_db[i], 100 * clip[i]);
        if (clip[i] > CLIP_OK || floor_db[i] > FLOOR_MAX_DBFS) break;  // only gets worse from here
    }
    s_sweep_step = 0;
    if (!n || s_gone) return -1;

    int best = 0;
    for (int i = 0; i < n && clip[i] <= CLIP_OK && floor_db[i] <= FLOOR_MAX_DBFS; i++) best = i;
    ESP_LOGI(TAG, "gain %.1f dB: noise floor %.1f dBFS", k_gains[best] / 10.0, floor_db[best]);
    return best;
}

// Clip guard for the chosen gain: a step down when a strong signal clips, and
// back up once it's been quiet for a while.
static void gain_guard(void)
{
    static int64_t window_start, last_change;
    static uint64_t c0, b0;
    int64_t now = esp_timer_get_time();
    if (now - window_start < GUARD_WINDOW_US) return;
    uint64_t bytes = s_byte_total - b0;
    float clip = bytes ? (float)(s_clip_total - c0) / bytes : 0;
    window_start = now;
    c0 = s_clip_total;
    b0 = s_byte_total;
    if (s_target_idx < 0 || !bytes) return;
    if (clip > GUARD_CLIP && s_idx > 0) {
        s_idx--;
        set_gain(k_gains[s_idx]);
        last_change = now;
        ESP_LOGW(TAG, "clipping (%.2f%%): gain down to %.1f dB", 100 * clip, k_gains[s_idx] / 10.0);
    } else if (clip == 0 && s_idx < s_target_idx && now - last_change > GUARD_RECOVER_US) {
        s_idx++;
        set_gain(k_gains[s_idx]);
        last_change = now;
        ESP_LOGI(TAG, "no clipping for a while: gain back up to %.1f dB", k_gains[s_idx] / 10.0);
    }
    s_gain_steps_down = s_target_idx - s_idx;
}

// Gain and bias-tee from the settings; the ppm goes to the decoder task.
// "Best fixed gain" is left to the sweep (the caller runs it).
static void apply_settings(const settings_t *cfg)
{
    if (cfg->sdr_gain != SDR_GAIN_BEST) {
        set_gain(cfg->sdr_gain);
        s_target_idx = -1;
    }
    bool manual = cfg->sdr_gain >= 0;
    bool bias = cfg->sdr_enabled && cfg->sdr_bias_tee;
    set_gpio(BIAS_TEE_GPIO, bias);
    s_ppm = cfg->sdr_ppm_tenths / 10.0f;
    s_ppm_changed = true;
    if (manual) ESP_LOGI(TAG, "gain %.1f dB, bias-tee %s, %+.1f ppm", cfg->sdr_gain / 10.0, bias ? "on" : "off", s_ppm);
    else ESP_LOGI(TAG, "gain %s, bias-tee %s, %+.1f ppm", cfg->sdr_gain == SDR_GAIN_AUTO ? "tuner AGC" : "best fixed",
                  bias ? "on" : "off", s_ppm);
}

// ---------------------------------------------------------------------------
// Streaming test
// ---------------------------------------------------------------------------

// Single-producer (the transfer callback), single-consumer byte ring. A
// buffer that doesn't fit is dropped whole and counted.
static void ring_push(ring_t *r, const uint8_t *d, uint32_t n)
{
    if (n > r->size - (r->head - r->tail)) {
        r->dropped += n;
        return;
    }
    uint32_t at = r->head % r->size, first = MIN(n, r->size - at);
    memcpy(r->buf + at, d, first);
    memcpy(r->buf, d + first, n - first);
    r->head += n;
}

// The longest contiguous run of unread bytes, up to max.
static uint32_t ring_peek(ring_t *r, const uint8_t **p, uint32_t max)
{
    uint32_t at = r->tail % r->size;
    *p = r->buf + at;
    return MIN(MIN(r->head - r->tail, r->size - at), max);
}

static void bulk_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_bytes += t->actual_num_bytes;
        if (t->actual_num_bytes < t->num_bytes) s_short_xfers++;
        // Sample spread, clipping and power: for the throughput test and the
        // gain control.
        uint32_t lo = s_hist_min, hi = s_hist_max, clipped = 0, sum = 0, pwr = 0;
        int n = t->actual_num_bytes;
        for (int i = 0; i < n; i++) {
            uint8_t v = t->data_buffer[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            clipped += v == 0 || v == 255;
            sum += v;
            int c = v - 128;
            pwr += c * c;
        }
        s_hist_min = lo;
        s_hist_max = hi;
        s_sum += sum;
        s_count += n;
        s_clip_total += clipped;
        s_byte_total += n;
        if (s_meas_on && s_meas_n < MEAS_MAX && n) s_meas_pw[s_meas_n++] = (float)pwr / n;
        if (s_dec_on) {
            ring_push(&s_dec_ring, t->data_buffer, t->actual_num_bytes);
            xTaskNotifyGive(s_dec_task);
        }
        if (s_cap_on) ring_push(&s_cap_ring, t->data_buffer, t->actual_num_bytes);
    } else {
        s_xfer_errors++;
    }
    if (s_streaming && usb_host_transfer_submit(t) != ESP_OK) s_xfer_errors++;
}

// Starts the bulk sample stream. Returns the exact rate, or 0 if it failed.
static uint32_t stream_start(uint32_t rate)
{
    uint32_t real = set_sample_rate(rate);
    reset_buffer();
    s_bytes = s_xfer_errors = s_short_xfers = 0;
    s_hist_min = 255;
    s_hist_max = 0;
    s_sum = s_count = 0;
    s_streaming = true;
    for (int i = 0; i < BULK_XFERS && s_streaming; i++) {
        if (usb_host_transfer_alloc(BULK_XFER_BYTES, 0, &s_xfers[i]) != ESP_OK) {
            ESP_LOGE(TAG, "transfer alloc failed (internal RAM free: %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_xfers[i] = NULL;
            s_streaming = false;
            break;
        }
        s_xfers[i]->device_handle = s_dev;
        s_xfers[i]->bEndpointAddress = BULK_EP;
        s_xfers[i]->callback = bulk_cb;
        s_xfers[i]->num_bytes = BULK_XFER_BYTES;
        s_xfers[i]->timeout_ms = 0;
        if (usb_host_transfer_submit(s_xfers[i]) != ESP_OK) {
            ESP_LOGE(TAG, "bulk submit failed");
            s_streaming = false;
        }
    }
    return s_streaming ? real : 0;
}

static void stream_stop(void)
{
    s_streaming = false;
    vTaskDelay(pdMS_TO_TICKS(300)); // let in-flight transfers finish
    // Cancel any that won't (a stalled stream), so they can be freed.
    if (usb_host_endpoint_halt(s_dev, BULK_EP) == ESP_OK) {
        usb_host_endpoint_flush(s_dev, BULK_EP);
        usb_host_endpoint_clear(s_dev, BULK_EP);
        vTaskDelay(pdMS_TO_TICKS(50)); // the cancelled callbacks
    }
    for (int i = 0; i < BULK_XFERS; i++) {
        if (s_xfers[i]) usb_host_transfer_free(s_xfers[i]);
        s_xfers[i] = NULL;
    }
}

// Streams for run_ms and reports the throughput.
static void stream_test(uint32_t rate, int run_ms)
{
    uint32_t real = stream_start(rate);
    if (!real) real = rate;

    int64_t t0 = esp_timer_get_time();
    uint64_t last = 0;
    for (int s = 1; s * 2000 <= run_ms && s_streaming && !s_gone; s++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint64_t b = s_bytes;
        ESP_LOGI(TAG, "  %u S/s: %.2f MB/s this period (need %.2f), errors %lu, short %lu", (unsigned)real,
                 (b - last) / 2.0 / 1e6, real * 2 / 1e6, (unsigned long)s_xfer_errors, (unsigned long)s_short_xfers);
        last = b;
    }
    // Rate over the streaming time only, not the drain below.
    uint64_t bytes = s_bytes;
    double secs = (esp_timer_get_time() - t0) / 1e6;
    stream_stop();
    double got = bytes / secs, need = real * 2.0;
    ESP_LOGI(TAG, "RESULT %u S/s: %.2f MB/s of %.2f MB/s needed (%.0f%%), %lu errors, samples %lu..%lu mean %.1f",
             (unsigned)real, got / 1e6, need / 1e6, 100 * got / need, (unsigned long)s_xfer_errors,
             (unsigned long)s_hist_min, (unsigned long)s_hist_max, s_count ? (double)s_sum / s_count : 0.0);
}

// ---------------------------------------------------------------------------
// I/Q capture to the microSD card, in rtl_sdr's format (unsigned 8-bit I/Q)
// ---------------------------------------------------------------------------

static void cap_fail(const char *msg)
{
    if (!s_cap_error[0]) snprintf(s_cap_error, sizeof(s_cap_error), "%s", msg);
}

void sdr_capture_log(char source, const char *sentence)
{
    if (!s_cap_on || !s_cap_log) return;
    char line[160];
    double t = (esp_timer_get_time() - s_cap_t0) / 1e6;
    int n = snprintf(line, sizeof(line), "%.3f %c %s\n", t, source, sentence);
    if (n >= (int)sizeof(line)) n = sizeof(line) - 1, line[n - 1] = '\n';
    // Trim a CR the network line may carry.
    if (n >= 2 && line[n - 2] == '\r') line[n - 2] = '\n', n--;
    taskENTER_CRITICAL(&s_cap_log_mux);
    if (!s_cap_log) {
        // The recording just ended.
    } else if (s_cap_log_len + n <= CAP_LOG) {
        memcpy(s_cap_log + s_cap_log_len, line, n);
        s_cap_log_len += n;
        s_cap_log_lines++;
    } else {
        s_cap_log_dropped++;
    }
    taskEXIT_CRITICAL(&s_cap_log_mux);
}

static void run_capture(int seconds)
{
    uint64_t total = (uint64_t)seconds * AIS_RATE * 2;
    s_cap_seconds = seconds;
    s_cap_total = total;
    s_cap_bytes = s_cap_dropped = s_cap_file_bytes = 0;
    s_cap_error[0] = '\0';
    ESP_LOGI(TAG, "capture: %d s (%.1f MB) to %s", seconds, total / 1e6, SDR_CAPTURE_PATH);

    // The SD driver needs DMA-capable memory, so the ring (PSRAM) is copied
    // through a small internal buffer.
    s_cap_ring = (ring_t){.buf = heap_caps_malloc(CAP_RING, MALLOC_CAP_SPIRAM), .size = CAP_RING};
    uint32_t chunk;
    uint8_t *bounce = NULL;
    for (chunk = CAP_CHUNK; chunk >= CAP_CHUNK_MIN; chunk /= 2) {
        bounce = heap_caps_aligned_alloc(64, chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (bounce) break;
    }
    if (bounce && chunk < CAP_CHUNK) ESP_LOGW(TAG, "capture: internal RAM is short, writing in %lu KB pieces", (unsigned long)(chunk / 1024));
    if (!bounce) {
        ESP_LOGE(TAG, "capture: no %d KB of internal RAM (largest free block %u)", CAP_CHUNK_MIN / 1024,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    }
    s_cap_log = heap_caps_malloc(CAP_LOG, MALLOC_CAP_SPIRAM);
    s_cap_log_len = 0;
    s_cap_log_lines = s_cap_log_dropped = 0;
    FILE *f = NULL;
    if (!bounce) {
        cap_fail("the display is short of internal memory; restart it and try again");
    } else if (!s_cap_ring.buf || !s_cap_log) {
        cap_fail("out of memory");
    } else {
        mkdir(CAP_DIR, 0777);
        f = fopen(SDR_CAPTURE_PATH, "wb");
        if (!f) cap_fail("could not create the file on the microSD card");
        else setvbuf(f, NULL, _IONBF, 0);
    }
    if (f) {
        s_cap_t0 = esp_timer_get_time();
        s_cap_on = true;  // the stream is already running, for the decoder
    }

    int64_t last_data = esp_timer_get_time();
    while (!s_cap_error[0] && s_cap_bytes < total) {
        if (s_gone) {
            cap_fail("the dongle was unplugged");
            break;
        }
        const uint8_t *p;
        uint32_t n = ring_peek(&s_cap_ring, &p, chunk);
        if (!n) {
            if (esp_timer_get_time() - last_data > 2000000) cap_fail("no samples from the dongle");
            else vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last_data = esp_timer_get_time();
        n = MIN((uint64_t)n, total - s_cap_bytes);
        memcpy(bounce, p, n);
        s_cap_ring.tail += n;
        if (fwrite(bounce, 1, n, f) != n) {
            cap_fail("writing to the microSD card failed (is it full?)");
            break;
        }
        s_cap_bytes += n;
        s_cap_dropped = s_cap_ring.dropped;
    }
    s_cap_on = false;
    vTaskDelay(pdMS_TO_TICKS(50));  // let a callback that's mid-push finish
    s_cap_dropped = s_cap_ring.dropped;
    if (f && fclose(f) != 0) cap_fail("closing the file on the microSD card failed");
    free(s_cap_ring.buf);
    s_cap_ring.buf = NULL;
    free(bounce);

    // The sentences, timed from the start of the I/Q file.
    if (!s_cap_error[0] && s_cap_log) {
        FILE *lf = fopen(SDR_CAPTURE_NMEA_PATH, "w");
        if (!lf || fwrite(s_cap_log, 1, s_cap_log_len, lf) != s_cap_log_len) cap_fail("writing the NMEA log failed");
        if (lf) fclose(lf);
    }
    taskENTER_CRITICAL(&s_cap_log_mux);
    char *log = s_cap_log;
    s_cap_log = NULL;
    taskEXIT_CRITICAL(&s_cap_log_mux);
    free(log);

    if (s_cap_error[0]) {
        ESP_LOGE(TAG, "capture failed after %.1f MB: %s", s_cap_bytes / 1e6, s_cap_error);
        remove(SDR_CAPTURE_PATH);
        remove(SDR_CAPTURE_NMEA_PATH);
    } else {
        s_cap_file_bytes = s_cap_bytes;
        ESP_LOGI(TAG, "capture done: %.1f MB, %.1f MB dropped; %lu sentences logged (%lu didn't fit)", s_cap_bytes / 1e6,
                 s_cap_dropped / 1e6, (unsigned long)s_cap_log_lines, (unsigned long)s_cap_log_dropped);
    }
}

// A USB string descriptor (UTF-16) as ASCII, trimmed; "" if there isn't one.
static void usb_string(const usb_str_desc_t *d, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; d && i < (d->bLength - 2) / 2 && n + 1 < cap; i++) {
        uint16_t c = d->wData[i];
        out[n++] = c >= 0x20 && c < 0x7f ? (char)c : '?';
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

static void probe_device(uint8_t addr)
{
    if (usb_host_device_open(s_client, addr, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "could not open USB device %u", addr);
        return;
    }
    usb_device_info_t info;
    const usb_device_desc_t *dd;
    usb_host_device_info(s_dev, &info);
    usb_host_get_device_descriptor(s_dev, &dd);
    static const char *const speeds[] = {"Low", "Full", "High"};
    ESP_LOGI(TAG, "USB device %04x:%04x, %s speed, max packet %u", dd->idVendor, dd->idProduct,
             info.speed < 3 ? speeds[info.speed] : "?", dd->bMaxPacketSize0);
    // What the dongle calls itself: "Realtek RTL2838UHIDIR", "RTLSDRBlog Blog V4", ...
    char maker[32], product[32], name[64];
    usb_string(info.str_desc_manufacturer, maker, sizeof(maker));
    usb_string(info.str_desc_product, product, sizeof(product));
    if (!maker[0] || strncmp(product, maker, strlen(maker)) == 0) snprintf(name, sizeof(name), "%s", product);
    else if (!product[0]) snprintf(name, sizeof(name), "%s", maker);
    else snprintf(name, sizeof(name), "%s %s", maker, product);
    if (!name[0]) snprintf(name, sizeof(name), "RTL2832U dongle");
    ESP_LOGI(TAG, "USB name: %s", name);

    if (dd->idVendor != RTL_VID || (dd->idProduct != 0x2838 && dd->idProduct != 0x2832)) {
        ESP_LOGW(TAG, "not an RTL2832U dongle; ignoring it");
        goto out;
    }
    s_state = SDR_TESTING;
    if (info.speed != USB_SPEED_HIGH) ESP_LOGW(TAG, "dongle is not on High Speed USB: high sample rates won't fit");
    if (usb_host_interface_claim(s_client, s_dev, 0, 0) != ESP_OK) {
        ESP_LOGE(TAG, "could not claim interface 0");
        goto out;
    }

    // librtlsdr's open sequence: a dummy write, then baseband set-up.
    if (write_reg(USBB, USB_SYSCTL, 0x09, 1) < 0) ESP_LOGW(TAG, "first register write failed");
    init_baseband();

    // Probe the V3's tuner first: reading an absent tuner's address makes the
    // dongle STALL the request, which the USB host logs as an error.
    set_i2c_repeater(true);
    uint8_t r820 = i2c_read_reg(R820T_I2C_ADDR, 0x00), r828 = 0;
    if (r820 != R82XX_CHECK_VAL) r828 = i2c_read_reg(R828D_I2C_ADDR, 0x00);
    set_i2c_repeater(false);
    if (r820 == R82XX_CHECK_VAL) ESP_LOGI(TAG, "tuner: Rafael Micro R820T/R820T2");
    else if (r828 == R82XX_CHECK_VAL) ESP_LOGI(TAG, "tuner: Rafael Micro R828D");
    else ESP_LOGW(TAG, "tuner: not recognised (R820T check read 0x%02x, R828D 0x%02x)", r820, r828);

    ESP_LOGI(TAG, "internal RAM free %u, PSRAM free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // With an R820T, decode AIS; other tuners just get the throughput test.
    if (r820 == R82XX_CHECK_VAL && tune_r820t(AIS_CENTER_HZ, AIS_RATE)) {
        snprintf(s_tuner, sizeof(s_tuner), "%s, R820T tuner", name);
        // Stream continuously into the decoder (captures tap the same stream),
        // following the settings as they change.
        bool streaming = false, need_sweep = false;
        int64_t retry_at = 0, last_data = 0;
        uint64_t last_bytes = 0;
        settings_t old = {0};
        s_target_idx = -1;
        for (bool first = true; !s_gone; first = false) {
            if (first || (xEventGroupClearBits(s_events, SETTINGS_EV_SDR) & SETTINGS_EV_SDR)) {
                settings_t cfg;
                settings_get(&cfg);
                apply_settings(&cfg);
                // Choose the gain again whenever something that changes the answer does.
                if (cfg.sdr_gain == SDR_GAIN_BEST &&
                    (first || old.sdr_gain != SDR_GAIN_BEST || old.sdr_bias_tee != cfg.sdr_bias_tee ||
                     (!old.sdr_enabled && cfg.sdr_enabled))) {
                    need_sweep = true;
                }
                old = cfg;
                if (!cfg.sdr_enabled && streaming) {
                    s_dec_on = false;
                    stream_stop();
                    streaming = false;
                    ESP_LOGI(TAG, "turned off in the settings");
                }
            }
            // A stream that stops delivering samples (its transfers all failed)
            // isn't receiving: drop it and start it again below.
            int64_t now = esp_timer_get_time();
            if (streaming && s_bytes != last_bytes) {
                last_bytes = s_bytes;
                last_data = now;
            } else if (streaming && now - last_data > STALL_US) {
                ESP_LOGE(TAG, "no samples for %d s (%lu transfer errors); restarting the stream",
                         (int)(STALL_US / 1000000), (unsigned long)s_xfer_errors);
                s_dec_on = false;
                stream_stop();
                streaming = false;
            }
            // Start the stream, retrying while it fails (say internal RAM is
            // short for the transfer buffers).
            if (old.sdr_enabled && !streaming && now >= retry_at) {
                s_dec_reset = true;
                s_dec_on = true;
                streaming = stream_start(AIS_RATE) != 0;
                if (streaming) {
                    ESP_LOGI(TAG, "receiving: decoding AIS on 161.975 and 162.025 MHz");
                    last_bytes = 0;
                    last_data = esp_timer_get_time();
                } else {
                    s_dec_on = false;
                    stream_stop(); // frees the transfers it did get
                    ESP_LOGE(TAG, "could not start the sample stream; trying again in %d s",
                             (int)(RETRY_US / 1000000));
                    retry_at = esp_timer_get_time() + RETRY_US;
                }
            }
            // "Receiving" only while samples are flowing.
            s_state = !old.sdr_enabled ? SDR_DISABLED : streaming ? SDR_READY : SDR_TESTING;
            if (need_sweep && streaming) {
                need_sweep = false;
                int best = gain_sweep();
                if (best >= 0) {
                    s_target_idx = s_idx = best;
                    s_gain_steps_down = 0;
                    set_gain(k_gains[best]);
                }
            }
            if (streaming) gain_guard();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            int seconds = s_cap_req;
            if (seconds) {
                if (streaming && !s_gone) {
                    s_state = SDR_CAPTURING;
                    run_capture(seconds);
                    s_state = SDR_READY;
                }
                s_cap_req = 0;
            }
        }
        s_dec_on = false;
        if (streaming) stream_stop();
    } else {
        snprintf(s_tuner, sizeof(s_tuner), "%s, %s", name, r828 == R82XX_CHECK_VAL ? "R828D tuner (not supported yet)" : "unknown tuner");
        stream_test(AIS_RATE, RUN_MS);
        ESP_LOGI(TAG, "test finished; unplug and replug the dongle to run it again");
    }
    usb_host_interface_release(s_client, s_dev, 0);
out:
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
    s_tuner[0] = '\0';
    s_state = SDR_NONE;
}

// ---------------------------------------------------------------------------
// Decoder task
// ---------------------------------------------------------------------------

static void on_sentence(void *ctx, const char *sentence)
{
    sdr_capture_log('S', sentence);
    ais_fwd_sentence(sentence);
    ais_report_t r;
    ais_result_t res = ais_parse_line(&s_parser, sentence, (uint32_t)(esp_timer_get_time() / 1000), &r);
    if (res == AIS_RESULT_REPORT || res == AIS_RESULT_UNSUPPORTED) rx_stats_count(RX_SDR, rx_sentence_channel(sentence));
    if (res == AIS_RESULT_REPORT) vessel_db_update(&r, RX_SDR);
}

static void decode_task(void *arg)
{
    int64_t period_start = esp_timer_get_time(), busy = 0, last_expire = 0;
    for (;;) {
        if (s_dec_reset) {
            s_dec_reset = false;
            ais_demod_init(s_demod, on_sentence, NULL);
            ais_parser_init(&s_parser);
            s_ppm_changed = true;
            s_dec_ring.tail = s_dec_ring.head;
            s_dec_ring.dropped = 0;
        }
        if (s_ppm_changed) {
            s_ppm_changed = false;
            ais_demod_set_ppm(s_demod, s_ppm);
        }
        const uint8_t *p;
        uint32_t n = ring_peek(&s_dec_ring, &p, DEC_CHUNK);
        if (n) {
            int64_t t0 = esp_timer_get_time();
            ais_demod_feed(s_demod, p, n);
            busy += esp_timer_get_time() - t0;
            s_dec_ring.tail += n;
        } else {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        }

        int64_t now = esp_timer_get_time();
        if (now - period_start >= CPU_PERIOD_US) {
            // Time spent in the demodulator, as a share of one core.
            s_cpu_pct = s_dec_on ? 100.0f * busy / (now - period_start) : 0;
            period_start = now;
            busy = 0;
        }
        if (now - last_expire > 1000000) {
            vessel_db_expire();  // the network client does this too, but may not be running
            last_expire = now;
        }
    }
}

static void probe_task(void *arg)
{
    s_state = SDR_NONE;
    struct stat st;
    if (stat(SDR_CAPTURE_PATH, &st) == 0) s_cap_file_bytes = st.st_size;  // from before a restart
    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true));
    usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {.client_event_callback = client_event_cb},
    };
    ESP_ERROR_CHECK(usb_host_client_register(&cfg, &s_client));
    usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 64, 0, &s_ctrl);
    s_ctrl_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(event_task, "sdr_usb_ev", 4096, NULL, 6, NULL, 1);

    // A dongle plugged in at boot was enumerated before this client registered,
    // so no NEW_DEV event comes for it: look for it directly.
    vTaskDelay(pdMS_TO_TICKS(1500));
    uint8_t addrs[8];
    int n = 0;
    usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n);
    if (n == 0) ESP_LOGI(TAG, "USB host ready: plug the RTL-SDR into the USB-A port");
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "found a USB device already connected (address %u)", addrs[i]);
        s_gone = false;
        probe_device(addrs[i]);
    }
    ulTaskNotifyTake(pdTRUE, 0); // drop any NEW_DEV notification for a device just probed
    s_new_addr = 0;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_new_addr) {
            uint8_t addr = s_new_addr;
            s_new_addr = 0;
            s_gone = false;
            probe_device(addr);
        }
    }
}

void sdr_probe_start(void)
{
    // The demodulator's state is touched for every sample: keep it in internal RAM.
    s_demod = heap_caps_malloc(ais_demod_size(), MALLOC_CAP_INTERNAL);
    s_dec_ring = (ring_t){.buf = heap_caps_malloc(DEC_RING, MALLOC_CAP_SPIRAM), .size = DEC_RING};
    if (!s_demod || !s_dec_ring.buf) {
        ESP_LOGE(TAG, "out of memory for the AIS decoder");
        return;
    }
    ais_demod_init(s_demod, on_sentence, NULL);
    s_events = settings_subscribe();
    xTaskCreatePinnedToCore(decode_task, "sdr_decode", 7168, NULL, 3, &s_dec_task, 1);
    xTaskCreatePinnedToCore(probe_task, "sdr_probe", 8192, NULL, 4, &s_probe_task, 1);
}

const char *sdr_state_str(sdr_state_t st)
{
    static const char *const names[] = {"off", "no dongle", "starting", "receiving", "recording", "turned off"};
    return st < sizeof(names) / sizeof(names[0]) ? names[st] : "?";
}

void sdr_get_status(sdr_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    snprintf(out->tuner, sizeof(out->tuner), "%s", s_tuner);
    out->center_hz = AIS_CENTER_HZ;
    out->rate = AIS_RATE;
    out->cap_seconds = s_cap_seconds;
    out->cap_bytes = s_cap_bytes;
    out->cap_total = s_cap_total;
    out->cap_dropped = s_cap_dropped;
    out->cap_file_bytes = s_cap_file_bytes;
    out->cap_log_lines = s_cap_log_lines;
    snprintf(out->cap_error, sizeof(out->cap_error), "%s", s_cap_error);
    out->cpu_pct = s_cpu_pct;
    out->gain = s_gain_now;
    out->gain_best = s_target_idx >= 0;
    out->gain_steps_down = s_gain_steps_down;
    out->sweep_step = s_sweep_step;
    out->sweep_steps = NGAINS;
    out->dropped_bytes = s_dec_ring.dropped;
    out->parser = s_parser.stats; // counters only: a torn copy is off by one at most
    if (s_demod) ais_demod_get_stats(s_demod, &out->demod);
}

bool sdr_capture_start(int seconds, char *err, size_t errlen)
{
    if (seconds < 1 || seconds > SDR_CAPTURE_MAX_S) {
        snprintf(err, errlen, "length must be 1-%d seconds", SDR_CAPTURE_MAX_S);
        return false;
    }
    if (s_state != SDR_READY || s_cap_req) {
        snprintf(err, errlen, "%s", s_state == SDR_CAPTURING  ? "a capture is already running"
                                    : s_state == SDR_DISABLED ? "the SDR is turned off in the settings"
                                    : s_state == SDR_TESTING ? "the dongle is still starting; try again in a moment"
                                                              : "no RTL-SDR dongle ready");
        return false;
    }
    if (!map_store_card_ok()) {
        snprintf(err, errlen, "no microSD card in the display");
        return false;
    }
    uint64_t total, free_bytes;
    map_store_usage(&total, &free_bytes);
    uint64_t need = (uint64_t)seconds * AIS_RATE * 2 + (1 << 20);
    if (need > free_bytes + s_cap_file_bytes) {
        snprintf(err, errlen, "not enough free space on the microSD card");
        return false;
    }
    s_cap_req = seconds;
    s_cap_error[0] = '\0';
    xTaskNotifyGive(s_probe_task);
    return true;
}
