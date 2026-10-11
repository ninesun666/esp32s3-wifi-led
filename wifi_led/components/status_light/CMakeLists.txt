idf_component_register(SRCS "status_light.c"
                       INCLUDE_DIRS "include"
                       REQUIRES link_state led_strip esp_driver_rmt esp_driver_gpio esp_timer json)
