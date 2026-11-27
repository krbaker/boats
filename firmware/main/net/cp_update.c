// Wi-Fi co-processor (ESP32-C6) firmware: report its version, and update it
// from an uploaded image using ESP-Hosted's OTA calls. The C6 writes the new
// image to its spare slot and only switches once it is complete, so a failed
// update leaves the old firmware running.

#include "cp_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eh_host_core.h"
#include "eh_host_cp_ota.h"
#include "eh_host_sys.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "map_store.h"
#include "vessel_db.h"

static const char *TAG = "cp_update";

// The board's Wi-Fi chip: an ESP32-C6, or an ESP32-C5 on the ESP32-P4-WIFI6-DB.
#if CONFIG_SLAVE_IDF_TARGET_ESP32C5
#define CP_CHIP_ID      ESP_CHIP_ID_ESP32C5
#define CP_CHIP_NAME    "ESP32-C5"
#else
#define CP_CHIP_ID      ESP_CHIP_ID_ESP32C6
#define CP_CHIP_NAME    "ESP32-C6"
#endif

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
            if (h->chip_id != CP_CHIP_ID) {
                snprintf(u->err, sizeof(u->err), "firmware is for another chip (id %u), not this board's " CP_CHIP_NAME,
                         h->chip_id);
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
    vessel_db_save();                // first: the card shares its SDMMC host with the Wi-Fi chip
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

#define CARD_IMAGE      MAP_ROOT "/eh_cp_ota_coprocessor_ota.bin"
#define CARD_INSTALLED  MAP_ROOT "/eh_cp_ota_coprocessor_ota.installed"
#define CARD_FAILED     MAP_ROOT "/eh_cp_ota_coprocessor_ota.failed"
#define LINK_WAIT_MS    20000   // for the link to the C6 to come up after boot
#define CARD_CHUNK      4096

// Renamed so it's tried once: FAT won't rename over a file.
static void card_image_done(const char *to)
{
    unlink(to);
    rename(CARD_IMAGE, to);
}

void cp_update_from_card(void)
{
    if (!map_store_card_ok()) return;
    FILE *f = fopen(CARD_IMAGE, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char ver[24];
    for (int waited = 0; !cp_update_version(ver, sizeof(ver)) && waited < LINK_WAIT_MS; waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ESP_LOGW(TAG, "microSD card: installing %s (%ld bytes) on the Wi-Fi co-processor, now %s",
             CARD_IMAGE, size, ver);

    uint8_t *buf = malloc(CARD_CHUNK);
    cp_update_t *u = cp_update_begin();
    bool ok = buf && u;
    long done = 0;
    int shown = 0;
    while (ok) {
        size_t n = fread(buf, 1, CARD_CHUNK, f);
        if (!n) break;
        ok = cp_update_feed(u, buf, n);
        done += n;
        if (size > 0 && done * 10 / size > shown) {
            shown = done * 10 / size;
            ESP_LOGI(TAG, "%d%%", shown * 10);
        }
    }
    fclose(f);
    free(buf);
    char err[96] = "out of memory";
    if (u) ok = cp_update_finish(u, err, sizeof(err)) && ok;  // finish frees u, and restarts on success
    if (ok) {
        card_image_done(CARD_INSTALLED);
        ESP_LOGW(TAG, "Wi-Fi co-processor updated from the card; restarting");
        for (;;) vTaskDelay(portMAX_DELAY);  // the restart is on its way
    }
    card_image_done(CARD_FAILED);
    ESP_LOGE(TAG, "updating the Wi-Fi co-processor from the card failed: %s (file renamed to %s)", err, CARD_FAILED);
}
