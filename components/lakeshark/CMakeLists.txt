# LakeShark radio/DSP backend, built headless (CONFIG_ENABLE_TUI is left
# unset). The TUI renderers (adsb_render.c, p25_tui.c), ui/, audio_stub and
# core/app_main.c are NOT vendored - main/ owns app_main and the display.
#
# The excluded TUI files were removed when the tree was vendored, so a glob
# over the radio/core/audio/app dirs yields exactly the headless set.

# apps/*/ rather than a list of decoders, and CONFIGURE_DEPENDS so a source
# added after the build directory was configured is actually compiled.
#
# Both halves have bitten. apps/acars arrived with the ACARS decoder and was
# not in the list, and grant_follower.c landed inside apps/p25 - which the
# glob did cover - and still was not built, because a glob without
# CONFIGURE_DEPENDS is evaluated once at configure time. The host tests were
# green either way; it was the device link that failed, on symbols whose .c
# was sitting in the tree.
if(CMAKE_SCRIPT_MODE_FILE)
    # ESP-IDF evaluates this twice and the requirements pass runs in script
    # mode, where CONFIGURE_DEPENDS is a hard error.
    file(GLOB_RECURSE LS_SRCS
        core/*.c radio/*.c audio/*.c output/*.c dsp/*.c nfc/*.c
        apps/*/*.c apps/*/*.cpp)
else()
    file(GLOB_RECURSE LS_SRCS CONFIGURE_DEPENDS
        core/*.c radio/*.c audio/*.c output/*.c dsp/*.c nfc/*.c
        apps/*/*.c apps/*/*.cpp)
endif()

# Every app directory is an include directory too, for the same reason.
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
        # LS-210  ls_crash_esp.c reads back the saved panic backtrace via
        # esp_core_dump_get_summary(). Without the REQUIRES the header
        # (esp_core_dump.h) is invisible to this component and the build
        # dies at compile.
        espcoredump
        # LS-220  ls_version.c calls esp_app_get_description() so the
        # `version` command and the FIRMWARE row in Settings can print the
        # git-derived PROJECT_VER. Without the REQUIRES the header
        # (esp_app_desc.h) is invisible and the build dies at compile.
        esp_app_format
        # LS-271: rec/app_rec.c reads free bytes through esp_vfs_fat_info /
        # esp_spiffs_info since ESP-IDF v5.5.4's picolibc does not ship
        # sys/statvfs.h.  Without these REQUIRES the includes are invisible
        # to the component and the build dies at compile.
        fatfs
        spiffs
        mbelib
        imbe_vocoder
        esp32_p4_wifi6_touch_lcd_4b
        bsp_extra
        lvgl
)

# ---- Per-file flags replicated verbatim from LakeShark/main/CMakeLists.txt.
# The P25 C set is ported-from-C++ dsd code; under GCC 14 several of these
# (-Wno-implicit-function-declaration, -Wno-incompatible-pointer-types,
# -Wno-int-conversion) are required because those diagnostics are errors by
# default. diag.c is included here too (its diag_* symbols are called
# unconditionally; it no-ops at runtime because we never call diag_init()).
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
