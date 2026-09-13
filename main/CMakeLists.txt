if(CONFIG_LS_BLE_HEAD)
    set(LS_BLE_SRC ble_link.c ble_link_core.c ble_link_host_task.c ble_hci_rx_guard.c)
    set(LS_BLE_REQ bt)
else()
    set(LS_BLE_SRC ble_link_stub.c ble_link_core.c)
    set(LS_BLE_REQ "")
endif()

if(CONFIG_LAKESHARK_HEADLESS)
    idf_component_register(
        SRCS headless_main.c compact_ui.cpp sx1262_console.cpp ls_wifi.c ls_wifi_sta_core.c ls_wireless.c flipper_link.c flipper_link_telemetry.c ${LS_BLE_SRC} bsod.c ls_hosted_board.c
        INCLUDE_DIRS .
        PRIV_REQUIRES console esp_driver_uart esp_driver_gpio esp_timer lakeshark apps meshcore esp_lvgl_port espressif__esp_hosted ${LS_BLE_REQ})
else()
    idf_component_register(
        SRCS main.cpp boot_splash.cpp bsod.c gui_link.cpp flipper_link.c flipper_link_telemetry.c ${LS_BLE_SRC} ls_wifi.c ls_wifi_sta_core.c ls_wireless.c screenshot.c screenshot_plan.c
        INCLUDE_DIRS .)
endif()

if(CONFIG_LAKESHARK_HEADLESS)
    idf_component_get_property(LS_HOSTED_DIR espressif__esp_hosted COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${LS_HOSTED_DIR}/host/port/esp/freertos/include")
endif()

if(CONFIG_LS_BLE_HEAD)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=hci_rx_handler"
        "-Wl,--wrap=transport_drv_reconfigure")
endif()

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
)

if(CONFIG_LV_MEM_CUSTOM)
    include(${CMAKE_CURRENT_LIST_DIR}/ls_lvgl_allocator.cmake)
    ls_configure_lvgl_allocator(${LVGL_LIB})
endif()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
