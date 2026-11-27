// mDNS advertisement: see mdns_adv.h.
//
// The mdns component answers on the Wi-Fi station and access point by itself
// (its predefined interfaces follow the Wi-Fi events). This only sets the
// name and the services, and redoes them when the name or the TCP server's
// settings change. Its task and its memory are in PSRAM (sdkconfig.defaults).

#include "mdns_adv.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"

#include "settings.h"

static const char *TAG = "mdns";

static bool s_running;

static void apply(void)
{
    settings_t cfg;
    settings_get(&cfg);

    // Routers show the DHCP host name in their client lists: keep it the same.
    // It's sent from the next lease renewal on.
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_set_hostname(sta, cfg.mdns_name);

    mdns_hostname_set(cfg.mdns_name);
    mdns_instance_name_set(cfg.mdns_name);
    mdns_service_remove_all();
    mdns_txt_item_t http_txt[] = {{"path", "/"}};
    mdns_service_add(NULL, "_http", "_tcp", 80, http_txt, 1);
    if (cfg.srv_enabled) mdns_service_add(NULL, "_nmea-0183", "_tcp", cfg.srv_port, NULL, 0);
    ESP_LOGI(TAG, "%s.local: config page%s", cfg.mdns_name, cfg.srv_enabled ? ", NMEA server" : "");
}

static void mdns_task(void *arg)
{
    EventGroupHandle_t ev = arg;
    for (;;) {
        apply();
        // The TCP server's settings raise SETTINGS_EV_FWD, as the destinations' do.
        xEventGroupWaitBits(ev, SETTINGS_EV_NAME | SETTINGS_EV_FWD, pdTRUE, pdFALSE, portMAX_DELAY);
    }
}

void mdns_adv_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    s_running = true;
    // The stack in PSRAM: it only waits for settings changes.
    xTaskCreateWithCaps(mdns_task, "mdns_adv", 3072, settings_subscribe(), 2, NULL, MALLOC_CAP_SPIRAM);
}

void mdns_adv_hostname(char *out, size_t cap)
{
    char name[MDNS_NAME_BUF_LEN] = "";
    if (s_running) mdns_hostname_get(name);
    strlcpy(out, name, cap);
}
