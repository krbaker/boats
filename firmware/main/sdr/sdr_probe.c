/*
 * RTL-SDR bring-up test (RTL_SDR_PLAN.md, step 1): find an RTL2832U dongle on
 * the USB-A port, report what it is and how it connected, start its sample
 * stream, and measure USB throughput. Results go to the serial log only.
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

#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "sdr_probe";

#define RTL_VID             0x0bda
#define RTL_XTAL_HZ         28800000
#define BULK_EP             0x81
#define BULK_XFER_BYTES     (16 * 1024)
#define BULK_XFERS          4
#define CTRL_TIMEOUT_MS     300
#define RUN_MS              10000

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
// Streaming test
// ---------------------------------------------------------------------------

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
    } else {
        s_xfer_errors++;
    }
    if (s_streaming && usb_host_transfer_submit(t) != ESP_OK) s_xfer_errors++;
}

static void stream_test(uint32_t rate)
{
    uint32_t real = set_sample_rate(rate);
    reset_buffer();
    s_bytes = s_xfer_errors = s_short_xfers = 0;
    s_hist_min = 255;
    s_hist_max = 0;
    s_sum = s_count = 0;
    s_streaming = true;

    usb_transfer_t *xfers[BULK_XFERS] = {0};
    for (int i = 0; i < BULK_XFERS; i++) {
        if (usb_host_transfer_alloc(BULK_XFER_BYTES, 0, &xfers[i]) != ESP_OK) {
            ESP_LOGE(TAG, "transfer alloc failed (internal RAM free: %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_streaming = false;
            break;
        }
        xfers[i]->device_handle = s_dev;
        xfers[i]->bEndpointAddress = BULK_EP;
        xfers[i]->callback = bulk_cb;
        xfers[i]->num_bytes = BULK_XFER_BYTES;
        xfers[i]->timeout_ms = 0;
        if (usb_host_transfer_submit(xfers[i]) != ESP_OK) {
            ESP_LOGE(TAG, "bulk submit failed");
            s_streaming = false;
        }
    }

    int64_t t0 = esp_timer_get_time();
    uint64_t last = 0;
    for (int s = 1; s * 2000 <= RUN_MS && s_streaming && !s_gone; s++) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        uint64_t b = s_bytes;
        ESP_LOGI(TAG, "  %u S/s: %.2f MB/s this period (need %.2f), errors %lu, short %lu", (unsigned)real,
                 (b - last) / 2.0 / 1e6, real * 2 / 1e6, (unsigned long)s_xfer_errors, (unsigned long)s_short_xfers);
        last = b;
    }
    s_streaming = false;
    vTaskDelay(pdMS_TO_TICKS(300)); // let in-flight transfers finish
    double secs = (esp_timer_get_time() - t0) / 1e6;
    double got = s_bytes / secs, need = real * 2.0;
    ESP_LOGI(TAG, "RESULT %u S/s: %.2f MB/s of %.2f MB/s needed (%.0f%%), %lu errors, samples %lu..%lu mean %.1f",
             (unsigned)real, got / 1e6, need / 1e6, 100 * got / need, (unsigned long)s_xfer_errors,
             (unsigned long)s_hist_min, (unsigned long)s_hist_max, s_count ? (double)s_sum / s_count : 0.0);
    for (int i = 0; i < BULK_XFERS; i++) {
        if (xfers[i]) usb_host_transfer_free(xfers[i]);
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
    // The rate AIS will use, then the dongle's usual top rate as a stress test.
    stream_test(288000);
    if (!s_gone) stream_test(2400000);
    ESP_LOGI(TAG, "test finished; unplug and replug the dongle to run it again");
    usb_host_interface_release(s_client, s_dev, 0);
out:
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
}

static void probe_task(void *arg)
{
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
    xTaskCreatePinnedToCore(probe_task, "sdr_probe", 6144, NULL, 4, &s_probe_task, 1);
}
