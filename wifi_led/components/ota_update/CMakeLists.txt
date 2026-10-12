# Registered unconditionally: idf_component_register must not sit inside
# if(CONFIG_*), which silently drops the REQUIRES (project-wide gotcha).
idf_component_register(SRCS "ota_update.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update mbedtls
                       PRIV_REQUIRES freertos esp_timer log)
