set(APPS_DIR ./)
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE APPS_C_SRCS ${APPS_DIR}/*.c)
else()
    file(GLOB_RECURSE APPS_C_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.c)
endif()
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE APPS_CPP_SRCS ${APPS_DIR}/*.cpp)
else()
    file(GLOB_RECURSE APPS_CPP_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.cpp)
endif()

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    REQUIRES libcarto pmtiles meshcore lvgl__lvgl esp_event esp_wifi esp_netif lwip esp_hw_support esp_app_format nvs_flash esp_mm bsp_extra esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch_gt911 esp_http_client esp_http_server json vfs lakeshark esp_adc esp_driver_jpeg chmorgan__esp-file-iterator chmorgan__esp-audio-player)

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
