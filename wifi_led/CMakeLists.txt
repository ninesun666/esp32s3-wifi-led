# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Two-layer config: committed hardware defaults + gitignored local secrets.
# An explicit -DSDKCONFIG_DEFAULTS=... (CI swaps the local layer for
# sdkconfig.ci) wins; otherwise the local layer is appended only when present,
# because IDF hard-fails on a listed-but-missing defaults file and a fresh
# checkout has no secrets to merge.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local)
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.local")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_led)
