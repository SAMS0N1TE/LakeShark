if(CONFIG_LAKESHARK_HEADLESS)
    # Headless entry: radio core + UART console, no GUI. (Brookesia/LVGL are
    # still compiled below for now since the GUI components pull them in;
    # excluding them entirely is a follow-up optimization.)
    idf_component_register(
        SRCS headless_main.c flipper_link.c ble_link.c bsod.c
        INCLUDE_DIRS .
        PRIV_REQUIRES console esp_driver_uart esp_driver_gpio esp_timer lakeshark espressif__esp_hosted bt)
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
        SRCS main.cpp boot_splash.cpp bsod.c gui_link.cpp flipper_link.c ble_link.c
        INCLUDE_DIRS .)
endif()

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE 
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

if(CONFIG_ESP_BROOKESIA_MEMORY_USE_CUSTOM)
    idf_build_get_property(build_components BUILD_COMPONENTS)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "esp-brookesia" OR COMPONENT MATCHES "espressif__esp-brookesia")
            set(TARGET_COMPONENT ${COMPONENT})
            break()
        endif()
    endforeach()

    if(TARGET_COMPONENT STREQUAL "")
        message(FATAL_ERROR "Component 'esp-brookesia' not found.")
    else()
        idf_component_get_property(ESP_BROOKESIA_LIB ${TARGET_COMPONENT} COMPONENT_LIB)
    endif()

    target_compile_options(
        ${ESP_BROOKESIA_LIB}
        PUBLIC
            "-DESP_BROOKESIA_MEMORY_CUSTOM_MALLOC(x)=heap_caps_aligned_alloc(1, x, MALLOC_CAP_SPIRAM)"  "-DESP_BROOKESIA_MEMORY_CUSTOM_FREE=free"
    )

    # The custom MALLOC/FREE shim (allocate via operator new, release via plain
    # free) trips -Werror=mismatched-new-delete once C++ exceptions are enabled
    # (LoRaMesher needs them). Benign allocator mismatch in third-party code;
    # relax it for the whole brookesia lib. target_compile_options lands after
    # IDF's -Werror=all, so -Wno-error wins.
    target_compile_options(${ESP_BROOKESIA_LIB} PRIVATE
        -Wno-error=mismatched-new-delete -Wno-error=stringop-overflow)
endif()

if(CONFIG_LV_MEM_CUSTOM)
    target_compile_options(
        ${LVGL_LIB} 
        PRIVATE 
            "-DLV_MEM_CUSTOM_ALLOC(x)=heap_caps_malloc(x, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"
    )
endif()

spiffs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)