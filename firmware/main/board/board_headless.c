// The screenless boards: see board.h. Waveshare's ESP32-P4-WIFI6-POE-ETH, and
// its boards without Ethernet: the same pins for the card and USB.
//
// microSD on SDMMC slot 0, 4 bits: CLK 43, CMD 44, D0-D3 39-42, as on the
// boards with a screen. The card's 3.3 V goes through a P-channel switch whose
// gate is pulled down (on) and also on GPIO45: driven low here so it's on
// whatever the pin did before. The pins' own supply comes from the P4's LDO
// channel 4, as the BSP and ESP-IDF's P4 SD example set it up.
// The USB-A port's 5 V is always on (its switch's enable is tied high).

#include "board.h"

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "usb/usb_host.h"

static const char *TAG = "board";

#define SD_POWER_GPIO   GPIO_NUM_45     // low: the card has power
#define SD_LDO_CHAN     4

esp_err_t board_sdcard_mount(void)
{
    gpio_config_t pwr = {.pin_bit_mask = 1ULL << SD_POWER_GPIO, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&pwr);
    gpio_set_level(SD_POWER_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10)); // let the card's supply settle

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = SD_LDO_CHAN};
    sd_pwr_ctrl_handle_t pwr_ctrl = NULL;
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD pin supply (LDO %d): %s", SD_LDO_CHAN, esp_err_to_name(err));
        return err;
    }
    host.pwr_ctrl_handle = pwr_ctrl;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // As the BSP: never format a card that won't mount.
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 64 * 1024,
    };
    sdmmc_card_t *card;
    return esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &card);
}

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

esp_err_t board_usb_host_start(void)
{
    const usb_host_config_t cfg = {.skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1};
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) return err;
    // On core 0, like the rest of the system work: core 1 is the SDR decoder's.
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL, 0) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Three quick power cycles: the setup access point
// ---------------------------------------------------------------------------

#define QUICK_BOOT_US   (5 * 1000000LL)  // a power-on that lasts less than this counts
#define QUICK_BOOTS     3

// Kept in NVS: a power cut clears the RTC memory.
static void set_quick_boots(uint8_t n)
{
    nvs_handle_t h;
    if (nvs_open("board", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "quick_boots", n);
    nvs_commit(h);
    nvs_close(h);
}

static void running_cb(void *arg)
{
    set_quick_boots(0); // this one lasted: it wasn't part of a quick series
}

bool board_ap_requested(void)
{
    // Only power-ons count: a crash loop mustn't open the access point.
    if (esp_reset_reason() != ESP_RST_POWERON) {
        set_quick_boots(0);
        return false;
    }
    uint8_t n = 0;
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "quick_boots", &n);
        nvs_close(h);
    }
    n++;
    if (n >= QUICK_BOOTS) {
        set_quick_boots(0);
        ESP_LOGW(TAG, "%d quick power cycles: starting the setup access point", QUICK_BOOTS);
        return true;
    }
    set_quick_boots(n);
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = {.callback = running_cb, .name = "quick_boots"};
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_once(t, QUICK_BOOT_US);
    return false;
}
