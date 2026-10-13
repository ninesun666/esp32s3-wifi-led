# Registered unconditionally: putting idf_component_register() inside an
# if(CONFIG_*) branch silently drops its REQUIRES in the IDF build system.
# The source file compiles to nothing when the toggle is off.
idf_component_register(SRCS "ble_gatt.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bt status_light monitor_server net_config ota_update usb_hid json)
