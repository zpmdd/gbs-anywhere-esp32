file(GLOB display_fonts "${CMAKE_CURRENT_LIST_DIR}/fonts/*.c")
idf_component_register(SRCS "gbs_display.c" "gbs_ui.c" "gbs_ui_model.c" "gbs_hub.c" "gbs_hub_client.c" "icons/gbs_icons.c"
    ${display_fonts}
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd esp_lcd_touch_cst816s json
    PRIV_REQUIRES esp_timer esp_driver_gpio esp_driver_spi esp_driver_ledc driver nvs_flash esp_psram esp_http_client esp_netif esp_hw_support)
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_definitions(${lvgl_lib} PUBLIC LV_CONF_PATH=${CMAKE_CURRENT_LIST_DIR}/lv_conf.h)
