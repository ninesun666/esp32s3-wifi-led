idf_component_register(SRCS "app_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES link_state status_light display_tft monitor_server)
