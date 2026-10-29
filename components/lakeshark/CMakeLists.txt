
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE LS_SRCS
        board/*.c core/*.c radio/*.c audio/*.c audio/*.cpp output/*.c dsp/*.c nfc/*.c rx/*.c
        apps/*/*.c apps/*/*.cpp apps/*/*.S)
else()
    file(GLOB_RECURSE LS_SRCS CONFIGURE_DEPENDS
        board/*.c core/*.c radio/*.c audio/*.c audio/*.cpp output/*.c dsp/*.c nfc/*.c rx/*.c
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
        rx
        ${LS_APP_INCLUDES}
        nfc
    REQUIRES
        ls_textpack
        mbedtls
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
    "-Wno-unused-function -Wno-unused-variable -Wno-register -fno-use-cxa-atexit -fno-exceptions"
)

# Size build: the radio core stays fast. DSP, decoders, audio and radio I/O
# keep -O2; settings, console and glue are compiled for size.
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    file(GLOB_RECURSE LS_HOT_SRCS
        ${CMAKE_CURRENT_LIST_DIR}/dsp/*.c ${CMAKE_CURRENT_LIST_DIR}/audio/*.c
        ${CMAKE_CURRENT_LIST_DIR}/radio/*.c ${CMAKE_CURRENT_LIST_DIR}/apps/p25/*.c
        ${CMAKE_CURRENT_LIST_DIR}/apps/fm/*.c ${CMAKE_CURRENT_LIST_DIR}/apps/adsb/*.c
        ${CMAKE_CURRENT_LIST_DIR}/apps/dmr/*.c ${CMAKE_CURRENT_LIST_DIR}/apps/acars/*.c
        ${CMAKE_CURRENT_LIST_DIR}/board/ls_panel.c)
    # In those folders but never on a sample or buffer path: settings and
    # profile parsing, stores, SD programs, tuner register setup, health and
    # diagnostics reports. Measured -O2 -> -Os: about 27 KB.
    foreach(f radio/receiver_diagnostics.c radio/radio_health.c radio/iq_mission.c
              radio/rx_plan.c radio/librtlsdr.c radio/tuner_e4k.c radio/tuner_fc0012.c
              radio/tuner_fc0013.c radio/tuner_fc2580.c radio/tuner_r82xx.c
              apps/p25/p25_profile.c apps/p25/p25_program.c apps/p25/p25_program_sd.c
              apps/p25/p25_sites.c apps/p25/p25_geo.c apps/p25/p25_p2_bench.c
              apps/p25/p25_cqpsk_controls.c apps/fm/aprs_store.c apps/fm/ais_store.c
              apps/adsb/uat_weather.c audio/call_store.c
              # control traffic, not samples: the transmit-token broker, P25
              # trunking messages (tens a second), the scan controller and
              # the health snapshot published at UI rate. About 7 KB.
              radio/tx_broker.c apps/p25/p25_tsbk.c apps/p25/scan_ctrl.c
              apps/p25/p25_health.c
              # settings, console commands, NVS and SD stores, retry and
              # policy bookkeeping, the ADS-B demo: run on a command, a key
              # or a timer, never on a sample or a buffer.
              radio/radio_frontend.c radio/tx_policy.c radio/radio_open_retry.c
              radio/usb_boot_retry.c radio/usb_port_cycle.c radio/iq_app_control.c
              apps/fm/same_store.c apps/fm/fm_bcast_cmd.c apps/fm/fm_mode_label.c
              apps/fm/fm_lifecycle.c apps/p25/p25_iq_capture_command.c
              apps/p25/scan_ctrl_nvs.c apps/p25/p25_entry_settings.c
              apps/p25/p25_tg_roster.c apps/p25/p25_pack.c apps/p25/p25_p2_follow.c
              apps/adsb/uat_console.c apps/adsb/adsb_demo.c apps/dmr/dmr_report.c)
        list(REMOVE_ITEM LS_HOT_SRCS ${CMAKE_CURRENT_LIST_DIR}/${f})
    endforeach()
    # -mno-save-restore: the component is built with -msave-restore (root
    # CMakeLists.txt); the hot files keep inline prologues.
    set_source_files_properties(${LS_HOT_SRCS} PROPERTIES COMPILE_OPTIONS "-O2;-mno-save-restore")
endif()
