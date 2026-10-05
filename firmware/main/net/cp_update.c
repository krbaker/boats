// Wi-Fi co-processor (ESP32-C6) firmware: report its version, and update it
// from an uploaded image using ESP-Hosted's OTA calls. The C6 writes the new
// image to its spare slot and only switches once it is complete, so a failed
// update leaves the old firmware running.

#include "cp_update.h"

#include <stdio.h>
#include <string.h>

#include "eh_host_core.h"
#include "eh_host_cp_ota.h"
#include "eh_host_sys.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cp_update";

#define OTA_CHUNK 1536  // largest write ESP-Hosted accepts in one call

struct cp_update {
    uint8_t buf[OTA_CHUNK];
    size_t have;
    size_t total;
    bool started;
    char err[96];
};

bool cp_update_version(char *out, size_t len)
{
    // Asked over RPC once, then cached: the status page polls every few seconds.
    static char s_ver[24];
    if (!s_ver[0]) {
        eh_host_coprocessor_fwver_t v = {0};
        if (eh_host_sys_get_cp_fw_version(&v) == ESP_OK) {
            snprintf(s_ver, sizeof(s_ver), "%lu.%lu.%lu", (unsigned long)v.major1, (unsigned long)v.minor1,
                     (unsigned long)v.patch1);
        }
    }
    snprintf(out, len, "%s", s_ver[0] ? s_ver : "unknown");
    return s_ver[0] != 0;
}

cp_update_t *cp_update_begin(void)
{
    return calloc(1, sizeof(cp_update_t));
}

static bool fail(cp_update_t *u, const char *fmt, esp_err_t e)
{
    if (!u->err[0]) snprintf(u->err, sizeof(u->err), fmt, esp_err_to_name(e));
    ESP_LOGE(TAG, "%s", u->err);
    return false;
}

static bool flush(cp_update_t *u)
{
    if (!u->have) return true;
    esp_err_t e = eh_host_cp_ota_write(u->buf, u->have);
    u->have = 0;
    return e == ESP_OK || fail(u, "writing to the Wi-Fi chip failed (%s)", e);
}

bool cp_update_feed(cp_update_t *u, const uint8_t *data, size_t len)
{
    if (u->err[0]) return false;
    while (len) {
        size_t take = OTA_CHUNK - u->have < len ? OTA_CHUNK - u->have : len;
        memcpy(u->buf + u->have, data, take);
        u->have += take;
        u->total += take;
        data += take;
        len -= take;

        // Before sending anything, check this really is an ESP32-C6 firmware
        // image, not (say) the display's own firmware.
        if (!u->started && u->have >= sizeof(esp_image_header_t)) {
            const esp_image_header_t *h = (const esp_image_header_t *)u->buf;
            if (h->magic != ESP_IMAGE_HEADER_MAGIC) {
                snprintf(u->err, sizeof(u->err), "not an ESP firmware image (.bin)");
                return false;
            }
            if (h->chip_id != ESP_CHIP_ID_ESP32C6) {
                snprintf(u->err, sizeof(u->err), "firmware is for another chip (id %u), not the ESP32-C6", h->chip_id);
                return false;
            }
            esp_err_t e = eh_host_cp_ota_begin();
            if (e != ESP_OK) return fail(u, "the Wi-Fi chip refused the update (%s); its firmware may be too old to update this way", e);
            u->started = true;
            ESP_LOGI(TAG, "updating Wi-Fi co-processor firmware");
        }
        if (u->started && u->have == OTA_CHUNK && !flush(u)) return false;
    }
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); // let the HTTP response go out
    eh_host_deinit();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

bool cp_update_finish(cp_update_t *u, char *err, size_t errlen)
{
    bool ok = !u->err[0];
    if (ok && !u->started) {
        snprintf(u->err, sizeof(u->err), "upload too short to be a firmware image");
        ok = false;
    }
    if (ok) ok = flush(u);
    if (ok) {
        esp_err_t e = eh_host_cp_ota_end();
        if (e != ESP_OK) ok = fail(u, "the Wi-Fi chip rejected the image (%s)", e);
    }
    if (ok) {
        // Older C6 firmware (before ESP-Hosted 2.6) switches by itself on end;
        // newer firmware needs the separate activate call.
        esp_err_t e = eh_host_cp_ota_activate();
        if (e != ESP_OK) ESP_LOGW(TAG, "activate: %s (fine on older co-processor firmware)", esp_err_to_name(e));
        ESP_LOGI(TAG, "Wi-Fi co-processor firmware written (%u bytes); restarting", (unsigned)u->total);
        xTaskCreate(restart_task, "cp_restart", 3072, NULL, 5, NULL);
    }
    if (!ok) snprintf(err, errlen, "%s", u->err);
    free(u);
    return ok;
}
