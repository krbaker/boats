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

#include "map_store.h"
#include "r82xx.h"

static const char *TAG = "sdr_probe";

#define RTL_VID             0x0bda
#define RTL_XTAL_HZ         28800000
#define BULK_EP             0x81
#define BULK_XFER_BYTES     (16 * 1024)
#define BULK_XFERS          4
#define CTRL_TIMEOUT_MS     300
#define RUN_MS              10000
#define AIS_RUN_MS          20000
#define AIS_CENTER_HZ       162000000   // channel A at -25 kHz, B at +25 kHz
#define AIS_RATE            288000
#define AIS_GAIN_TENTH_DB   402
#define CAP_RING            (4 * 1024 * 1024)  // PSRAM: ~7 s of slack if the card stalls
#define CAP_CHUNK           (32 * 1024)        // written through an internal-RAM bounce buffer
#define CAP_DIR             MAP_ROOT "/sdr"

// librtlsdr register blocks
enum { DEMODB = 0, USBB = 1, SYSB = 2, IICB = 6 };
#define USB_SYSCTL          0x2000
#define USB_EPA_CTL         0x2148
#define USB_EPA_MAXPKT      0x2158
#define DEMOD_CTL           0x3000
#define DEMOD_CTL_1         0x300b
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
static SemaphoreHandle_t s_ctrl_done;
static usb_transfer_t *s_ctrl;

// Streaming statistics, updated from the transfer callback.
static volatile uint64_t s_bytes;
static volatile uint32_t s_xfer_errors, s_short_xfers;
static volatile bool s_streaming;
static volatile uint32_t s_hist_min = 255, s_hist_max;
static volatile uint64_t s_sum, s_count;
static usb_transfer_t *s_xfers[BULK_XFERS];

// What the web page sees.
static volatile sdr_state_t s_state = SDR_OFF;
static char s_tuner[40];
static volatile int s_cap_req;  // seconds asked for, picked up by the probe task
static volatile int s_cap_seconds;
static volatile uint64_t s_cap_bytes, s_cap_total, s_cap_dropped, s_cap_file_bytes;
static char s_cap_error[80];

// Capture ring: the transfer callback fills it, the probe task drains it to the card.
static uint8_t *s_ring;
static volatile uint32_t s_ring_head, s_ring_tail;
static volatile bool s_cap_on;

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

