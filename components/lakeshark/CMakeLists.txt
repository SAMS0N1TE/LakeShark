# LakeShark radio/DSP backend, vendored into the Brookesia phone project and
# built headless (CONFIG_ENABLE_TUI is left unset). The TUI renderers
# (adsb_render.c, p25_tui.c), ui/, audio_stub, and core/app_main.c are NOT
# vendored - Brookesia owns app_main and the display.
#
# The excluded TUI files were removed when the tree was vendored, so a glob
# over the radio/core/audio/app dirs yields exactly the headless set.

file(GLOB_RECURSE LS_SRCS
    core/*.c
    radio/*.c
    audio/*.c
    output/*.c
    apps/adsb/*.c
    apps/p25/*.c
    apps/p25/*.cpp
    apps/fm/*.c
)
list(APPEND LS_SRCS lakeshark_backend.c)

idf_component_register(
    SRCS ${LS_SRCS}
    INCLUDE_DIRS
        .
        core
        radio
        audio
        output
        apps/adsb
        apps/p25
        apps/fm
    REQUIRES
        usb
        driver
        esp_driver_uart
        esp_timer
        nvs_flash
        mbelib
        imbe_vocoder
        esp32_p4_wifi6_touch_lcd_4b
        bsp_extra
)

# ---- Per-file flags replicated verbatim from LakeShark/main/CMakeLists.txt.
# The P25 C set is ported-from-C++ dsd code; under GCC 14 several of these
# (-Wno-implicit-function-declaration, -Wno-incompatible-pointer-types,
# -Wno-int-conversion) are required because those diagnostics are errors by
# default. diag.c is included here too (its diag_* symbols are called
# unconditionally; it no-ops at runtime because we never call diag_init()).
set(P25_C_SOURCES
    apps/p25/scanner.c
    apps/p25/cqpsk_demod.c
    apps/p25/dsd_main.c
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
