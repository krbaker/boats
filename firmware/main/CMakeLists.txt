# The sources every board builds, then the board's own (BOARDS_PLAN.md).
set(srcs
    "app_main.c"
    "config/settings.c"
    "config/settings_json.c"
    "net/wifi_mgr.c"
    "net/web_server.c"
    "net/log_buf.c"
    "net/log_save.c"
    "net/https_lock.c"
    "net/cp_update.c"
    "net/fw_update.c"
    "net/ais_fwd.c"
    "net/mdns_adv.c"
    "ais/vessel_db.c"
    "ais/met_db.c"
    "ais/nmea_client.c"
    "ais/rx_stats.c"
    "adsb/adsb.c"
    "tide/tide.c"
    "weather/weatherlink.c"
    "weather/weather.c"
    "geo/geo.c"
    "map/map_store.c"
    "map/map_pack.c"
    "sdr/sdr_probe.c"
    "sdr/r82xx.c"
)
if(CONFIG_AIS_HAS_DISPLAY)
    list(APPEND srcs
        "board/board_display.c"
        "ui/ui.c"
        "ui/ui_map.c"
        "ui/ui_detail.c"
        "ui/ui_netinfo.c"
        "ui/ui_settings.c"
        "ui/ui_safety.c"
        "ui/ui_flags.c"
        "ui/ui_met.c"
        "ui/ui_vessels.c"
        "ui/ui_aircraft.c"
        "ui/ui_tide.c"
        "ui/map_tiles.c"
        "ui/map_tile_decode.c"
        "ui/wx_icon_render.c"
        "ui/ui_weather.c"
    )
else()
    # No screen: no UI and no LVGL. board_headless.c does the card and USB
    # the BSP does on the others.
    list(APPEND srcs "board/board_headless.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "." "config" "net" "ais" "tide" "geo" "map" "ui" "sdr" "weather" "adsb" "board"
    EMBED_TXTFILES "web/index.html"
    REQUIRES adsb_parse ais ais_demod vessel_cache weather_parse esp_ringbuf app_update esp_http_server esp_http_client esp-tls json nvs_flash esp_wifi esp_netif esp_timer lwip usb esp_hosted mdns bootloader_support espcoredump esp_partition esp_driver_sdmmc esp_driver_gpio sdmmc fatfs
)
