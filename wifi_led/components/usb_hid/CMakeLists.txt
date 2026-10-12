# Registered unconditionally: putting idf_component_register() inside an
# if(CONFIG_*) branch silently drops its REQUIRES in the IDF build system.
# usb_hid.c compiles to stubs when the toggle is off; the esp_tinyusb download
# and its include paths come via idf_component.yml's rule only when enabled.
idf_component_register(SRCS "usb_hid.c"
                       INCLUDE_DIRS "include")
