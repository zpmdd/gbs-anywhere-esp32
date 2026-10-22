set(integrated_sources "")
if(HUB_GRINDER)
    list(APPEND integrated_sources "../integrated/main/main.c" "../integrated/main/logic.c" "../integrated/main/network.c" "../integrated/main/recipe_log.c")
    list(APPEND integrated_sources "../integrated/main/recipe_follow.c")
endif()
idf_component_register(
    SRCS
        ${integrated_sources}
        "app_main.c"
        "hub_brew_timer.c"
        "hub_ble.c"
        "hub_cloud.c"
        "hub_schedule.c"
        "hub_cloud_websocket.c"
        "hub_cloud_websocket_policy.c"
        "hub_command_coordinator.c"
        "hub_command_deadline.c"
        "hub_config.c"
        "hub_control.c"
        "hub_event_log.c"
        "hub_http.c"
        "hub_https.c"
        "hub_http_response.c"
        "hub_identity.c"
        "hub_ota.c"
        "hub_routes.c"
        "hub_runtime_resources.c"
        "hub_security.c"
        "hub_setup_mode.c"
        "hub_state.c"
        "hub_state_keeper.c"
        "hub_statistics_cache.c"
        "hub_stomp.c"
        "hub_ws_reassembly.c"
        "hub_provisioning_ble.c"
        "hub_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES
        display
        bt
        esp_app_format
        spi_flash
        esp_http_client
        esp_http_server
        esp_https_server
        esp_websocket_client
        esp_netif
        esp_psram
        esp_timer
        esp_wifi
        app_update
        json
        mbedtls
        mdns
        nvs_flash
        bootloader_support
)
if(HUB_GRINDER)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HUB_GRINDER=1)
    target_include_directories(${COMPONENT_LIB} PRIVATE "../integrated/main")
endif()
if(HUB_TOUCHSCREEN)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HUB_TOUCHSCREEN=1)
endif()
