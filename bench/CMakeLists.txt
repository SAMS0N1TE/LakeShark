cmake_minimum_required(VERSION 3.18)
project(lakeshark_bench C CXX)

add_compile_definitions(LV_FONT_MONTSERRAT_14=1 LV_FONT_MONTSERRAT_16=1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FW  ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(APP ${FW}/components/lakeshark/apps)

set(P2 ${FW}/components/p25_phase2)
file(GLOB P2_SOURCES ${P2}/upstream/*.c ${P2}/upstream/*.cc)
add_library(p25_phase2_host STATIC ${P2}/p25_phase2.cpp ${P2_SOURCES})
target_include_directories(p25_phase2_host PUBLIC ${P2} PRIVATE ${P2}/upstream)
target_compile_options(p25_phase2_host PRIVATE -O2 $<$<COMPILE_LANGUAGE:C>:-fsingle-precision-constant>)
add_executable(p25p2_replay tools/p25p2_replay.cpp)
target_link_libraries(p25p2_replay p25_phase2_host m)

set(CARTO ${FW}/components/libcarto/upstream)
set(PMT   ${FW}/components/pmtiles)

set(LS_TUI_APP_SRCS
    ${FW}/bench/tools/lssim_p25_settings.c
    ${FW}/components/apps/tui/ls_radio_panel.c
    ${FW}/bench/tools/lssim_scan.c
    ${FW}/bench/tools/lssim_watch.c
    ${APP}/subghz/subghz_file.c
    ${APP}/rec/rec_ook24.c
    ${FW}/components/apps/tui/ls_compass.c
    ${FW}/components/apps/tui/ls_app.c
    ${FW}/components/apps/tui/ls_app_docs.c
    ${FW}/components/apps/tui/ls_anim.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_skyplot.c
    ${FW}/components/apps/tui/ls_skyview.c
    ${FW}/components/apps/tui/ls_text.c
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_quick.c
    ${FW}/components/apps/tui/ls_numpad.c
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_notify.c
    ${FW}/components/apps/tui/ls_geo.c
    ${FW}/components/apps/tui/ls_picker.c
    ${FW}/components/apps/tui/ls_action.c
    ${FW}/components/apps/tui/ls_value.c
    ${FW}/components/apps/tui/ls_font_list.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/bench/shims/tui_hw_stubs.c)

set(LS_TUI_APP_INCLUDES
    ${FW}/components/lakeshark/core
    ${FW}/components/lakeshark/radio
    ${FW}/components/lakeshark/nfc
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/board
    ${FW}/components/meshcore
    ${FW}/bench/shims)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -g -O1)

if(LS_SANITIZE)
    include(CheckLinkerFlag)
    set(_san_compile "")
    set(_san_link    "")
    set(_san_active  "")

    check_linker_flag(C "-fsanitize=address" LS_HAS_ASAN_RT)
    if(LS_HAS_ASAN_RT)
        list(APPEND _san_compile "-fsanitize=address")
        list(APPEND _san_link    "-fsanitize=address")
        list(APPEND _san_active  "address")
    endif()

    check_linker_flag(C "-fsanitize=undefined" LS_HAS_UBSAN_RT)
    if(LS_HAS_UBSAN_RT)
        list(APPEND _san_compile "-fsanitize=undefined")
        list(APPEND _san_link    "-fsanitize=undefined")
        list(APPEND _san_active  "undefined")
    else()
        check_linker_flag(C "-fsanitize=undefined;-fsanitize-trap=undefined"
                          LS_HAS_UBSAN_TRAP)
        if(LS_HAS_UBSAN_TRAP)
            list(APPEND _san_compile
                "-fsanitize=undefined" "-fsanitize-trap=undefined")
            list(APPEND _san_link
                "-fsanitize=undefined" "-fsanitize-trap=undefined")
            list(APPEND _san_active "undefined(trap)")
        endif()
    endif()

    if(NOT _san_compile)
        message(FATAL_ERROR
            "LS_SANITIZE=ON but no working sanitizer flag found for this "
            "toolchain.  Install a gcc/clang that ships libasan+libubsan, "
            "or drop -Sanitize.")
    endif()

    add_compile_options(${_san_compile} -fno-omit-frame-pointer)
    add_link_options(${_san_link})

    string(REPLACE ";" "," _san_active_csv "${_san_active}")
    message(STATUS "LS_SANITIZE active: ${_san_active_csv}")
    if(NOT LS_HAS_ASAN_RT)
        message(WARNING
            "libasan not available on this host - sanitizer gate is "
            "running without AddressSanitizer.  This is expected on the "
            "stock MinGW/GCC 14.x distributions for Windows, which do "
            "not ship the sanitizer runtime archives.")
    endif()
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/shims
    ${CMAKE_CURRENT_SOURCE_DIR}/harness
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    ${APP}/fm
    ${APP}/p25
    ${APP}/rec
    ${APP}/subghz
    ${APP}/adsb
    ${APP}/fm
    ${FW}/components/lakeshark/radio
    ${FW}/components/lakeshark/dsp
    ${FW}/components/lakeshark/board
    ${FW}/components/mbelib
    ${FW}/main
    ${FW}/components/apps
    ${FW}/components/apps/media_gui
    ${FW}/components/apps
    ${FW}/components/apps/file_browser
    ${FW}/components/bsp_extra/include
    ${FW}/managed_components/chmorgan__esp-file-iterator/include
)

add_library(ls_harness STATIC
    harness/ls_test.c
    harness/shim_impl.c
    shims/freertos_shim.c
    shims/spi_shim.c
    fixtures/pocsag_gen.c
    fixtures/dmr_gen.c
    ${FW}/components/lakeshark/core/ls_time.c
)
target_include_directories(ls_harness PUBLIC
    ${APP}/dmr
    ${APP}/subghz
    ${FW}/components/lakeshark/core
)
target_compile_definitions(ls_harness PUBLIC LS_TIME_HOST=1)

find_package(Threads REQUIRED)
target_link_libraries(ls_harness PUBLIC Threads::Threads)

enable_testing()

add_subdirectory(${FW}/managed_components/lvgl__lvgl
                 ${CMAKE_BINARY_DIR}/lvgl-host EXCLUDE_FROM_ALL)
target_compile_definitions(lvgl PUBLIC LV_CONF_SKIP=1 LV_MEM_CUSTOM=1)
target_compile_definitions(lvgl PUBLIC
    LV_MEM_CUSTOM_INCLUDE=\"ls_lv_mem_probe.h\"
    LV_MEM_CUSTOM_ALLOC=ls_lv_probe_malloc
    LV_MEM_CUSTOM_FREE=ls_lv_probe_free
    LV_MEM_CUSTOM_REALLOC=ls_lv_probe_realloc)
add_library(ls_lv_probe STATIC shims/ls_lv_mem_probe.c)
target_include_directories(ls_lv_probe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/shims)
target_link_libraries(lvgl PUBLIC ls_lv_probe)
target_compile_options(lvgl PRIVATE -w)

add_executable(test_ls_ui_layout
    tests/test_ls_ui_layout.cpp
    ${FW}/components/apps/ui/ls_ui.cpp
    ${FW}/components/apps/ui/ls_ui_palette.c
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/ui/ls_safe_screen_hint.c)
target_link_libraries(test_ls_ui_layout ls_harness lvgl m)
add_test(NAME test_ls_ui_layout COMMAND test_ls_ui_layout)

add_executable(test_p25_screen_fit
    tests/test_p25_screen_fit.cpp
    ${FW}/components/apps/ui/ls_safe_screen_hint.c
    ${FW}/components/apps/ui/ls_ui.cpp
    ${FW}/components/apps/ui/ls_ui_palette.c
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/ui/ls_spectrum_waterfall.cpp
    ${FW}/components/apps/ui/ls_spectrum_waterfall_logic.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c)
target_include_directories(test_p25_screen_fit PRIVATE
    ${FW}/components/apps/p25_gui)
target_link_libraries(test_p25_screen_fit ls_harness lvgl m)
add_test(NAME test_p25_screen_fit COMMAND test_p25_screen_fit)

add_executable(test_acars_screen_fit
    tests/test_acars_screen_fit.cpp
    ${FW}/components/apps/acars_gui/AppACARS.cpp
    ${APP}/acars/acars_app.c
    ${FW}/components/apps/sdr_ui/sdr_ui.cpp
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c
    ${FW}/components/apps/ui/ls_ui.cpp
    ${FW}/components/apps/ui/ls_ui_palette.c
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/ui/ls_safe_screen_hint.c
    ${FW}/components/apps/ui/ls_receiver_status.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c
    ${FW}/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_48.c)
target_compile_definitions(test_acars_screen_fit PRIVATE
    LV_FONT_MONTSERRAT_16=1 LV_FONT_MONTSERRAT_48=1)
target_include_directories(test_acars_screen_fit PRIVATE
    ${APP}/acars ${FW}/components/lakeshark)
target_link_libraries(test_acars_screen_fit ls_harness lvgl m)
add_test(NAME test_acars_screen_fit COMMAND test_acars_screen_fit)

add_executable(test_home_widget
    tests/test_home_widget.cpp
    ${FW}/components/apps/home/AppHome.cpp
    ${FW}/components/apps/home/home_widget_view.c
    ${FW}/components/lakeshark/core/home_widget_pref.c
    ${FW}/components/apps/sdr_ui/sdr_ui.cpp
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c
    ${FW}/components/apps/ui/ls_ui.cpp
    ${FW}/components/apps/ui/ls_ui_palette.c
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/ui/ls_safe_screen_hint.c
    ${FW}/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_20.c
    ${FW}/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_32.c)
target_compile_definitions(test_home_widget PRIVATE
    LV_FONT_MONTSERRAT_20=1 LV_FONT_MONTSERRAT_32=1)
target_include_directories(test_home_widget PRIVATE ${FW}/components/lakeshark)
target_link_libraries(test_home_widget ls_harness lvgl m)
add_test(NAME test_home_widget COMMAND test_home_widget)

add_executable(test_text_entry_theme
    tests/test_text_entry_theme.cpp
    ${FW}/components/apps/shell/ls_text_entry.cpp
    ${FW}/components/apps/sdr_ui/sdr_ui.cpp
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c
    ${FW}/components/apps/ui/ls_ui.cpp
    ${FW}/components/apps/ui/ls_ui_palette.c
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/ui/ls_safe_screen_hint.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c
    ${FW}/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_24.c
    ${FW}/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_32.c)
target_compile_definitions(test_text_entry_theme PRIVATE
    LV_FONT_MONTSERRAT_16=1 LV_FONT_MONTSERRAT_24=1 LV_FONT_MONTSERRAT_32=1)
target_link_libraries(test_text_entry_theme ls_harness lvgl m)
add_test(NAME test_text_entry_theme COMMAND test_text_entry_theme)

add_executable(test_instrument
    tests/test_instrument.cpp
    ${FW}/components/apps/home/AppHome.cpp
    ${FW}/components/apps/home/home_widget_view.c
    ${FW}/components/apps/ui/ls_instrument.cpp
    ${FW}/components/apps/sdr_ui/sdr_ui.cpp
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c
    ${FW}/components/apps/ui/ls_ui.cpp
    ${FW}/components/apps/ui/ls_ui_palette.c
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/ui/ls_safe_screen_hint.c
    ${FW}/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_32.c)
target_sources(test_instrument PRIVATE ${FW}/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_20.c)
target_compile_definitions(test_instrument PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1 LV_FONT_MONTSERRAT_20=1 LV_FONT_MONTSERRAT_32=1)
target_include_directories(test_instrument PRIVATE ${FW}/components/lakeshark)
target_link_libraries(test_instrument ls_harness lvgl m)
add_test(NAME test_instrument COMMAND test_instrument)

add_executable(test_lvgl_psram_allocator
    tests/test_lvgl_psram_allocator.c
    ${FW}/managed_components/lvgl__lvgl/src/misc/lv_mem.c
    ${FW}/managed_components/lvgl__lvgl/src/misc/lv_gc.c)
target_include_directories(test_lvgl_psram_allocator PRIVATE
    ${FW}/managed_components/lvgl__lvgl)
target_compile_definitions(test_lvgl_psram_allocator PRIVATE
    LV_CONF_SKIP=1 LV_MEM_CUSTOM=1
    LV_MEM_CUSTOM_INCLUDE=\"ls_lvgl_heap_test.h\")
include(${FW}/main/ls_lvgl_allocator.cmake)
ls_configure_lvgl_allocator(test_lvgl_psram_allocator)
target_link_libraries(test_lvgl_psram_allocator ls_harness m)
add_test(NAME test_lvgl_psram_allocator COMMAND test_lvgl_psram_allocator)

add_executable(test_ls_ui_alloc
    tests/test_ls_ui_alloc.cpp
    ${FW}/components/apps/ui/ls_ui.cpp
    ${FW}/components/apps/ui/ls_ui_palette.c
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/ui/ls_safe_screen_hint.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_16.c
    ${FW}/components/apps/sdr_ui/lv_font_lsmono_14.c)
target_link_libraries(test_ls_ui_alloc ls_harness lvgl m)
add_test(NAME test_ls_ui_alloc COMMAND test_ls_ui_alloc)

function(ls_add_test name)
    add_executable(${name} tests/${name}.c ${ARGN})
    target_link_libraries(${name} ls_harness m)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ls_add_test(test_tui_surface ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_tui_surface PRIVATE
    ${FW}/components/apps/tui/tuilib)

# Optional private tests, if this tree has any. The file names what it
# registers; this line does not, so it is safe to publish either way.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/private.cmake)
    include(${CMAKE_CURRENT_SOURCE_DIR}/private.cmake)
endif()

ls_add_test(test_fsk_capture
    ${FW}/components/lakeshark/radio/ls_fsk_capture.c
    ${FW}/components/lakeshark/radio/ls_sub_fsk.c)
target_include_directories(test_fsk_capture PRIVATE
    ${FW}/components/lakeshark/radio)

ls_add_test(test_display_dim)
ls_add_test(test_audio_pcm_ring)
target_include_directories(test_audio_pcm_ring PRIVATE ${FW}/components/lakeshark/audio)
target_include_directories(test_display_dim PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_tui_chrome
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_text.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_tui_chrome PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib)

ls_add_test(test_tui_screens
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    tools/lssim_field.c
    ${FW}/components/apps/tui/screens/ext/scr_labs.c
    ${FW}/components/apps/tui/screens/ext/scr_subghz.c
    ${FW}/components/apps/tui/screens/ext/scr_mixrf.c
    ${FW}/components/apps/tui/screens/ext/scr_nfc.c
    ${FW}/components/lakeshark/nfc/ls_nfc_suite.c
    ${FW}/components/lakeshark/nfc/nfc_classic.c
    ${FW}/components/apps/tui/screens/ext/scr_journal.c
    ${FW}/components/apps/tui/screens/ext/scr_settings.c
    ${FW}/components/apps/tui/screens/ext/scr_diag.c
    ${FW}/components/lakeshark/core/ls_vitals.c
    ${FW}/components/lakeshark/core/ls_trend.c
    ${FW}/components/lakeshark/core/ls_version.c
    ${FW}/components/lakeshark/core/ls_safe_mode.c
    ${FW}/components/lakeshark/board/ls_haptic.c
    ${FW}/components/apps/tui/screens/ext/scr_rec.c
    ${FW}/components/apps/tui/screens/ext/scr_falls.c
    ${FW}/components/apps/tui/screens/main/scr_home.c
    ${FW}/components/apps/tui/screens/main/scr_fm.c
    ${FW}/components/apps/tui/screens/main/scr_adsb.c
    ${APP}/fm/fm_mode_label.c
    ${APP}/adsb/adsb_state.c
    ${APP}/p25/p25_health.c
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/lakeshark/radio/radio_health.c)
target_include_directories(test_tui_screens PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/core
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/radio
    ${APP}/p25
    ${APP}/rec
    ${APP}/subghz
    ${FW}/bench/shims)

ls_add_test(test_tui_router
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_tui_router PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/bench/shims)

ls_add_test(test_trend
    ${FW}/components/lakeshark/core/ls_trend.c)
target_include_directories(test_trend PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_text
    ${FW}/components/apps/tui/ls_text.c)
target_include_directories(test_text PRIVATE
    ${FW}/components/apps/tui)

ls_add_test(test_tui_p25_screen
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/screens/main/scr_p25.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    ${APP}/p25/p25_tg_observed.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_tui_p25_screen PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark
    ${APP}/p25
    ${FW}/bench/shims)

ls_add_test(test_tui_waterfall_claim
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_waterfall_claim PRIVATE
    ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_waterfall_scale
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_waterfall_scale PRIVATE
    ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_waterfall_stall
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_waterfall_stall PRIVATE
    ${LS_TUI_APP_INCLUDES})

ls_add_test(test_wf_source ${FW}/components/apps/tui/ls_wf_source.c)
target_include_directories(test_wf_source PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/core
    ${FW}/components/lakeshark/apps
    ${APP}/p25)

ls_add_test(test_tui_picker
    ${FW}/components/apps/tui/ls_picker.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_picker PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_keyboard
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_keyboard PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_wireless
    ${FW}/components/apps/tui/screens/ext/scr_wireless.c
    ${FW}/components/apps/tui/ls_wireless_history.c
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_wireless PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_quick
    ${FW}/components/apps/tui/ls_quick.c
    ${FW}/components/apps/tui/ls_action.c
    ${FW}/components/apps/tui/ls_value.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_quick PRIVATE ${LS_TUI_APP_INCLUDES})

add_executable(lssim
    ${FW}/components/apps/tui/ls_splash.c
    ${FW}/components/apps/tui/ls_wordmark.c
    tools/lssim.c
    tools/lssim_radio_preview.c
    tools/lssim_field.c
    ${FW}/components/apps/tui/screens/ext/scr_labs.c
    ${FW}/components/apps/tui/screens/ext/scr_subghz.c
    ${FW}/components/apps/tui/screens/ext/scr_mixrf.c
    ${FW}/components/apps/tui/screens/ext/scr_nfc.c
    ${FW}/components/lakeshark/nfc/ls_nfc_suite.c
    ${FW}/components/lakeshark/nfc/nfc_classic.c
    ${FW}/components/apps/tui/screens/ext/scr_journal.c
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/ls_tui.c
    ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/screens/main/scr_home.c
    ${FW}/components/apps/tui/screens/main/scr_p25.c
    ${APP}/p25/p25_tg_observed.c
    ${FW}/components/apps/tui/screens/main/scr_fm.c
    ${FW}/components/apps/tui/screens/main/scr_adsb.c
    ${FW}/components/apps/tui/screens/ext/scr_rec.c
    ${FW}/components/apps/tui/screens/ext/scr_diag.c
    ${FW}/components/lakeshark/core/ls_vitals.c
    ${FW}/components/lakeshark/core/ls_trend.c
    ${FW}/components/lakeshark/core/ls_version.c
    ${FW}/components/lakeshark/core/ls_safe_mode.c
    ${FW}/components/lakeshark/board/ls_haptic.c
    ${FW}/components/apps/tui/screens/ext/scr_settings.c
    ${FW}/components/apps/tui/screens/ext/scr_gps.c
    ${FW}/components/apps/tui/ls_map_motion.c
    ${FW}/components/apps/tui/screens/ext/scr_map.c
    ${FW}/components/apps/tui/ls_map.c
    ${FW}/components/apps/tui/ls_stroke.c
    ${FW}/components/apps/tui/screens/ext/scr_falls.c
    ${FW}/components/apps/tui/ls_wf_source.c
    ${FW}/components/apps/tui/screens/ext/scr_mesh.c
    ${FW}/components/apps/tui/screens/ext/scr_radios.c
    tools/lssim_mesh.c
    ${PMT}/upstream/zeromesh_pmtiles.c
    ${CARTO}/src/carto.c
    ${CARTO}/src/framebuffer.c
    ${CARTO}/src/geom.c
    ${CARTO}/src/mvt.c
    ${CARTO}/src/raster.c
    ${CARTO}/src/style.c
    ${FW}/components/apps/tui/ls_value_builtin.c
    ${FW}/components/apps/tui/ls_action_builtin.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${APP}/fm/fm_mode_label.c
    ${APP}/adsb/adsb_state.c
    ${APP}/p25/p25_health.c
    ${FW}/components/lakeshark/core/perf.c
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/lakeshark/radio/radio_health.c
    tools/lssim_state.c)
target_include_directories(lssim PRIVATE
    ${FW}/components/meshcore
    ${LS_TUI_APP_INCLUDES}
    ${PMT}/upstream
    ${PMT}/compat
    ${CARTO}/include
    ${CARTO}/src
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/board
    ${FW}/components/lakeshark/core
    ${FW}/components/lakeshark/apps
    ${FW}/components/meshcore
    ${APP}/p25
    ${APP}/rec
    ${APP}/subghz
    ${FW}/bench/harness
    ${FW}/bench/shims)
target_sources(lssim PRIVATE
    tools/lssim_i2c.c
    harness/shim_impl.c
    shims/freertos_shim.c)
target_compile_definitions(lssim PRIVATE
    CONFIG_LS_BOARD_T_DISPLAY_P4=1
    LS_WF_SOURCE_REAL
    LSSIM_PMTILES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/carto/franklin_z12.pmtiles")
target_sources(lssim PRIVATE ${FW}/components/lakeshark/core/ls_time.c)
target_compile_definitions(lssim PRIVATE LS_TIME_HOST=1)
target_link_libraries(lssim lvgl m)

ls_add_test(test_carto_render
    ${CARTO}/src/carto.c
    ${CARTO}/src/framebuffer.c
    ${CARTO}/src/geom.c
    ${CARTO}/src/mvt.c
    ${CARTO}/src/raster.c
    ${CARTO}/src/style.c)
target_include_directories(test_carto_render PRIVATE
    ${CARTO}/include
    ${CARTO}/src)
target_compile_definitions(test_carto_render PRIVATE
    CARTO_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/carto/sample.mvt")
target_link_libraries(test_carto_render m)

ls_add_test(test_pmtiles
    ${PMT}/upstream/zeromesh_pmtiles.c
    ${CARTO}/src/carto.c
    ${CARTO}/src/framebuffer.c
    ${CARTO}/src/geom.c
    ${CARTO}/src/mvt.c
    ${CARTO}/src/raster.c
    ${CARTO}/src/style.c)
target_include_directories(test_pmtiles PRIVATE
    ${PMT}/upstream
    ${PMT}/compat
    ${CARTO}/include
    ${CARTO}/src
    ${FW}/bench/shims)
target_compile_definitions(test_pmtiles PRIVATE
    PMTILES_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/carto/franklin_z12.pmtiles")
target_link_libraries(test_pmtiles m)

ls_add_test(test_map
    ${FW}/components/apps/tui/ls_map.c
    ${PMT}/upstream/zeromesh_pmtiles.c
    ${CARTO}/src/carto.c
    ${CARTO}/src/framebuffer.c
    ${CARTO}/src/geom.c
    ${CARTO}/src/mvt.c
    ${CARTO}/src/raster.c
    ${CARTO}/src/style.c)
target_include_directories(test_map PRIVATE
    ${FW}/components/apps/tui
    ${PMT}/upstream
    ${PMT}/compat
    ${CARTO}/include
    ${CARTO}/src
    ${FW}/bench/shims)
target_compile_definitions(test_map PRIVATE
    PMTILES_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/carto/franklin_z12.pmtiles")
target_link_libraries(test_map m)

ls_add_test(test_tui_transform
    ${FW}/components/apps/tui/ls_tui.c
    ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_font_list.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_tui_transform PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_link_libraries(test_tui_transform lvgl)

ls_add_test(test_tui_touch ${FW}/components/apps/tui/ls_tui_touch.c)
target_include_directories(test_tui_touch PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)

ls_add_test(test_tui_button_centre
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_tui_button_centre PRIVATE
    ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_theme ${FW}/components/apps/tui/ls_theme.c)
target_include_directories(test_tui_theme PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_tui_font
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c)
target_include_directories(test_tui_font PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_board_keymap ${FW}/components/lakeshark/board/ls_keymap.c)
target_include_directories(test_board_keymap PRIVATE
    ${FW}/components/lakeshark/board)

ls_add_test(test_tui_render_cost
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/ls_tui.c
    ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/screens/main/scr_p25.c
    ${APP}/p25/p25_tg_observed.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_tui_render_cost PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/board
    ${APP}/p25
    ${FW}/bench/shims)
target_link_libraries(test_tui_render_cost lvgl)

ls_add_test(test_lora_sx1262
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lora_sx1262 PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_lora_sx1262 PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_lora_scan
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lora_scan PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)

ls_add_test(test_xl9535 ${FW}/components/lakeshark/board/ls_xl9535.c)
target_include_directories(test_xl9535 PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_xl9535 PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_track ${FW}/components/lakeshark/core/ls_track.c)
target_include_directories(test_track PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_rlog ${FW}/components/lakeshark/core/ls_rlog.c)
target_include_directories(test_rlog PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_wordmark
    ${FW}/components/apps/tui/ls_wordmark.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(test_wordmark PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib)

ls_add_test(test_stroke ${FW}/components/apps/tui/ls_stroke.c)
target_include_directories(test_stroke PRIVATE
    ${FW}/components/apps/tui)

ls_add_test(test_skyplot ${FW}/components/apps/tui/ls_skyplot.c)
target_include_directories(test_skyplot PRIVATE
    ${FW}/components/apps/tui)

ls_add_test(test_tui_inset ${FW}/components/apps/tui/ls_tui_inset.c)
target_include_directories(test_tui_inset PRIVATE
    ${FW}/components/apps/tui)

ls_add_test(test_haptic ${FW}/components/lakeshark/board/ls_haptic.c)
target_include_directories(test_haptic PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_haptic PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_gps_nmea ${FW}/components/lakeshark/board/ls_gps.c)
target_include_directories(test_gps_nmea PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_gps_nmea PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_adsb_state ${APP}/adsb/adsb_state.c)
target_include_directories(test_adsb_state PRIVATE ${APP}/adsb ${FW}/bench/shims)

ls_add_test(test_adsb_phantom
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c)
target_include_directories(test_adsb_phantom PRIVATE
    ${APP}/adsb
    ${FW}/components/lakeshark/audio
    ${FW}/bench/shims)

ls_add_test(test_adsb_cpr
    ${APP}/adsb/adsb_demo.c
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c)
target_include_directories(test_adsb_cpr PRIVATE
    ${APP}/adsb
    ${FW}/components/lakeshark/audio
    ${FW}/bench/shims)

ls_add_test(test_tui_render_dump
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/ls_tui.c
    ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/screens/main/scr_p25.c
    ${APP}/p25/p25_tg_observed.c
    ${FW}/components/apps/tui/screens/main/scr_home.c
    ${FW}/components/apps/tui/screens/ext/scr_settings.c
    ${FW}/components/apps/tui/screens/ext/scr_rec.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_sources(test_tui_render_dump PRIVATE ${FW}/components/apps/tui/screens/ext/scr_subghz.c)
target_sources(test_tui_render_dump PRIVATE tools/lssim_field.c)
target_sources(test_tui_p25_screen PRIVATE tools/lssim_field.c)
target_sources(test_tui_render_cost PRIVATE tools/lssim_field.c)
target_include_directories(test_tui_render_dump PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/board
    ${FW}/components/lakeshark/core
    ${APP}/p25
    ${FW}/bench/shims)
target_link_libraries(test_tui_render_dump lvgl)
target_compile_definitions(test_tui_render_dump PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_pocsag  ${APP}/fm/pocsag.c)
ls_add_test(test_fm_mode_label ${APP}/fm/fm_mode_label.c)
ls_add_test(test_fm_mode_handoff ${APP}/fm/fm_mode_handoff.c)
ls_add_test(test_flipper_fm_command
    ${FW}/main/flipper_link.c
    ${APP}/fm/fm_mode_label.c)
target_compile_options(test_flipper_fm_command PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/shims/flipper_link_host.h)
target_include_directories(test_flipper_fm_command PRIVATE
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/audio
    ${FW}/components/meshcore
    ${FW}/components/lakeshark/board
    ${FW}/components/lakeshark/core)
ls_add_test(test_fm_lifecycle ${APP}/fm/fm_lifecycle.c)
ls_add_test(test_fm_rx_worker
    ${APP}/fm/fm_rx_worker.c
    ${FW}/components/lakeshark/core/radio_decode_worker.c)
ls_add_test(test_flex ${APP}/fm/flex.c fixtures/flex_gen.c)
ls_add_test(test_rec_storage_label ${APP}/rec/rec_storage_label.c)
ls_add_test(test_rec_unique_name ${APP}/rec/rec_unique_name.c)
ls_add_test(test_rec_list_format ${APP}/rec/rec_list_format.c)
ls_add_test(test_rec_sidecar     ${APP}/rec/rec_sidecar.c)
ls_add_test(test_tdp4_power ${FW}/components/lakeshark/board/ls_board_hw.c)
target_compile_definitions(test_tdp4_power PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
ls_add_test(test_hosted_board ${FW}/main/ls_hosted_board.c)
target_compile_definitions(test_hosted_board PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
target_include_directories(test_hosted_board PRIVATE
    ${FW}/main ${FW}/bench/shims/hosted_gpio
    ${FW}/managed_components/espressif__esp_hosted/host)
ls_add_test(test_rec_scout_span  ${APP}/rec/rec_scout_span.c)
ls_add_test(test_rec_space       ${APP}/rec/rec_space.c)
# The export writes a 2-FSK preset for a demodulated capture, so it needs
# the CC1101 register arithmetic that builds one.
ls_add_test(test_rec_watch ${APP}/rec/rec_watch.c ${APP}/rec/rec_watch_store.c ${APP}/rec/rec_ook24.c
    ${FW}/components/lakeshark/radio/ls_sub_fsk.c
    ${FW}/components/lakeshark/radio/ls_fsk_capture.c)
target_include_directories(test_rec_watch PRIVATE ${FW}/components/lakeshark/radio)
ls_add_test(test_screenshot_plan ${FW}/main/screenshot_plan.c)
ls_add_test(test_p25_dsp_modes
    ${APP}/p25/dsp_pipeline.c
    ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/p25_demod_mode.c)
ls_add_test(test_p25_dsp_cost ${APP}/p25/dsp_pipeline.c
    ${APP}/p25/p25_cqpsk_controls.c)
ls_add_test(test_p25_cqpsk_controls
    ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/dsp_pipeline.c)
ls_add_test(test_p25_demod_control
    ${APP}/p25/p25_demod_control.c
    ${APP}/p25/p25_qual.c)
ls_add_test(test_p25_cqpsk_baseline
    ${APP}/p25/dsp_pipeline.c
    ${APP}/p25/dsd_symbol.c
    ${APP}/p25/dsd_frame_sync.c
    ${APP}/p25/dsd_filters.c
    ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/p25_tsbk.c
    fixtures/lsm_gen.c)
ls_add_test(test_p25_nid)
ls_add_test(test_p25_iq_capture ${APP}/p25/p25_iq_capture_command.c)
ls_add_test(test_p25_acquisition
    ${APP}/p25/p25_acquisition.c
    ${APP}/p25/p25_demod_control.c
    ${APP}/p25/p25_qual.c
    ${APP}/p25/dsp_pipeline.c
    ${APP}/p25/dsd_symbol.c
    ${APP}/p25/dsd_frame_sync.c
    ${APP}/p25/dsd_filters.c
    ${APP}/p25/dsd_main.c
    ${APP}/p25/dsd_dibit.c
    ${APP}/p25/dsd_frame.c
    ${APP}/p25/p25p1_heuristics.c
    ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/p25_tsbk.c
    ${APP}/p25/p25_ess.c
    ${APP}/p25/p25_lcw.c
    ${FW}/components/mbelib/mbelib.c
    fixtures/lsm_gen.c)
target_include_directories(test_p25_acquisition PRIVATE
    ${FW}/components/imbe_vocoder)
target_compile_options(test_p25_acquisition PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_p25_acquisition PRIVATE -Wl,--gc-sections)
get_target_property(_p25_replay_sources test_p25_acquisition SOURCES)
list(REMOVE_AT _p25_replay_sources 0)
add_executable(p25_iq_replay tools/p25_iq_replay.c ${_p25_replay_sources})
target_link_libraries(p25_iq_replay ls_harness m)
target_include_directories(p25_iq_replay PRIVATE ${FW}/components/imbe_vocoder)
target_compile_options(p25_iq_replay PRIVATE -ffunction-sections -fdata-sections)
target_link_options(p25_iq_replay PRIVATE -Wl,--gc-sections)
ls_add_test(test_p25_tsdu ${APP}/p25/dsd_frame.c ${APP}/p25/p25_tsbk.c
    ${APP}/p25/p25_ess.c ${APP}/p25/p25_lcw.c)
ls_add_test(test_p25_tsbk ${APP}/p25/p25_tsbk.c
    ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c
    ${APP}/p25/p25_controls.c)
ls_add_test(test_p25_ess ${APP}/p25/p25_ess.c)
ls_add_test(test_p25_voice_hold ${APP}/p25/p25_voice_hold.c)
ls_add_test(test_p25_grant ${APP}/p25/p25_tsbk.c ${APP}/p25/grant_follower.c
    ${APP}/p25/p25_ess.c ${APP}/p25/p25_controls.c)
ls_add_test(test_p25_receive ${APP}/p25/dsd_frame.c ${APP}/p25/p25_tsbk.c
    ${APP}/p25/grant_follower.c ${APP}/p25/scan_ctrl.c
    ${APP}/p25/p25_ess.c ${APP}/p25/p25_lcw.c ${APP}/p25/p25_controls.c)
ls_add_test(test_p25_scan_ctrl ${APP}/p25/scan_ctrl.c
    ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c
    ${APP}/p25/p25_controls.c)
ls_add_test(test_p25_geo ${APP}/p25/p25_geo.c)
# The route preset is operator-supplied data. The test runs once it is there.
set(LS_ROUTE_PROFILE ${FW}/tools/p25_profile_franklin_nh_to_west_dennis_ma.example)
if(EXISTS ${LS_ROUTE_PROFILE})
    ls_add_test(test_p25_route_profile ${APP}/p25/p25_profile.c
                ${APP}/p25/p25_cqpsk_controls.c ${APP}/p25/p25_geo.c)
    target_compile_definitions(test_p25_route_profile PRIVATE
        P25_ROUTE_PROFILE_PATH="${LS_ROUTE_PROFILE}")
else()
    message(STATUS "route preset absent: test_p25_route_profile not built")
endif()
ls_add_test(test_p25_profile ${APP}/p25/p25_profile.c
    ${APP}/p25/p25_cqpsk_controls.c)
target_compile_definitions(test_p25_profile PRIVATE
    P25_PROFILE_EXAMPLE_PATH="${FW}/tools/p25_profile.example")
ls_add_test(test_p25_program ${APP}/p25/p25_program.c ${APP}/p25/p25_profile.c
    ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/scan_ctrl.c ${APP}/p25/grant_follower.c
    ${APP}/p25/p25_controls.c ${APP}/p25/p25_ess.c)
ls_add_test(test_p25_scan_feedback
    ${FW}/components/lakeshark/core/scan_feedback.c
    ${APP}/p25/p25_tune_policy.c)
ls_add_test(test_p25_tg_roster ${APP}/p25/p25_tg_roster.c
    ${APP}/p25/scan_ctrl.c ${APP}/p25/grant_follower.c
    ${APP}/p25/p25_controls.c ${APP}/p25/p25_ess.c)
ls_add_test(test_p25_controls ${APP}/p25/p25_controls.c
    ${APP}/p25/scan_ctrl.c ${APP}/p25/grant_follower.c
    ${APP}/p25/p25_ess.c)
ls_add_test(test_p25_spectrum
    ${APP}/p25/p25_spectrum.c
    ${FW}/components/lakeshark/dsp/spectrum.c)
ls_add_test(test_fm_spectrum
    ${APP}/fm/fm_spectrum.c
    ${FW}/components/lakeshark/dsp/spectrum.c)
ls_add_test(test_p25_health ${APP}/p25/p25_health.c)
ls_add_test(test_p25_diag_disabled ${APP}/p25/diag.c)
target_compile_definitions(test_p25_diag_disabled PRIVATE
    diag_init=p25_diag_test_init
    diag_line=p25_diag_test_line
    diag_vline=p25_diag_test_vline
    diag_emit_periodic=p25_diag_test_emit_periodic
    diag_count_sync_attempt=p25_diag_test_count_sync_attempt
    diag_count_bch_result=p25_diag_test_count_bch_result
    diag_count_frame=p25_diag_test_count_frame
    diag_dump_nid=p25_diag_test_dump_nid
    diag_uptime_s=p25_diag_test_uptime_s)
target_link_options(test_p25_diag_disabled PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=calloc
    -Wl,--wrap=free)
ls_add_test(test_p25_lcw ${APP}/p25/p25_lcw.c)
ls_add_test(test_app_contract ${FW}/components/apps/tui/ls_app_docs.c)
target_include_directories(test_app_contract PRIVATE
    ${FW}/components/apps/tui)

# Links the real loader, so it does not take the hardware stubs.
ls_add_test(test_userapp
    ${FW}/components/apps/tui/ls_userapp.c
    ${FW}/components/apps/tui/ls_app.c)
target_include_directories(test_userapp PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_dmr
    ${APP}/dmr/dmr_sync.c
    ${APP}/dmr/dmr_slot.c
    ${APP}/dmr/dmr_bptc.c
    ${APP}/dmr/dmr_lc.c
    ${APP}/dmr/dmr_burst.c)
ls_add_test(test_dmr_framer
    ${APP}/dmr/dmr_sync.c
    ${APP}/dmr/dmr_slot.c
    ${APP}/dmr/dmr_bptc.c
    ${APP}/dmr/dmr_lc.c
    ${APP}/dmr/dmr_burst.c
    ${APP}/dmr/dmr_framer.c)
ls_add_test(test_dmr_watch
    ${APP}/dmr/dmr_sync.c
    ${APP}/dmr/dmr_slot.c
    ${APP}/dmr/dmr_bptc.c
    ${APP}/dmr/dmr_lc.c
    ${APP}/dmr/dmr_burst.c
    ${APP}/dmr/dmr_framer.c
    ${APP}/dmr/dmr_watch.c)
ls_add_test(test_subghz_nrz ${APP}/subghz/subghz_nrz.c)
ls_add_test(test_subghz_file ${APP}/subghz/subghz_file.c)
target_include_directories(test_subghz_file PRIVATE ${APP}/subghz)
target_include_directories(test_subghz_nrz PRIVATE ${APP}/subghz)
ls_add_test(test_subghz_pwm
    ${APP}/subghz/subghz_pwm.c
    ${APP}/rec/rec_ook24.c)
target_include_directories(test_subghz_pwm PRIVATE
    ${APP}/subghz ${APP}/rec)
ls_add_test(test_dmr_over_iq
    ${APP}/dmr/dmr_sync.c
    ${APP}/dmr/dmr_slot.c
    ${APP}/dmr/dmr_bptc.c
    ${APP}/dmr/dmr_lc.c
    ${APP}/dmr/dmr_burst.c
    ${APP}/dmr/dmr_framer.c
    ${APP}/dmr/dmr_watch.c
    ${APP}/p25/dsp_pipeline.c
    ${APP}/p25/dsd_filters.c
    ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/p25_demod_control.c
    fixtures/dmr_iq_gen.c)
target_include_directories(test_dmr_over_iq PRIVATE ${APP}/dmr ${APP}/p25)
ls_add_test(test_wifi_sta ${FW}/main/ls_wifi_sta_core.c)
ls_add_test(test_ble_hci_rx_guard ${FW}/main/ble_hci_rx_guard.c
    shims/hci_rx_fake.c)
target_link_options(test_ble_hci_rx_guard PRIVATE -Wl,--wrap=hci_rx_handler
    -Wl,--wrap=transport_drv_reconfigure)
ls_add_test(test_wifi_operation)
ls_add_test(test_wifi_file_stream)
ls_add_test(test_ls_sweep ${FW}/components/lakeshark/dsp/ls_sweep_core.c)
target_include_directories(test_ls_sweep PRIVATE ${FW}/components/lakeshark/dsp)
ls_add_test(test_cell_core ${APP}/cell/cell_core.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c)
target_include_directories(test_cell_core PRIVATE ${APP}/cell ${FW}/components/lakeshark/dsp)
ls_add_test(test_cell_report ${APP}/cell/cell_report_core.c)
target_include_directories(test_cell_report PRIVATE ${APP}/cell)
ls_add_test(test_lte_sync ${APP}/cell/lte_sync.c)
target_include_directories(test_lte_sync PRIVATE ${APP}/cell)
ls_add_test(test_lte_resample ${APP}/cell/lte_resample.c)
target_include_directories(test_lte_resample PRIVATE ${APP}/cell)
ls_add_test(test_lte_mib ${APP}/cell/lte_mib.c)
target_include_directories(test_lte_mib PRIVATE ${APP}/cell)
ls_add_test(test_lte_sync_full ${APP}/cell/lte_sync.c)
target_include_directories(test_lte_sync_full PRIVATE ${APP}/cell)
target_compile_definitions(test_lte_sync_full PRIVATE LTE_SYNC_SEARCH_SAMPLES=57600)
ls_add_test(test_ble_link ${FW}/main/ble_link_core.c)
target_include_directories(test_ble_link PRIVATE ${FW}/main)
ls_add_test(test_flipper_telemetry_contract
    ${FW}/main/flipper_link_telemetry.c
    ${FW}/main/ble_link_core.c)
target_include_directories(test_flipper_telemetry_contract PRIVATE ${FW}/main)
target_include_directories(test_flipper_telemetry_contract PRIVATE
    ${FW}/components/lakeshark)
ls_add_test(test_ls_time)
ls_add_test(test_ls_cpu_busy ${FW}/components/lakeshark/core/ls_cpu_busy.c)
ls_add_test(test_ls_flash_task
    ${FW}/components/lakeshark/core/ls_flash_task.c
    ${FW}/components/lakeshark/core/ls_nvs_safe.c
    ${FW}/main/ble_link_host_task.c)
target_include_directories(test_ls_flash_task PRIVATE
    ${FW}/components/lakeshark/core)
ls_add_test(test_keymap ${FW}/components/apps/shell/ls_keymap.c)
ls_add_test(test_shell_transition
    ${FW}/components/apps/shell/ls_transition.c)
target_include_directories(test_shell_transition PRIVATE
    ${FW}/components/apps/shell)

ls_add_test(test_app_registry_lifecycle
    ${FW}/components/lakeshark/core/app_registry.c)
target_include_directories(test_app_registry_lifecycle PRIVATE
    ${FW}/components/lakeshark/core
    ${FW}/components/lakeshark/radio)
ls_add_test(test_p25_entry_settings
    ${APP}/p25/p25_entry_settings.c
    ${APP}/p25/p25_cqpsk_controls.c
    ${FW}/components/lakeshark/core/ls_flash_task.c
    ${FW}/components/lakeshark/core/ls_nvs_safe.c)
target_include_directories(test_p25_entry_settings PRIVATE
    ${FW}/components/lakeshark/core)
ls_add_test(test_ls_ui_palette
    ${FW}/components/apps/ui/ls_ui_palette.c)
ls_add_test(test_spectrum_waterfall
    ${FW}/components/apps/ui/ls_spectrum_waterfall_logic.c)
ls_add_test(test_settings_safety
    ${FW}/components/apps/ui/ls_ui_logic.c
    ${FW}/components/apps/settings/settings_action.c)
ls_add_test(test_safe_screen_hint
    ${FW}/components/apps/ui/ls_safe_screen_hint.c
    ${FW}/components/apps/ui/ls_ui_logic.c)
ls_add_test(test_media_playlist
    ${FW}/components/apps/media_gui/media_playlist.c)
ls_add_test(test_radio_endpoint
    ${FW}/components/lakeshark/radio/radio_endpoint.c)
ls_add_test(test_hackrf_radio
    ${FW}/components/lakeshark/radio/hackrf_radio.c)
ls_add_test(test_usb_descriptors)
target_include_directories(test_usb_descriptors PRIVATE ${FW}/components/lakeshark/board)
ls_add_test(test_imu_pose_policy)
target_include_directories(test_imu_pose_policy PRIVATE ${FW}/components/lakeshark/board)
ls_add_test(test_imu_status_policy)
target_include_directories(test_imu_status_policy PRIVATE ${FW}/components/lakeshark/board)
ls_add_test(test_radio_health
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/lakeshark/radio/radio_health.c)
ls_add_test(test_radio_health_report
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/lakeshark/radio/radio_health.c)
# a dongle that enumerates and then answers no control request. The
# endpoint counts control io, the watchdog escalates without waiting for a
# stall that cannot come, and the root-port cycle keeps the order IDF 5.4.3
# needs if it is not to abort.
ls_add_test(test_radio_control_fault
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/lakeshark/radio/radio_health.c)
ls_add_test(test_usb_port_cycle
    ${FW}/components/lakeshark/radio/usb_port_cycle.c)
# FM's radio open bounded for io, lifted by an endpoint change.
ls_add_test(test_radio_open_retry
    ${FW}/components/lakeshark/radio/radio_open_retry.c)
ls_add_test(test_usb_stream_start
    ${FW}/components/lakeshark/radio/esp_libusb.c)
ls_add_test(test_receiver_diagnostics
    ${FW}/components/lakeshark/radio/receiver_diagnostics.c)
ls_add_test(test_iq_control_preemption ${FW}/components/lakeshark/radio/iq_app_control.c)
target_include_directories(test_iq_control_preemption BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims/iq_critical)
ls_add_test(test_iq_app_control
    ${FW}/components/lakeshark/radio/iq_app_control.c
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/apps/ui/ls_receiver_status.c)
set(TX_BROKER_SOURCES
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/lakeshark/radio/tx_policy.c
    ${FW}/components/lakeshark/radio/tx_broker.c
    ${FW}/components/apps/shell/ls_tx_confirmation.c)
ls_add_test(test_tx_broker ${TX_BROKER_SOURCES}
    shims/tx_driver_fake.c shims/ls_input_gesture_fake.c)
target_compile_definitions(test_tx_broker PRIVATE
    CONFIG_LS_BOARD_P4_TOUCH_LCD_4B=1)
ls_add_test(test_tx_headless ${TX_BROKER_SOURCES})
target_compile_definitions(test_media_playlist PRIVATE
    CONFIG_AUDIO_PLAYER_ENABLE_MP3=1
    CONFIG_AUDIO_PLAYER_ENABLE_WAV=1)
ls_add_test(test_player_state
    ${FW}/components/bsp_extra/src/bsp_extra_player_state.c)
ls_add_test(test_media_handoff
    ${FW}/components/apps/media_gui/ls_media_handoff.c)
ls_add_test(test_media_lifecycle
    ${FW}/components/apps/media_gui/ls_media_lifecycle.c)

ls_add_test(test_file_iterator
    ${FW}/managed_components/chmorgan__esp-file-iterator/file_iterator.c
    ${FW}/components/apps/media_gui/file_iterator_delete.c
    ${FW}/components/apps/media_gui/media_playlist.c)
target_compile_definitions(test_file_iterator PRIVATE
    CONFIG_AUDIO_PLAYER_ENABLE_MP3=1
    CONFIG_AUDIO_PLAYER_ENABLE_WAV=1)
target_compile_options(test_file_iterator PRIVATE -Wno-format)
target_link_options(test_file_iterator PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=calloc
    -Wl,--wrap=free
    -Wl,--wrap=strdup)

ls_add_test(test_player_open
    ${FW}/components/bsp_extra/src/bsp_extra_player_open.c
    ${FW}/components/bsp_extra/src/bsp_extra_player_state.c
    ${FW}/managed_components/chmorgan__esp-file-iterator/file_iterator.c
    ${FW}/components/apps/media_gui/file_iterator_delete.c)
target_compile_options(test_player_open PRIVATE -Wno-format)
target_link_options(test_player_open PRIVATE
    -Wl,--wrap=fopen
    -Wl,--wrap=fclose)

ls_add_test(test_file_browser_path
    ${FW}/components/apps/file_browser/file_browser_path.c)

ls_add_test(test_media_supported
    ${FW}/components/apps/media_gui/media_playlist.c)
target_compile_definitions(test_media_supported PRIVATE
    CONFIG_AUDIO_PLAYER_ENABLE_MP3=1
    CONFIG_AUDIO_PLAYER_ENABLE_WAV=1)

ls_add_test(test_file_browser_dialog
    ${FW}/components/apps/file_browser/ls_dialog_slot.c)

ls_add_test(test_shell_back
    ${FW}/components/apps/shell/ls_shell_nav.c)

ls_add_test(test_hub_present
    ${FW}/components/apps/shell/ls_hub_present.c)

ls_add_test(test_map_wpt_select
    ${FW}/components/apps/map_gui/map_wpt_select.c)

ls_add_test(test_map_tiles_init
    ${FW}/components/apps/map_gui/map_tiles.c)
target_link_options(test_map_tiles_init PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=calloc
    -Wl,--wrap=free)

ls_add_test(test_map_tile_status
    ${FW}/components/apps/map_gui/map_tile_status.c)

ls_add_test(test_fm_sweep_arbitration
    ${FW}/components/apps/fm_gui/fm_sweep_arbitration.c)

ls_add_test(test_usb_autoreboot_pref
    ${FW}/components/lakeshark/core/usb_autoreboot_pref.c)
target_include_directories(test_usb_autoreboot_pref PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_p25_ldu2_ess ${APP}/p25/p25p1_ldu2.c ${APP}/p25/p25_ess.c)
ls_add_test(test_p25_hdu_ess ${APP}/p25/p25p1_hdu.c
    ${APP}/p25/p25p1_ldu.c ${APP}/p25/p25_ess.c)
ls_add_test(test_location_pref)
ls_add_test(test_p25_tg_observed ${APP}/p25/p25_tg_observed.c ${APP}/p25/p25_lcw.c)
file(GLOB P25_IMBE_TEST_SOURCES ${FW}/components/imbe_vocoder/*.cc)
# The vocoder overflows on purpose and is correct doing it.  L_add in
# basicop2.cc is the ETSI saturating add: it performs the addition that
# overflows and then reads the sign bits to clamp to MIN_32/MAX_32, which is
# how every fixed-point reference implementation of that operator is written.
# v_synt.cc says so on the line itself - "integer multiplication by mod 1".
#
# So the wrap is the contract, and -fwrapv states it instead of leaving it to
# chance.  It also keeps UBSan out of code whose whole method it would
# otherwise report, which is what left the sanitizer gate red and unrunnable:
# two of its three failures were this, and neither was a defect.
set_source_files_properties(
    ${P25_IMBE_TEST_SOURCES} ${FW}/components/imbe_vocoder/imbe_shim.cpp
    PROPERTIES COMPILE_OPTIONS "-fwrapv")
add_executable(test_imbe_pcm tests/test_imbe_pcm.cpp ${P25_IMBE_TEST_SOURCES}
    ${FW}/components/imbe_vocoder/imbe_shim.cpp)
target_include_directories(test_imbe_pcm PRIVATE ${FW}/components/imbe_vocoder)
target_link_libraries(test_imbe_pcm ls_harness m)
add_test(NAME test_imbe_pcm COMMAND test_imbe_pcm)
add_executable(test_imbe_alloc tests/test_imbe_alloc.cpp ${P25_IMBE_TEST_SOURCES}
    ${FW}/components/imbe_vocoder/imbe_shim.cpp)
target_include_directories(test_imbe_alloc PRIVATE ${FW}/components/imbe_vocoder)
target_link_libraries(test_imbe_alloc ls_harness m)
add_test(NAME test_imbe_alloc COMMAND test_imbe_alloc)

ls_add_test(test_ls_crash
    ${FW}/components/lakeshark/core/ls_crash.c)
target_include_directories(test_ls_crash PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_ls_crash_esp
    ${FW}/components/lakeshark/core/ls_crash.c
    ${FW}/components/lakeshark/core/ls_crash_esp.c)
target_include_directories(test_ls_crash_esp BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_shims
    ${FW}/components/lakeshark/core)
target_compile_definitions(test_ls_crash_esp PRIVATE CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=1)

ls_add_test(test_settings_schema
    ${FW}/components/lakeshark/core/settings_schema.c)
target_include_directories(test_settings_schema PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_p25_demod_pref_cache
    ${FW}/components/lakeshark/core/p25_demod_pref_cache.c)
target_include_directories(test_p25_demod_pref_cache PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_nfc_ndef
    ${FW}/components/lakeshark/nfc/ndef.c)
target_include_directories(test_nfc_ndef PRIVATE
    ${FW}/components/lakeshark/nfc)

ls_add_test(test_nfc_t2t
    ${FW}/components/lakeshark/nfc/t2t.c)
target_include_directories(test_nfc_t2t PRIVATE
    ${FW}/components/lakeshark/nfc)

ls_add_test(test_nfc_a
    ${FW}/components/lakeshark/nfc/nfc_a.c)
target_include_directories(test_nfc_a PRIVATE
    ${FW}/components/lakeshark/nfc)

ls_add_test(test_ls_version
    ${FW}/components/lakeshark/core/ls_version.c)
target_include_directories(test_ls_version PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_safe_mode
    ${FW}/components/lakeshark/core/ls_safe_mode.c)
target_include_directories(test_safe_mode PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_tui_contrast
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_contrast PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_fm_am ${APP}/fm/fm_dsp.c)

ls_add_test(test_field ${FW}/components/apps/tui/ls_field.c ${FW}/components/apps/tui/ls_compass.c)
target_include_directories(test_field PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
ls_add_test(test_compass ${FW}/components/apps/tui/ls_compass.c)
target_include_directories(test_compass PRIVATE ${LS_TUI_APP_INCLUDES})
target_compile_definitions(test_field PRIVATE LS_FIELD_TEST=1)

ls_add_test(test_map_motion ${FW}/components/apps/tui/ls_map_motion.c)
target_include_directories(test_map_motion PRIVATE ${LS_TUI_APP_INCLUDES})
target_include_directories(test_tui_screens PRIVATE ${CARTO}/include)

ls_add_test(test_nfc_classic ${FW}/components/lakeshark/nfc/nfc_classic.c)

add_executable(test_p25_phase2 tests/test_p25_phase2.cpp)
target_link_libraries(test_p25_phase2 p25_phase2_host ls_harness m)
add_test(NAME test_p25_phase2 COMMAND test_p25_phase2)
ls_add_test(test_p25_phase2_dsp ${APP}/p25/dsp_pipeline.c ${APP}/p25/p25_cqpsk_controls.c fixtures/lsm_gen.c)
target_compile_definitions(test_p25_phase2_dsp PRIVATE LSM_GEN_BAUD=6000)

foreach(target test_tui_picker test_tui_wireless test_tui_waterfall_claim
               test_tui_waterfall_scale test_tui_quick test_tui_waterfall_stall
               test_tui_keyboard)
    target_compile_definitions(${target} PRIVATE LS_TUI_CORNER_STUB=1)
endforeach()
foreach(target test_tui_picker test_tui_quick test_tui_keyboard)
    target_compile_definitions(${target} PRIVATE LS_TUI_WIDE_STUB=1)
endforeach()
target_include_directories(test_nfc_classic PRIVATE ${FW}/components/lakeshark/nfc)

ls_add_test(test_scan_engine
    ${FW}/components/lakeshark/core/scan_feedback.c
    ${FW}/components/lakeshark/core/scan_geo.c)
ls_add_test(test_scan_geo ${FW}/components/lakeshark/core/scan_geo.c
    ${FW}/components/lakeshark/core/scan_import.c)
ls_add_test(test_scan_sd_store)
ls_add_test(test_scan_journal ${FW}/components/lakeshark/core/scan_journal_codec.c)
target_include_directories(test_scan_journal PRIVATE ${FW}/components/lakeshark/core)
ls_add_test(test_scan_store)
target_include_directories(test_scan_store BEFORE PRIVATE ${FW}/bench/shims/scan_store)
ls_add_test(test_scan_upload ${FW}/components/lakeshark/core/scan_import.c)
ls_add_test(test_scan_list_nh_ma
    ${FW}/components/lakeshark/core/scan_import.c
    ${FW}/components/lakeshark/core/scan_geo.c)
target_compile_definitions(test_scan_list_nh_ma PRIVATE
    SCAN_LIST_NH_MA_PATH="${FW}/tools/scanlist_nh_ma.lscan")
ls_add_test(test_scan_list_cape_cod
    ${FW}/components/lakeshark/core/scan_import.c
    ${FW}/components/lakeshark/core/scan_geo.c)
target_compile_definitions(test_scan_list_cape_cod PRIVATE
    SCAN_LIST_CAPE_PATH="${FW}/tools/scanlist_cape_cod.lscan")
ls_add_test(test_scan_list_all
    ${FW}/components/lakeshark/core/scan_import.c
    ${FW}/components/lakeshark/core/scan_geo.c)
target_compile_definitions(test_scan_list_all PRIVATE
    SCAN_LIST_ALL_PATH="${FW}/tools/scanlist_all.lscan")
ls_add_test(test_hub_transfer)
target_include_directories(test_scan_engine PRIVATE
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/radio
    ${APP}/p25 ${APP}/fm)

ls_add_test(test_cell_session ${APP}/cell/cell_session.c)
target_include_directories(test_cell_session PRIVATE ${APP}/cell)
ls_add_test(test_panel_id)
target_include_directories(test_panel_id PRIVATE ${FW}/components/lakeshark/board)
target_sources(lssim PRIVATE ${APP}/subghz/subghz_pwm.c ${APP}/subghz/subghz_nrz.c)
target_sources(test_tui_screens PRIVATE ${APP}/subghz/subghz_pwm.c ${APP}/subghz/subghz_nrz.c)
target_sources(test_tui_render_dump PRIVATE ${APP}/subghz/subghz_pwm.c ${APP}/subghz/subghz_nrz.c)

add_executable(subdecode tools/subdecode.c ${APP}/subghz/subghz_pwm.c ${APP}/subghz/subghz_nrz.c)
target_include_directories(subdecode PRIVATE ${APP}/subghz)

# The production voice path end to end on a real capture: HDU/LDU/TDU, the ESS
# mute policy and the OP25 IMBE decoder. A diagnostic, like p25_iq_replay;
# test_p25_voice_capture turns it into a gate when a capture is present.
set(P25_VOICE_REPLAY_SOURCES
    ${APP}/p25/dsp_pipeline.c
    ${APP}/p25/dsd_symbol.c
    ${APP}/p25/dsd_dibit.c
    ${APP}/p25/dsd_frame_sync.c
    ${APP}/p25/dsd_frame.c
    ${APP}/p25/dsd_filters.c
    ${APP}/p25/dsd_main.c
    ${APP}/p25/p25p1_heuristics.c
    ${APP}/p25/p25p1_hdu.c
    ${APP}/p25/p25p1_ldu.c
    ${APP}/p25/p25p1_ldu1.c
    ${APP}/p25/p25p1_ldu2.c
    ${APP}/p25/p25p1_tdu.c
    ${APP}/p25/p25p1_tdulc.c
    ${APP}/p25/p25p1_check_nid.c
    ${APP}/p25/p25p1_check_hdu.cpp
    ${APP}/p25/p25p1_check_ldu.cpp
    ${APP}/p25/Hamming.cpp
    ${APP}/p25/p25_lcw.c
    ${APP}/p25/p25_ess.c
    ${APP}/p25/p25_tsbk.c
    ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/p25_acquisition.c
    ${APP}/p25/p25_voice_hold.c
    ${FW}/components/mbelib/mbelib.c
    ${FW}/components/mbelib/ecc.c
    ${FW}/components/mbelib/imbe7200x4400.c
    ${P25_IMBE_TEST_SOURCES}
    ${FW}/components/imbe_vocoder/imbe_shim.cpp)
add_executable(p25_voice_replay tools/p25_voice_replay.c ${P25_VOICE_REPLAY_SOURCES})
target_include_directories(p25_voice_replay PRIVATE ${FW}/components/imbe_vocoder)
target_link_libraries(p25_voice_replay ls_harness m)

# Generated voice calls through the production path, vocoder recorded:
# which IMBE frames reach the speaker, bit-exact, under the impairments that
# have broken voice on the air. The fixture includes the NID module itself,
# and the real vocoder is replaced by the recorder, so neither is linked.
set(P25_VOICE_CALL_SOURCES ${P25_VOICE_REPLAY_SOURCES})
list(REMOVE_ITEM P25_VOICE_CALL_SOURCES
    ${APP}/p25/p25p1_check_nid.c
    ${FW}/components/imbe_vocoder/imbe_shim.cpp
    ${P25_IMBE_TEST_SOURCES})
ls_add_test(test_p25_voice_call fixtures/p25_call_gen.c ${P25_VOICE_CALL_SOURCES})
target_include_directories(test_p25_voice_call PRIVATE
    ${FW}/components/imbe_vocoder ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)
