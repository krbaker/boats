// Display firmware updates from the web page: see fw_update.h and PLAN.md.

#include "fw_update.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

static const char *TAG = "fw_update";

#define CONFIRM_AFTER_US (60 * 1000000LL)  // a trial image that runs this long is kept

// The image header, its first segment's header and the app description: enough
// to check it's an ESP32-P4 build of this project before writing anything.
#define HEADER_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

struct fw_update {
    const esp_partition_t *slot;
    esp_ota_handle_t ota;
    bool started;
    uint8_t head[HEADER_LEN];
    size_t have, total;
    char err[128];
};

void fw_update_info(fw_info_t *out)
{
    memset(out, 0, sizeof(*out));
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(out->version, sizeof(out->version), "%s", d->version);
    snprintf(out->build_date, sizeof(out->build_date), "%s %s", d->date, d->time);
    const esp_partition_t *run = esp_ota_get_running_partition();
    snprintf(out->running, sizeof(out->running), "%s", run->label);
    esp_ota_img_states_t st;
    out->pending_verify = esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    out->rolled_back = esp_ota_get_last_invalid_partition() != NULL;
    out->can_update = esp_ota_get_next_update_partition(NULL) != NULL;
}

static void confirm_cb(void *arg)
{
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "new firmware has run for a minute: keeping it");
}

void fw_update_confirm_later(void)
{
    fw_info_t info;
    fw_update_info(&info);
    ESP_LOGI(TAG, "firmware %s (built %s) in %s%s%s", info.version, info.build_date, info.running,
             info.pending_verify ? ", on trial" : "", info.rolled_back ? "; the last update was rolled back" : "");
    if (!info.pending_verify) return;
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = confirm_cb, .name = "fw_confirm"};
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_once(t, CONFIRM_AFTER_US);
}

fw_update_t *fw_update_begin(size_t image_size, char *err, size_t errlen)
{
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (!slot) {
        snprintf(err, errlen, "this display's flash layout has no second firmware slot: flash it over USB once first");
        return NULL;
    }
    if (image_size > slot->size) {
        snprintf(err, errlen, "image is %u KB, the firmware slot holds %u KB", (unsigned)(image_size / 1024),
                 (unsigned)(slot->size / 1024));
        return NULL;
    }
    fw_update_t *u = calloc(1, sizeof(*u));
    if (!u) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    u->slot = slot;
    return u;
}

static bool fail(fw_update_t *u, const char *msg, esp_err_t e)
{
    if (!u->err[0]) {
        if (e != ESP_OK) snprintf(u->err, sizeof(u->err), "%s (%s)", msg, esp_err_to_name(e));
        else snprintf(u->err, sizeof(u->err), "%s", msg);
    }
    return false;
}

// Checks the start of the image, then opens the slot and writes what's buffered.
static bool start(fw_update_t *u)
{
    const esp_image_header_t *h = (const esp_image_header_t *)u->head;
    const esp_app_desc_t *d =
        (const esp_app_desc_t *)(u->head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (h->magic != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return fail(u, "not an ESP firmware image (upload firmware/build/ais_display.bin)", ESP_OK);
    }
    if (h->chip_id != ESP_CHIP_ID_ESP32P4) return fail(u, "this image is for another chip, not the display's ESP32-P4", ESP_OK);
    const esp_app_desc_t *me = esp_app_get_description();
    if (strncmp(d->project_name, me->project_name, sizeof(d->project_name)) != 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "this is \"%.32s\" firmware, not the display's (%.32s)", d->project_name,
                 me->project_name);
        return fail(u, msg, ESP_OK);
    }
    ui_updating_begin();  // before the first flash write
    esp_err_t e = esp_ota_begin(u->slot, OTA_WITH_SEQUENTIAL_WRITES, &u->ota);
    if (e != ESP_OK) return fail(u, "could not start writing the firmware", e);
    u->started = true;
    ESP_LOGI(TAG, "updating to %s (built %s %s) in %s", d->version, d->date, d->time, u->slot->label);
    e = esp_ota_write(u->ota, u->head, u->have);
    return e == ESP_OK || fail(u, "writing the firmware failed", e);
}

bool fw_update_feed(fw_update_t *u, const uint8_t *data, size_t len)
{
    if (u->err[0]) return false;
    u->total += len;
    if (!u->started) {
        size_t take = HEADER_LEN - u->have < len ? HEADER_LEN - u->have : len;
        memcpy(u->head + u->have, data, take);
        u->have += take;
        data += take;
        len -= take;
        if (u->have < HEADER_LEN) return true;  // need more before it can be checked
        if (!start(u)) return false;
    }
    if (!len) return true;
    esp_err_t e = esp_ota_write(u->ota, data, len);
    return e == ESP_OK || fail(u, "writing the firmware failed", e);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));  // let the HTTP response go out
    esp_restart();
}

bool fw_update_finish(fw_update_t *u, char *err, size_t errlen)
{
    bool ok = !u->err[0];
    if (ok && !u->started) ok = fail(u, "upload too short to be a firmware image", ESP_OK);
    if (u->started) {
        if (ok) {
            esp_err_t e = esp_ota_end(u->ota);  // checks the whole image, including its SHA-256
            if (e != ESP_OK) ok = fail(u, e == ESP_ERR_OTA_VALIDATE_FAILED ? "the image is damaged or incomplete" : "finishing the update failed", e == ESP_ERR_OTA_VALIDATE_FAILED ? ESP_OK : e);
        } else {
            esp_ota_abort(u->ota);
        }
    }
    if (ok) {
        esp_err_t e = esp_ota_set_boot_partition(u->slot);
        if (e != ESP_OK) ok = fail(u, "could not switch to the new firmware", e);
    }
    if (ok) {
        ESP_LOGI(TAG, "firmware written (%u bytes); restarting into it", (unsigned)u->total);
        xTaskCreate(restart_task, "fw_restart", 2048, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG, "update failed: %s", u->err);
        ui_updating_end();
        snprintf(err, errlen, "%s", u->err);
    }
    free(u);
    return ok;
}
