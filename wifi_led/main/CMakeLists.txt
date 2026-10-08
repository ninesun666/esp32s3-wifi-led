idf_component_register(SRCS "wifi_led_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES json esp_wifi nvs_flash esp_event esp_netif
                                esp_http_server esp_timer driver led_strip lwip
                                heap bt)
