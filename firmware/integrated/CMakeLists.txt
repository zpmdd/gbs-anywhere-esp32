cmake_minimum_required(VERSION 3.16)
option(HUB_GRINDER "Include the integrated grinder workflow" OFF)
option(HUB_TOUCHSCREEN "Include Waveshare ESP32-S3 Touch LCD 1.69" OFF)
if(HUB_TOUCHSCREEN)
    set(HUB_GRINDER ON)
endif()

# Keep generated configuration scoped to the build directory so a stale local
# sdkconfig cannot silently override the checked-in release defaults.
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/integrated/display")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lamabuddy_esp32_s3_hub)