static bool tune_r820t(uint32_t freq, uint32_t rate, int gain_tenth_db)
{
    demod_write_reg(1, 0xb1, 0x1a, 1);  // zero-IF off: the R820T has a low IF
    demod_write_reg(0, 0x08, 0x4d, 1);  // in-phase ADC input only
    demod_write_reg(1, 0x15, 0x01, 1);  // spectrum inversion

    static r82xx_t tuner;
    memset(&tuner, 0, sizeof(tuner));
    tuner.i2c_write = tuner_i2c_write;
    tuner.i2c_read = tuner_i2c_read;
    tuner.i2c_addr = R820T_I2C_ADDR;
    tuner.xtal_hz = RTL_XTAL_HZ;

    set_i2c_repeater(true);
    int if_hz = -1;
    const char *fail = NULL;
    if (r82xx_init(&tuner) < 0) fail = "init";
    else if ((if_hz = r82xx_set_bandwidth(&tuner, rate)) < 0) fail = "bandwidth";
    else if (r82xx_set_freq(&tuner, freq) < 0) fail = "frequency (PLL did not lock)";
    else if (r82xx_set_gain(&tuner, true, gain_tenth_db) < 0) fail = "gain";
    set_i2c_repeater(false);
    bool ok = !fail;
    if (fail) ESP_LOGE(TAG, "tuner set-up failed: %s", fail);
    if (if_hz >= 0) set_if_freq(if_hz);
    if (ok) {
        ESP_LOGI(TAG, "tuned to %.3f MHz (IF %.3f MHz, filter cal %u), gain %.1f dB", freq / 1e6, if_hz / 1e6,
                 tuner.fil_cal_code, gain_tenth_db / 10.0);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// AIS channel power: no demodulator yet, just the power in a 12 kHz slice at
// each channel and at a quiet reference frequency, in 2 ms windows. An AIS
// packet is a 26.7 ms burst, so it shows as a run of windows well above the
// channel's noise floor.
// ---------------------------------------------------------------------------

#define LUT_N     288   // one cycle of 1 kHz at 288 kS/s
#define BOX       24    // boxcar low-pass: first null at 12 kHz
#define WIN_BOXES 24    // boxcar outputs per 2 ms window
#define WIN_RING  1024  // windows kept (2 s worth, plus slack)
#define BURST_DB  10.0f // a window this far above the floor counts as signal
#define BURST_MIN 4     // windows in a row (8 ms) to count as a burst

enum { CH_A, CH_B, CH_REF, NCH };
static const int k_ch_khz[NCH] = {-25, 25, 75};
static const char *const k_ch_name[NCH] = {"A 161.975", "B 162.025", "ref +75k"};

static bool s_ais_on;
static float s_cos[LUT_N], s_sin[LUT_N];
static uint16_t s_idx[NCH], s_step[NCH];
static float s_acc_i[NCH], s_acc_q[NCH], s_win_sum[NCH];
static int s_box_n, s_win_n;
static float s_win[NCH][WIN_RING];
static volatile uint32_t s_wcount;
static volatile uint32_t s_clipped;

static void ais_reset(void)
{
    for (int i = 0; i < LUT_N; i++) {
        s_cos[i] = cosf(2 * (float)M_PI * i / LUT_N);
        s_sin[i] = sinf(2 * (float)M_PI * i / LUT_N);
    }
    for (int c = 0; c < NCH; c++) {
        s_step[c] = ((-k_ch_khz[c]) % LUT_N + LUT_N) % LUT_N;  // shifts the channel down to 0 Hz
        s_idx[c] = 0;
        s_acc_i[c] = s_acc_q[c] = s_win_sum[c] = 0;
    }
    s_box_n = s_win_n = 0;
    s_wcount = s_clipped = 0;
}

static void ais_measure(const uint8_t *d, int n)
{
    const float scale = 1.0f / ((float)BOX * BOX * 127.5f * 127.5f);
    for (int i = 0; i + 1 < n; i += 2) {
        if (d[i] == 0 || d[i] == 255 || d[i + 1] == 0 || d[i + 1] == 255) s_clipped++;
        float x = d[i] - 127.5f, y = d[i + 1] - 127.5f;
        for (int c = 0; c < NCH; c++) {
            float co = s_cos[s_idx[c]], si = s_sin[s_idx[c]];
            s_acc_i[c] += x * co - y * si;
            s_acc_q[c] += x * si + y * co;
            s_idx[c] += s_step[c];
            if (s_idx[c] >= LUT_N) s_idx[c] -= LUT_N;
        }
        if (++s_box_n < BOX) continue;
        s_box_n = 0;
        for (int c = 0; c < NCH; c++) {
            s_win_sum[c] += (s_acc_i[c] * s_acc_i[c] + s_acc_q[c] * s_acc_q[c]) * scale;
            s_acc_i[c] = s_acc_q[c] = 0;
        }
        if (++s_win_n < WIN_BOXES) continue;
        s_win_n = 0;
        uint32_t w = s_wcount % WIN_RING;
        for (int c = 0; c < NCH; c++) {
            s_win[c][w] = s_win_sum[c] / WIN_BOXES;
            s_win_sum[c] = 0;
        }
        s_wcount++;
    }
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float db(float p)
{
    return 10 * log10f(p > 1e-12f ? p : 1e-12f);
}

// Logs each channel's floor, peak and bursts for the windows since last time.
static void ais_report(uint32_t *from, uint32_t total_bursts[NCH])
{
    static float tmp[WIN_RING], sorted[WIN_RING];
    uint32_t end = s_wcount, start = *from;
    if (end - start > WIN_RING) start = end - WIN_RING;
    *from = end;
    int n = end - start;
    if (n < 10) return;
    for (int c = 0; c < NCH; c++) {
        for (int j = 0; j < n; j++) tmp[j] = s_win[c][(start + j) % WIN_RING];
        // Floor from the median of a sorted copy; bursts from the time order.
        memcpy(sorted, tmp, n * sizeof(float));
        qsort(sorted, n, sizeof(float), cmp_float);
        float floor_p = sorted[n / 2], peak_p = sorted[n - 1];
        float thresh = floor_p * powf(10, BURST_DB / 10);
        int bursts = 0, run = 0;
        for (int j = 0; j < n; j++) {
            if (tmp[j] > thresh) {
                if (++run == BURST_MIN) bursts++;
            } else {
                run = 0;
            }
        }
        total_bursts[c] += bursts;
        ESP_LOGI(TAG, "    %-9s floor %6.1f dBFS, peak %+5.1f dB, bursts %d (total %lu)", k_ch_name[c], db(floor_p),
                 db(peak_p) - db(floor_p), bursts, (unsigned long)total_bursts[c]);
    }
}

// ---------------------------------------------------------------------------
// Streaming test
// ---------------------------------------------------------------------------

static void cap_push(const uint8_t *d, uint32_t n)
{
    if (n > CAP_RING - (s_ring_head - s_ring_tail)) {
        s_cap_dropped += n;  // the card fell more than CAP_RING behind
        return;
    }
    uint32_t at = s_ring_head % CAP_RING, first = MIN(n, CAP_RING - at);
    memcpy(s_ring + at, d, first);
    memcpy(s_ring, d + first, n - first);
    s_ring_head += n;
}

static void bulk_cb(usb_transfer_t *t)
{
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_bytes += t->actual_num_bytes;
        if (t->actual_num_bytes < t->num_bytes) s_short_xfers++;
        // Sample spread, from a slice of each buffer: shows whether the ADC is live.
        uint32_t lo = s_hist_min, hi = s_hist_max;
        uint64_t sum = 0;
        int n = t->actual_num_bytes < 1024 ? t->actual_num_bytes : 1024;
        for (int i = 0; i < n; i++) {
            uint8_t v = t->data_buffer[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            sum += v;
        }
        s_hist_min = lo;
        s_hist_max = hi;
        s_sum += sum;
        s_count += n;
        if (s_ais_on) ais_measure(t->data_buffer, t->actual_num_bytes);
        if (s_cap_on) cap_push(t->data_buffer, t->actual_num_bytes);
    } else {
        s_xfer_errors++;
    }
    if (s_streaming && usb_host_transfer_submit(t) != ESP_OK) s_xfer_errors++;
}

// Streams for run_ms. With ais set, also measures the AIS channels (the tuner
// must already be on AIS_CENTER_HZ and rate must be AIS_RATE).
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
    for (int i = 0; i < BULK_XFERS; i++) {
        if (s_xfers[i]) usb_host_transfer_free(s_xfers[i]);
        s_xfers[i] = NULL;
    }
}

static void stream_test(uint32_t rate, int run_ms, bool ais)
{
    if (ais) ais_reset();
    s_ais_on = ais;
    uint32_t ais_from = 0, ais_bursts[NCH] = {0};
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
        if (ais) {
            ESP_LOGI(TAG, "    ADC %lu..%lu, clipped %.2f%%", (unsigned long)s_hist_min, (unsigned long)s_hist_max,
                     100.0 * s_clipped / ((double)real * 2 * s));
            ais_report(&ais_from, ais_bursts);
        }
    }
    // Rate over the streaming time only, not the drain below.
    uint64_t bytes = s_bytes;
    double secs = (esp_timer_get_time() - t0) / 1e6;
    s_ais_on = false;
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
    s_ring = heap_caps_malloc(CAP_RING, MALLOC_CAP_SPIRAM);
    uint8_t *bounce = heap_caps_aligned_alloc(64, CAP_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    FILE *f = NULL;
    if (!s_ring || !bounce) {
        cap_fail("out of memory");
    } else {
        mkdir(CAP_DIR, 0777);
        f = fopen(SDR_CAPTURE_PATH, "wb");
        if (!f) cap_fail("could not create the file on the microSD card");
        else setvbuf(f, NULL, _IONBF, 0);
    }
    if (f) {
        s_ring_head = s_ring_tail = 0;
        s_cap_on = true;
        if (!stream_start(AIS_RATE)) cap_fail("could not start the sample stream");
    }

    int64_t last_data = esp_timer_get_time();
    while (!s_cap_error[0] && s_cap_bytes < total) {
        if (s_gone) {
            cap_fail("the dongle was unplugged");
            break;
        }
        uint32_t avail = s_ring_head - s_ring_tail;
        if (!avail) {
            if (esp_timer_get_time() - last_data > 2000000) cap_fail("no samples from the dongle");
            else vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        last_data = esp_timer_get_time();
        uint32_t at = s_ring_tail % CAP_RING;
        uint32_t n = MIN(MIN(avail, CAP_RING - at), CAP_CHUNK);
        n = MIN((uint64_t)n, total - s_cap_bytes);
        memcpy(bounce, s_ring + at, n);
        s_ring_tail += n;
        if (fwrite(bounce, 1, n, f) != n) {
            cap_fail("writing to the microSD card failed (is it full?)");
            break;
        }
        s_cap_bytes += n;
    }
    s_cap_on = false;
    stream_stop();
    if (f && fclose(f) != 0) cap_fail("closing the file on the microSD card failed");
    free(s_ring);
    s_ring = NULL;
    free(bounce);

    if (s_cap_error[0]) {
        ESP_LOGE(TAG, "capture failed after %.1f MB: %s", s_cap_bytes / 1e6, s_cap_error);
        remove(SDR_CAPTURE_PATH);
    } else {
        s_cap_file_bytes = s_cap_bytes;
        ESP_LOGI(TAG, "capture done: %.1f MB, %.1f MB dropped", s_cap_bytes / 1e6, s_cap_dropped / 1e6);
    }
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
    if (info.str_desc_manufacturer) usb_print_string_descriptor(info.str_desc_manufacturer);
    if (info.str_desc_product) usb_print_string_descriptor(info.str_desc_product);

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
    if (r820 == R82XX_CHECK_VAL) ESP_LOGI(TAG, "tuner: Rafael Micro R820T/R820T2 (RTL-SDR Blog V3)");
    else if (r828 == R82XX_CHECK_VAL) ESP_LOGI(TAG, "tuner: Rafael Micro R828D (RTL-SDR Blog V4)");
    else ESP_LOGW(TAG, "tuner: not recognised (R820T check read 0x%02x, R828D 0x%02x)", r820, r828);

    ESP_LOGI(TAG, "internal RAM free %u, PSRAM free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // Throughput is settled (step 1), so with an R820T, listen on the AIS
    // channels instead; other tuners just get the throughput test.
    if (r820 == R82XX_CHECK_VAL && tune_r820t(AIS_CENTER_HZ, AIS_RATE, AIS_GAIN_TENTH_DB)) {
        snprintf(s_tuner, sizeof(s_tuner), "R820T (RTL-SDR Blog V3)");
        stream_test(AIS_RATE, AIS_RUN_MS, true);
        // Stay tuned, ready to record captures for the web page.
        s_state = SDR_READY;
        ESP_LOGI(TAG, "ready: record an I/Q capture from the web page");
        while (!s_gone) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            int seconds = s_cap_req;
            if (seconds && !s_gone) {
                s_state = SDR_CAPTURING;
                run_capture(seconds);
                s_cap_req = 0;
                s_state = SDR_READY;
            }
        }
    } else {
        snprintf(s_tuner, sizeof(s_tuner), r828 == R82XX_CHECK_VAL ? "R828D (not supported yet)" : "unknown");
        stream_test(AIS_RATE, RUN_MS, false);
        ESP_LOGI(TAG, "test finished; unplug and replug the dongle to run it again");
    }
    usb_host_interface_release(s_client, s_dev, 0);
out:
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
    s_tuner[0] = '\0';
    s_state = SDR_NONE;
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
    xTaskCreatePinnedToCore(probe_task, "sdr_probe", 8192, NULL, 4, &s_probe_task, 1);
}

const char *sdr_state_str(sdr_state_t st)
{
    static const char *const names[] = {"off", "no dongle", "testing", "ready", "capturing"};
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
    snprintf(out->cap_error, sizeof(out->cap_error), "%s", s_cap_error);
}

bool sdr_capture_start(int seconds, char *err, size_t errlen)
{
    if (seconds < 1 || seconds > SDR_CAPTURE_MAX_S) {
        snprintf(err, errlen, "length must be 1-%d seconds", SDR_CAPTURE_MAX_S);
        return false;
    }
    if (s_state != SDR_READY || s_cap_req) {
        snprintf(err, errlen, "%s", s_state == SDR_CAPTURING ? "a capture is already running"
                                    : s_state == SDR_TESTING ? "the dongle is still being tested; try again in a moment"
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
