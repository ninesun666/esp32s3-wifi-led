idf_component_register(SRCS "monitor_server.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES link_state net_config status_light display_tft i2c_probe ota_update usb_hid esp_http_server json mdns)
