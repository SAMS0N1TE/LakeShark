if(CONFIG_LS_BLE_HEAD)
    set(LS_BLE_SRC ble_link.c ble_link_core.c ble_link_host_task.c ble_hci_rx_guard.c)
    set(LS_BLE_REQ bt)
else()
    set(LS_BLE_SRC ble_link_stub.c ble_link_core.c)
    set(LS_BLE_REQ "")
endif()

# Optional private extensions, compiled only when this tree has them.
# Deleting the file is the whole removal -
# compact_ui.cpp reaches it through __has_include and needs no edit.
set(LS_PRIVATE_SRC "")
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/private_console.cpp)
    list(APPEND LS_PRIVATE_SRC private_console.cpp)
endif()

if(CONFIG_LAKESHARK_HEADLESS)
    idf_component_register(
        SRCS headless_main.c compact_ui.cpp sx1262_console.cpp ${LS_PRIVATE_SRC} ls_wifi.c ls_wifi_sta_core.c ls_wireless.c flipper_link.c flipper_link_telemetry.c ${LS_BLE_SRC} bsod.c ls_hosted_board.c panic_crumb.c cache_sync_lock.c console_lock.c
        INCLUDE_DIRS .
        PRIV_REQUIRES console esp_driver_uart esp_driver_gpio esp_timer lakeshark apps meshcore esp_lvgl_port espressif__esp_hosted ${LS_BLE_REQ})
else()
    idf_component_register(
        SRCS main.cpp boot_splash.cpp bsod.c panic_crumb.c cache_sync_lock.c gui_link.cpp flipper_link.c flipper_link_telemetry.c ${LS_BLE_SRC} ls_wifi.c ls_wifi_sta_core.c ls_wireless.c screenshot.c screenshot_plan.c
        INCLUDE_DIRS .)
endif()

if(CONFIG_LAKESHARK_HEADLESS)
    # The apps component's radio picker reads the wireless snapshot, and the
    # headless main code does not, so the linker would pass main before
    # anything asked for it.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ls_wireless_get")
    idf_component_get_property(LS_HOSTED_DIR espressif__esp_hosted COMPONENT_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${LS_HOSTED_DIR}/host/port/esp/freertos/include")
    # console_lock.c: TERMINAL runs commands beside the UART REPL, and
    # esp_console_run keeps the line in one shared buffer.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_console_run")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_esp_console_run")
endif()

# panic_crumb.c notes each core's trap frame before IDF's panic handler runs.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=panicHandler"
    "-Wl,--wrap=xt_unhandled_exception")

# cache_sync_lock.c serialises every address-range cache operation, as
# ESP-IDF 5.5.4 does and 5.4.3 does not.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=cache_hal_invalidate_addr"
    "-Wl,--wrap=cache_hal_writeback_addr")
# Nothing else names its functions, so the archive member is pulled in by hand.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u __wrap_cache_hal_invalidate_addr"
    "-u __wrap_cache_hal_writeback_addr")

if(CONFIG_LS_BLE_HEAD)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=hci_rx_handler"
        "-Wl,--wrap=transport_drv_reconfigure")
endif()

# Format checking back on for main/, against the project-wide -Wno-format.
# This is where the console commands live, and a console command is almost
# entirely printf.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat -Wno-format-truncation)

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
