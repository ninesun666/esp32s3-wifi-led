idf_component_register(SRCS "monitor_server.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES link_state status_light esp_http_server json mdns)
