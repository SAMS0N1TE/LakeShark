
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE LS_SRCS
        board/*.c core/*.c radio/*.c audio/*.c output/*.c dsp/*.c nfc/*.c
        apps/*/*.c apps/*/*.cpp)
else()
    file(GLOB_RECURSE LS_SRCS CONFIGURE_DEPENDS
        board/*.c core/*.c radio/*.c audio/*.c output/*.c dsp/*.c nfc/*.c
        apps/*/*.c apps/*/*.cpp)
endif()

file(GLOB LS_APP_DIR_CANDIDATES ${CMAKE_CURRENT_SOURCE_DIR}/apps/*)
set(LS_APP_INCLUDES "")
foreach(d ${LS_APP_DIR_CANDIDATES})
    if(IS_DIRECTORY ${d})
        list(APPEND LS_APP_INCLUDES ${d})
    endif()
endforeach()
list(APPEND LS_SRCS lakeshark_backend.c radio/iq_app_control.c)
list(REMOVE_DUPLICATES LS_SRCS)

idf_component_register(
    SRCS ${LS_SRCS}
    INCLUDE_DIRS
        .
        board
        core
        radio
        audio
        output
        dsp
        ${LS_APP_INCLUDES}
        nfc
    REQUIRES
        usb
        driver
        esp_driver_uart
        esp_timer
        esp_netif
        lwip
        esp_hw_support
        nvs_flash
        console
        espcoredump
        esp_app_format
        fatfs
        spiffs
        mbelib
        imbe_vocoder
        esp32_p4_wifi6_touch_lcd_4b
        bsp_extra
        esp_codec_dev
        esp_lcd
        esp_lvgl_port
        lvgl
)

set(P25_C_SOURCES
    apps/p25/scanner.c
    apps/p25/dsd_main.c
    apps/p25/p25_demod_mode.c
    apps/p25/dsd_symbol.c
    apps/p25/dsd_dibit.c
    apps/p25/dsd_frame_sync.c
    apps/p25/dsd_frame.c
    apps/p25/dsd_filters.c
    apps/p25/dsp_pipeline.c
    apps/p25/p25p1_heuristics.c
    apps/p25/p25p1_hdu.c
    apps/p25/p25p1_ldu.c
    apps/p25/p25p1_ldu1.c
    apps/p25/p25p1_ldu2.c
    apps/p25/p25p1_tdu.c
    apps/p25/p25p1_tdulc.c
    apps/p25/p25_lcw.c
    apps/p25/p25p1_check_nid.c
    apps/p25/diag.c
)
set_source_files_properties(${P25_C_SOURCES}
    PROPERTIES COMPILE_FLAGS
    "-Wno-unused-function -Wno-unused-variable -Wno-incompatible-pointer-types -Wno-format -Wno-int-conversion -Wno-return-type -Wno-implicit-function-declaration"
)

set_source_files_properties(
    apps/p25/p25p1_check_hdu.cpp
    apps/p25/p25p1_check_ldu.cpp
    apps/p25/Hamming.cpp
    PROPERTIES COMPILE_FLAGS
    "-Wno-unused-function -Wno-unused-variable -Wno-register -fno-use-cxa-atexit"
)
