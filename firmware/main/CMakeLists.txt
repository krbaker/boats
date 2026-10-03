idf_component_register(
    SRCS
        "app_main.c"
        "config/settings.c"
        "net/wifi_mgr.c"
        "net/web_server.c"
        "ais/vessel_db.c"
        "ais/nmea_client.c"
        "tide/tide.c"
        "geo/geo.c"
        "map/map_store.c"
        "ui/ui.c"
        "ui/ui_map.c"
        "ui/ui_detail.c"
        "ui/ui_netinfo.c"
        "ui/map_images.c"
    INCLUDE_DIRS "." "config" "net" "ais" "tide" "geo" "map" "ui"
    EMBED_TXTFILES "web/index.html"
    REQUIRES ais esp_http_server esp_http_client esp-tls json nvs_flash esp_wifi esp_netif esp_timer lwip
)
