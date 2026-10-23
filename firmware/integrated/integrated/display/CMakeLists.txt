set(display_sources "")
if(HUB_TOUCHSCREEN)
    # IDF's early requirements script cannot track globs; the build must track regenerated fonts.
    if(NOT CMAKE_BUILD_EARLY_EXPANSION)
        file(GLOB display_fonts CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fonts/*.c")
    endif()
    list(APPEND display_sources "gbs_display.c" "gbs_ui.c" "gbs_ui_model.c" "gbs_hub.c" "gbs_hub_client.c" "../../main/hub_touchscreen.c" "../../main/hub_touchscreen_command.c" "icons/gbs_icons.c" ${display_fonts})
endif()
set(board_dependencies "")
if(HUB_AMOLED_216)
    list(APPEND display_sources "gbs_board_amoled216.c" "gbs_device_policy.c" "gbs_scale_battery_cache.c")
    list(APPEND board_dependencies esp_lcd_co5300 esp_lcd_touch_cst9217 esp_codec_dev esp_driver_i2c esp_driver_i2s)
endif()
idf_component_register(SRCS ${display_sources}
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd esp_lcd_touch_cst816s json
    PRIV_REQUIRES ${board_dependencies} esp_timer esp_driver_gpio esp_driver_spi esp_driver_ledc driver nvs_flash esp_psram esp_hw_support esp_http_server)
if(HUB_TOUCHSCREEN)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PUBLIC LV_CONF_PATH=${CMAKE_CURRENT_LIST_DIR}/lv_conf.h)
    target_include_directories(${COMPONENT_LIB} PRIVATE "../../main")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HUB_TOUCHSCREEN=1)
endif()

if(HUB_AMOLED_216)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HUB_AMOLED_216=1)
    # Optimize the larger display without changing the established Hub build profile.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
    target_compile_options(${lvgl_lib} PRIVATE -O2)
endif()
