idf_component_register(SRCS "main.c" "network.c" "logic.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_http_client esp_http_server
             esp_timer mbedtls json console esp_psram
    PRIV_REQUIRES esp_app_format
    EMBED_TXTFILES "index.html")
if(GBS_TOUCHSCREEN)
    idf_component_optional_requires(PRIVATE display)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GBS_TOUCHSCREEN=1)
endif()
