# LS-816  BLE control head, on or off.
#
# When CONFIG_LS_BLE_HEAD is off we compile ble_link_stub.c INSTEAD of
# ble_link.c. Same symbols, all no-ops, so none of the ~40 ble_link_* call
# sites in headless_main.c / gui_link.cpp / flipper_link.c / ls_wifi.c need an
# #ifdef, and the `ble` console commands stay present and answer honestly.
if(CONFIG_LS_BLE_HEAD)
    # LS-825  ble_link_core.c holds the pure protocol logic (framing, backoff,
    # disconnect classification, state machine, LS-82x invariants) so the
    # bench can drive it against ble_link_core.h without an ESP-IDF build.
    set(LS_BLE_SRC ble_link.c ble_link_core.c ble_link_host_task.c)
    set(LS_BLE_REQ bt)
else()
    # ble_link_core is transport-independent and cheap; carry it in the stub
    # build too so ble_link.h (which includes ble_link_core.h) always has a
    # backing definition for anything that isn't a no-op API call.
    set(LS_BLE_SRC ble_link_stub.c ble_link_core.c)
    set(LS_BLE_REQ "")
endif()

if(CONFIG_LAKESHARK_HEADLESS)
    # Headless entry: radio core + UART console, no GUI. LVGL is still
    # compiled below since the GUI components pull it in; excluding it
    # entirely is a follow-up optimization.
    idf_component_register(
        SRCS headless_main.c flipper_link.c flipper_link_telemetry.c ${LS_BLE_SRC} bsod.c
        INCLUDE_DIRS .
        PRIV_REQUIRES console esp_driver_uart esp_driver_gpio esp_timer lakeshark espressif__esp_hosted ${LS_BLE_REQ})
else()
    # LS-019  The GUI build carries the Flipper head too. flipper_link.c is the
    # command parser and telemetry builder for BOTH transports, so it is needed
    # even on a board that only ever talks BLE - ble_link.c feeds it through
    # flipper_link_inject().
    #
    # No REQUIRES/PRIV_REQUIRES here on purpose. `main` already depends on every
    # component in the build - that is how main.cpp includes esp_hosted.h and the
    # app headers today without declaring anything. The headless branch above can
    # afford to be explicit because it pulls in far less; naming a partial list
    # here is the easy way to break the GUI includes.
    idf_component_register(
        SRCS main.cpp boot_splash.cpp bsod.c gui_link.cpp flipper_link.c flipper_link_telemetry.c ${LS_BLE_SRC} ls_wifi.c ls_wifi_sta_core.c screenshot.c screenshot_plan.c
        INCLUDE_DIRS .)
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
