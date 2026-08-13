if(CONFIG_LAKESHARK_HEADLESS)
    # Headless entry: radio core + UART console, no GUI. LVGL is still
    # compiled below since the GUI components pull it in; excluding it
    # entirely is a follow-up optimization.
    idf_component_register(
        SRCS headless_main.c flipper_link.c ble_link.c bsod.c
        INCLUDE_DIRS .
        PRIV_REQUIRES console esp_driver_uart esp_driver_gpio esp_timer lakeshark espressif__esp_hosted bt)
else()
    idf_component_register(
        SRCS main.cpp boot_splash.cpp bsod.c
        INCLUDE_DIRS .)
endif()

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB}
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
)

if(CONFIG_LV_MEM_CUSTOM)
    target_compile_options(
        ${LVGL_LIB} 
        PRIVATE 
            "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
    )
endif()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)