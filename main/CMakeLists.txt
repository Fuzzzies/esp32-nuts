idf_component_register(
    SRCS
        "main.c"
        "ups_data.c"
        "wifi_mgr.c"
        "web_server.c"
        "nut_server.c"
        "ups_usb.c"
        "hid_parser.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "www/index.html"
)
