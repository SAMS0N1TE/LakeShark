set(APPS_DIR ./)
# CONFIGURE_DEPENDS: without it a glob is evaluated once, at configure time,
# and a source added afterwards is silently not compiled. That is exactly what
# happened when the keyboard input layer landed - ls_input.cpp and
# ls_text_entry.cpp existed, every host test passed, and the device build died
# at link with undefined references to functions whose .cpp was sitting right
# there unbuilt.
if(CMAKE_SCRIPT_MODE_FILE)
    # ESP-IDF evaluates component CMakeLists twice, and the requirements pass
    # runs in script mode, where CONFIGURE_DEPENDS is a hard error. Only ask
    # for it on the pass that can act on it.
    file(GLOB_RECURSE APPS_C_SRCS ${APPS_DIR}/*.c)
else()
    file(GLOB_RECURSE APPS_C_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.c)
endif()
if(CMAKE_SCRIPT_MODE_FILE)
    # ESP-IDF evaluates component CMakeLists twice, and the requirements pass
    # runs in script mode, where CONFIGURE_DEPENDS is a hard error. Only ask
    # for it on the pass that can act on it.
    file(GLOB_RECURSE APPS_CPP_SRCS ${APPS_DIR}/*.cpp)
else()
    file(GLOB_RECURSE APPS_CPP_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.cpp)
endif()

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    REQUIRES lvgl__lvgl esp_event esp_wifi esp_netif lwip esp_hw_support esp_app_format nvs_flash esp_mm bsp_extra esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch_gt911 esp_http_client esp_http_server json vfs lakeshark esp_adc esp_driver_jpeg chmorgan__esp-file-iterator chmorgan__esp-audio-player)

target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wno-format
        -Wno-incompatible-pointer-types
        -Wno-int-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wno-format
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
    >
    -DLV_LVGL_H_INCLUDE_SIMPLE
)
