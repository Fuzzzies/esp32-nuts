idf_component_register(
    SRCS
        "main.c"
        "ups_data.c"
        "wifi_mgr.c"
        "web_server.c"
        "nut_server.c"
        "ups_usb.c"
        "hid_parser.c"
        "ota_mgr.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
    REQUIRES nvs_flash esp_wifi esp_http_server esp_netif esp_event esp_timer lwip mdns usb
    PRIV_REQUIRES esp_https_ota mbedtls
)
