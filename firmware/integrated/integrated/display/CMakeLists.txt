set(display_sources "")
if(HUB_TOUCHSCREEN)
    file(GLOB display_fonts "${CMAKE_CURRENT_LIST_DIR}/fonts/*.c")
    list(APPEND display_sources "gbs_display.c" "gbs_ui.c" "gbs_ui_model.c" "gbs_hub.c" "gbs_hub_client.c" "../../main/hub_touchscreen.c" "../../main/hub_touchscreen_command.c" "icons/gbs_icons.c" ${display_fonts})
endif()
idf_component_register(SRCS ${display_sources}
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd esp_lcd_touch_cst816s json
    PRIV_REQUIRES esp_timer esp_driver_gpio esp_driver_spi esp_driver_ledc driver nvs_flash esp_psram esp_hw_support esp_http_server)
if(HUB_TOUCHSCREEN)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_definitions(${lvgl_lib} PUBLIC LV_CONF_PATH=${CMAKE_CURRENT_LIST_DIR}/lv_conf.h)
    target_include_directories(${COMPONENT_LIB} PRIVATE "../../main")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HUB_TOUCHSCREEN=1)
endif()
