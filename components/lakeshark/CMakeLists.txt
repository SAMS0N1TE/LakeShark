
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE LS_SRCS
        board/*.c core/*.c radio/*.c audio/*.c audio/*.cpp output/*.c dsp/*.c nfc/*.c
        apps/*/*.c apps/*/*.cpp apps/*/*.S)
else()
    file(GLOB_RECURSE LS_SRCS CONFIGURE_DEPENDS
        board/*.c core/*.c radio/*.c audio/*.c audio/*.cpp output/*.c dsp/*.c nfc/*.c
        apps/*/*.c apps/*/*.cpp apps/*/*.S)
endif()

# P25 uses IQ only. Retain OS4 research sources for explicit host benches;
# their learned classifiers and assembly model do not belong in firmware.
list(FILTER LS_SRCS EXCLUDE REGEX "/apps/p25/p25_os4_(decode|lut|nn|nn_model|nn_pie)\\.(c|S)$")

# LakeShark LR: no USB host, dongle drivers, SX1262 or dongle-only apps.
# lr_only_stubs.c answers for them with "not present".
if(CONFIG_LS_LR2021_ONLY)
    list(FILTER LS_SRCS EXCLUDE REGEX "/radio/(librtlsdr|rtlsdr_dev|tuner_[a-z0-9]+|hackrf_dev|hackrf_radio|esp_libusb|usb_host|usb_boot_retry|usb_port_cycle)\.c$")
    list(FILTER LS_SRCS EXCLUDE REGEX "/board/ls_lora_sx126x\.c$")
    list(FILTER LS_SRCS EXCLUDE REGEX "/apps/cell/")
    list(FILTER LS_SRCS EXCLUDE REGEX "/apps/rec/rec_catch[a-z_]*\.c$")
else()
    list(FILTER LS_SRCS EXCLUDE REGEX "/lr_only_stubs\.c$")
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
        p25_phase2
        imbe_vocoder
        esp32_p4_wifi6_touch_lcd_4b
        bsp_extra
        esp_codec_dev
        esp_lcd
        esp_lvgl_port
        lvgl
        klatt_tts
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
# Format checking back on for this component, against the project-wide
# -Wno-format in the root CMakeLists. That flag exists for ESP-IDF and the
# vendored trees, not for this code. The vendored P25 sources below keep their
# own -Wno-format: per-source options are applied after per-target ones, so
# they still win.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wformat -Wno-format-truncation)

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
