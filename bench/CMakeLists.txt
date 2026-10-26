cmake_minimum_required(VERSION 3.18)
project(lakeshark_bench C CXX)

add_executable(test_character_mix_helpers tests/test_character_mix_helpers.c
    ../components/apps/tui/ls_tui_ui.c ../components/apps/tui/ls_glyph.c ../components/apps/tui/ls_icons.c
    ../components/apps/tui/tuilib/tui_core.c ../components/apps/tui/ls_i18n.c)
target_include_directories(test_character_mix_helpers PRIVATE
    shims ../components/apps/tui ../components/apps ../components/lakeshark)
target_compile_options(test_character_mix_helpers PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_character_mix_helpers PRIVATE -Wl,--gc-sections)
add_test(NAME test_character_mix_helpers COMMAND test_character_mix_helpers)

add_executable(test_ui_perf tests/test_ui_perf.c
    ../components/apps/tui/ls_ui_perf.c)
target_include_directories(test_ui_perf PRIVATE ../components/apps/tui)
add_test(NAME test_ui_perf COMMAND test_ui_perf)

add_executable(test_hamming_tables tests/test_hamming_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/lakeshark/apps/p25/Hamming.cpp)
target_include_directories(test_hamming_tables PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/lakeshark/apps/p25)
add_test(NAME test_hamming_tables COMMAND test_hamming_tables)

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

set(DMR_WATCH_SRCS
    ${APP}/dmr/dmr_sync.c ${APP}/dmr/dmr_slot.c ${APP}/dmr/dmr_bptc.c
    ${APP}/dmr/dmr_lc.c ${APP}/dmr/dmr_burst.c ${APP}/dmr/dmr_framer.c
    ${APP}/dmr/dmr_watch.c ${APP}/dmr/dmr_listen.c)
set(LS_TUI_APP_SRCS
    ${FW}/components/apps/tui/ls_sweep.c
    ${FW}/components/apps/tui/ls_sweep_ui.c
    ${FW}/components/apps/tui/ls_rid.c
    ${FW}/components/apps/tui/ls_rid_ui.c
    ${FW}/components/apps/tui/ls_drone_ui.c
    ${FW}/bench/shims/ls_drone_watch_fake.c
    ${FW}/bench/shims/ls_music_view_fake.c
    ${DMR_WATCH_SRCS}
    ${FW}/components/apps/tui/experiments/rs41_store.c
    ${FW}/components/apps/tui/ls_audio_view.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/ls_calls.c
    ${FW}/components/lakeshark/audio/call_archive.c
    ${FW}/components/lakeshark/audio/call_recorder.c
    ${FW}/components/lakeshark/audio/call_store.c
    ${FW}/bench/tools/lssim_survey.c
    ${FW}/components/apps/tui/ls_survey.c
    ${FW}/components/apps/tui/ls_survey_ui.c
    ${FW}/components/apps/tui/ls_compass_art.c
    ${FW}/bench/tools/lssim_p25_settings.c
    ${FW}/components/apps/tui/ls_radio_panel.c
    ${FW}/components/apps/tui/ls_scan_options.c
    ${FW}/components/apps/tui/ls_scan_view.c
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
    ${FW}/components/apps/tui/ls_update_view.c
    ${FW}/components/apps/tui/ls_c6_view.c
    ${FW}/components/apps/tui/ls_skyplot.c
    ${FW}/components/apps/tui/ls_skyview.c
    ${FW}/components/apps/tui/ls_text.c
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_quick.c
    ${FW}/components/apps/tui/ls_numpad.c
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_route_live.c
    ${FW}/components/lakeshark/core/ls_route.c
    ${FW}/components/apps/tui/ls_notify.c
    ${FW}/components/apps/tui/ls_geo.c
    ${FW}/components/apps/tui/ls_picker.c
    ${FW}/components/apps/tui/ls_radio_select.c
    ${FW}/bench/tools/lssim_rsel.c
    ${FW}/components/apps/tui/ls_options.c
    ${FW}/components/apps/tui/ls_action.c
    ${FW}/components/apps/tui/ls_value.c
    ${FW}/components/apps/tui/ls_font_list.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/lakeshark/core/ls_errlog.c
    ${FW}/bench/shims/ls_errlog_fake.c
    ${FW}/components/apps/tui/ls_term_scroll.c
    ${FW}/bench/shims/ls_term_fake.c
    ${FW}/bench/shims/tui_hw_stubs.c)

set(LS_EXP_SRCS
    ${FW}/bench/shims/rs41_stream_fake.c
    ${FW}/components/apps/tui/experiments/exp_loramon.c
    ${FW}/components/apps/tui/experiments/exp_zwave.c
    ${FW}/components/apps/tui/experiments/exp_wisun.c
    ${FW}/components/apps/tui/experiments/lorafam_decode.c
    ${FW}/bench/shims/lorafam_fake.c
    ${FW}/components/apps/tui/ls_experiments.c
    ${FW}/components/apps/tui/ls_experiments_builtin.c
    ${FW}/components/apps/tui/experiments/exp_aviation.c
    ${FW}/components/apps/tui/experiments/aviation_decode.c
    ${APP}/adsb/uat_weather.c
    ${FW}/components/apps/tui/experiments/exp_uat.c
    ${APP}/adsb/uat_decode.c
    ${FW}/components/apps/tui/experiments/exp_carrier.c
    ${FW}/components/apps/tui/experiments/exp_rs41.c
    ${FW}/components/apps/tui/experiments/rs41_decode.c
    ${FW}/components/apps/tui/experiments/rs41_stream.c
    ${FW}/components/apps/tui/experiments/rs41_store.c
    ${FW}/components/apps/tui/experiments/exp_dfm17.c
    ${FW}/components/apps/tui/experiments/dfm_decode.c
    ${FW}/components/apps/tui/experiments/exp_p25site.c
    ${FW}/components/apps/tui/experiments/p25_sitefind.c
    ${FW}/components/apps/tui/experiments/exp_pagers.c
    ${FW}/components/apps/tui/experiments/pager_recon.c
    ${FW}/components/apps/tui/experiments/pager_iq.c
    ${FW}/components/apps/tui/experiments/pager_iq_radio.c
    ${APP}/fm/pocsag.c
    ${FW}/components/apps/tui/experiments/exp_burst.c
    ${FW}/components/apps/tui/experiments/exp_iridium.c
    ${FW}/components/apps/tui/experiments/exp_rf.c
    ${FW}/components/apps/tui/experiments/exp_survey.c
    ${FW}/components/apps/tui/experiments/exp_lr433.c
    ${FW}/components/apps/tui/experiments/lr433_dec.c
    ${FW}/components/apps/tui/experiments/sensor_history.c
    ${FW}/components/apps/tui/experiments/lr433_history.c)

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
    ${APP}/dmr
    ${FW}/components/lakeshark/audio
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
    ${FW}/components/apps
    ${FW}/components/bsp_extra/include
    ${FW}/managed_components/chmorgan__esp-file-iterator/include
)

add_library(ls_harness STATIC
    harness/ls_test.c
    harness/shim_impl.c
    shims/pagers_settings_fake.c
    shims/freertos_shim.c
    shims/spi_shim.c
    shims/ls_trail_host.c
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

function(ls_add_test name)
    add_executable(${name} tests/${name}.c ${ARGN})
    target_link_libraries(${name} ls_harness m)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ls_add_test(test_radios_status ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_radios_status PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark)
target_compile_definitions(test_radios_status PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_radios_antenna
    ${FW}/components/lakeshark/board/ls_board_hw.c
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_radios_antenna PRIVATE
    ${FW}/components/lakeshark/board ${FW}/bench/shims)
target_compile_definitions(test_radios_antenna PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_tui_surface ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_tui_surface PRIVATE
    ${FW}/components/apps/tui/tuilib)

ls_add_test(test_term_vt ${FW}/components/apps/tui/ls_vt.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_term_vt PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui ${FW}/components/apps/tui/tuilib)

ls_add_test(test_ws ${FW}/components/apps/tui/ls_ws.c)
target_include_directories(test_ws PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_mdns ${FW}/components/apps/tui/ls_mdns.c)
target_include_directories(test_mdns PRIVATE ${FW}/components/apps/tui)

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

set(KLATT ${FW}/components/klatt_tts)
add_library(klatt_tts_host STATIC
    ${KLATT}/klatt-tts/src/config.cc
    ${KLATT}/klatt-tts/src/klatt.cc
    ${KLATT}/klatt-tts/src/phonemes.cc
    ${KLATT}/klatt-tts/src/synth_internal.cc
    ${KLATT}/klatt-tts/src/synth_stream.cc
    ${KLATT}/g2p/src/g2p.cc
    ${KLATT}/g2p/src/g2p_dict.cc
    ${KLATT}/g2p/src/g2p_numbers.cc
    ${KLATT}/g2p/src/g2p_phones.cc
    ${KLATT}/g2p/src/g2p_rules.cc
    ${KLATT}/g2p/src/ipa_tokens.cc)
# BEFORE: the bench-wide include list has its own config.h.
target_include_directories(klatt_tts_host BEFORE
    PUBLIC  ${KLATT}/klatt-tts/include ${KLATT}/g2p/include
    PRIVATE ${KLATT}/klatt-tts/include/tts ${KLATT}/g2p/include/g2p ${KLATT}/g2p/src)
target_compile_definitions(klatt_tts_host PRIVATE LAKESHARK_COMPACT_DICT)
target_compile_options(klatt_tts_host PRIVATE -O2)

ls_add_test(test_speech_engine
    ${FW}/components/lakeshark/audio/speech_engine.cpp
    ${FW}/components/lakeshark/audio/plane_audio.c)
target_include_directories(test_speech_engine PRIVATE ${FW}/components/lakeshark/audio)
target_link_libraries(test_speech_engine klatt_tts_host)

ls_add_test(test_mesh_phrase ${FW}/components/lakeshark/audio/mesh_phrase.c)
ls_add_test(test_mesh_inspect ${FW}/components/meshcore/ls_mesh_inspect.c)
target_include_directories(test_mesh_inspect PRIVATE ${FW}/components/meshcore)
target_include_directories(test_mesh_phrase PRIVATE ${FW}/components/lakeshark/audio)
target_include_directories(test_display_dim PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_tui_chrome
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_text.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_tui_chrome PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib)

ls_add_test(test_tui_screens
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    tools/lssim_field.c
    ${FW}/components/apps/tui/screens/ext/scr_labs.c
    ${FW}/components/apps/tui/screens/ext/scr_subghz.c
    ${FW}/components/apps/tui/screens/ext/scr_mixrf.c
    ${FW}/components/apps/tui/screens/ext/scr_experiments.c
    ${LS_EXP_SRCS}
    ${FW}/bench/shims/ls_exp_fake.c
    ${FW}/bench/shims/ls_scan_fake.c
    ${FW}/components/apps/tui/screens/ext/scr_nfc.c
    ${FW}/components/lakeshark/nfc/ls_nfc_suite.c
    ${FW}/components/lakeshark/nfc/nfc_classic.c
    ${FW}/components/apps/tui/screens/ext/scr_journal.c
    ${FW}/components/apps/tui/screens/ext/scr_notes.c
    ${FW}/components/apps/tui/screens/ext/scr_compass.c
    ${FW}/components/apps/tui/ls_compass_art.c
    ${FW}/components/apps/tui/ls_df.c
    ${FW}/components/apps/tui/ls_df_fit.c
    ${FW}/components/apps/tui/ls_df_band.c
    ${FW}/components/apps/tui/ls_df_cal.c
    ${FW}/components/apps/tui/ls_df_hits.c
    ${FW}/components/apps/tui/ls_df_view.c
    ${FW}/components/apps/tui/ls_sun.c
    tools/lssim_dfs.c
    ${FW}/components/apps/tui/ls_compass_live.c
    ${FW}/components/apps/tui/ls_compass_learn.c
    ${FW}/components/apps/tui/ls_wmm.c
    ${FW}/components/apps/tui/ls_textbuf.c
    ${FW}/components/apps/tui/ls_keydock.c
    ${FW}/components/apps/tui/ls_notes.c
    ${FW}/components/apps/tui/ls_note_blocks.c
    ${FW}/components/apps/tui/screens/ext/scr_settings.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/apps/tui/screens/ext/scr_update.c
    ${FW}/components/apps/tui/ls_voice_opts.c
    ${FW}/components/apps/tui/screens/ext/scr_diag.c
    ${FW}/components/apps/tui/screens/ext/scr_terminal.c
    ${FW}/components/lakeshark/core/ls_vitals.c
    ${FW}/components/lakeshark/core/ls_trend.c
    ${FW}/components/lakeshark/core/ls_version.c
    ${FW}/components/lakeshark/core/ls_safe_mode.c
    ${FW}/components/lakeshark/board/ls_haptic.c
    ${FW}/components/apps/tui/screens/ext/scr_rec.c
    ${FW}/components/apps/tui/screens/ext/scr_falls.c
    ${FW}/components/apps/tui/screens/main/scr_home.c
    ${APP}/fm/fm_tone.c
    ${APP}/acars/acars_app.c
    ${FW}/components/apps/tui/screens/main/scr_fm.c
    ${FW}/components/apps/tui/screens/main/fm_same_view.c
    ${FW}/components/apps/tui/screens/main/fm_aprs_view.c
    ${FW}/components/apps/tui/screens/main/fm_ais_view.c
    ${APP}/fm/same.c
    ${APP}/fm/same_store.c
    ${APP}/fm/ais.c
    ${APP}/fm/ais_store.c
    ${APP}/fm/aprs_store.c
    ${APP}/fm/aprs.c
    ${FW}/components/apps/tui/screens/main/scr_adsb.c
    ${FW}/components/apps/tui/ls_follow.c
    ${FW}/components/apps/tui/ls_map_motion.c
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

ls_add_test(test_experiments
    ${LS_EXP_SRCS}
    ${FW}/components/apps/tui/ls_geo.c)
target_include_directories(test_experiments PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_exp_dfm17
    ${FW}/components/apps/tui/experiments/dfm_decode.c
    ${FW}/components/apps/tui/experiments/exp_dfm17.c
    ${FW}/components/apps/tui/ls_experiments.c
    ${FW}/components/apps/tui/ls_geo.c)
target_include_directories(test_exp_dfm17 PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui/experiments)

ls_add_test(test_exp_pagers
    ${FW}/components/apps/tui/ls_experiments.c
    ${FW}/components/apps/tui/experiments/exp_pagers.c
    ${FW}/components/apps/tui/experiments/pager_recon.c
    ${APP}/fm/pocsag.c)
target_include_directories(test_exp_pagers PRIVATE
    ${LS_TUI_APP_INCLUDES} ${FW}/components/apps/tui/experiments)

ls_add_test(test_exp_p25site
    ${FW}/components/apps/tui/ls_experiments.c
    ${FW}/components/apps/tui/experiments/exp_p25site.c
    ${FW}/components/apps/tui/experiments/p25_sitefind.c)
target_include_directories(test_exp_p25site PRIVATE
    ${LS_TUI_APP_INCLUDES} ${FW}/components/apps/tui/experiments)

foreach(exp_id burst iridium survey)
    ls_add_test(test_exp_${exp_id}
        ${FW}/components/apps/tui/ls_experiments.c
        ${FW}/components/apps/tui/experiments/exp_rf.c
        ${FW}/components/apps/tui/experiments/exp_${exp_id}.c)
    target_include_directories(test_exp_${exp_id} PRIVATE ${LS_TUI_APP_INCLUDES})
endforeach()

ls_add_test(test_exp_lr433
    ${LS_EXP_SRCS}
    ${FW}/components/apps/tui/ls_geo.c
    ${FW}/bench/shims/ls_scan_fake.c
    ${FW}/bench/shims/ls_exp_others_fake.c)
target_include_directories(test_exp_lr433 PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_sensor_history
    ${FW}/components/apps/tui/experiments/sensor_history.c
    ${FW}/components/apps/tui/experiments/lr433_dec.c)
target_include_directories(test_sensor_history PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_sensor_history_io
    ${FW}/components/apps/tui/experiments/sensor_history.c
    ${FW}/components/apps/tui/experiments/lr433_history.c
    ${FW}/components/apps/tui/experiments/lr433_dec.c)
target_include_directories(test_sensor_history_io PRIVATE ${LS_TUI_APP_INCLUDES})
target_compile_definitions(test_sensor_history_io PRIVATE LS_SENSOR_HISTORY_TEST=1
    LS_SENSOR_ROOT="${CMAKE_CURRENT_BINARY_DIR}/sensor-history-io")

ls_add_test(test_tui_router
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/ls_tui_screen.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
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
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_tui_p25_screen PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark
    ${APP}/p25
    ${FW}/bench/shims)
foreach(boundary ls_lora_hw_lock spi_device_transmit spi_device_polling_transmit
        nvs_set_u8 nvs_set_i8 nvs_set_u32 nvs_set_i32 nvs_set_u64 nvs_commit
        ls_action_call lakeshark_p25_mode_name)
    target_link_options(test_tui_p25_screen PRIVATE "-Wl,--wrap=${boundary}")
endforeach()

ls_add_test(test_tui_waterfall_claim
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_waterfall_claim PRIVATE
    ${LS_TUI_APP_INCLUDES})

ls_add_test(test_falls_display
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_falls_display PRIVATE
    ${LS_TUI_APP_INCLUDES})

target_compile_definitions(test_falls_display PRIVATE LS_TUI_CORNER_STUB=1 LS_WF_SETTINGS_FAKE=1)

ls_add_test(test_tui_waterfall_scale
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_waterfall_scale PRIVATE
    ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_waterfall_stall
    ${FW}/components/apps/tui/ls_waterfall.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_waterfall_stall PRIVATE
    ${LS_TUI_APP_INCLUDES})

ls_add_test(test_lr_wf_session
    ${FW}/components/lakeshark/dsp/lr_iq_spectrum.c
    ${FW}/components/lakeshark/dsp/spectrum.c
    ${APP}/p25/lr2021_iq_queue.c)
ls_add_test(test_lr_iq_spectrum ${FW}/components/lakeshark/dsp/spectrum.c)
ls_add_test(test_wf_source ${FW}/components/apps/tui/ls_wf_source.c
    ${FW}/components/lakeshark/dsp/lr_iq_spectrum.c
    ${FW}/components/lakeshark/dsp/spectrum.c)
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
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_picker PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_mixrf_control)
ls_add_test(test_mixrf_field)
ls_add_test(test_nfc_suite ${FW}/components/lakeshark/nfc/nfc_classic.c)
target_include_directories(test_nfc_suite PRIVATE
    ${FW}/components/lakeshark/nfc ${FW}/components/lakeshark/board)

ls_add_test(test_radio_select
    ${FW}/components/apps/tui/ls_radio_select.c
    ${FW}/components/lakeshark/core/radio_choice.c
    ${FW}/components/apps/tui/ls_picker.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_radio_select PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_options
    ${FW}/components/apps/tui/ls_options.c
    ${FW}/components/apps/tui/ls_radio_select.c
    ${FW}/components/lakeshark/core/radio_choice.c
    ${FW}/components/apps/tui/ls_picker.c
    ${FW}/components/apps/tui/ls_numpad.c
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_options PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_scan_shell
    ${FW}/components/apps/tui/ls_scan_view.c
    ${FW}/components/apps/tui/ls_scan_options.c
    ${FW}/components/apps/tui/ls_options.c
    ${FW}/components/apps/tui/ls_radio_select.c
    ${FW}/components/lakeshark/core/radio_choice.c
    ${FW}/components/apps/tui/ls_picker.c
    ${FW}/components/apps/tui/ls_numpad.c
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/tools/lssim_scan.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_scan_shell PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_keyboard
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_keyboard PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_wireless
    ${FW}/components/apps/tui/ls_survey.c
    ${FW}/components/apps/tui/ls_survey_ui.c
    ${FW}/components/apps/tui/ls_compass_art.c
    ${FW}/components/apps/tui/screens/ext/scr_wireless.c
    ${FW}/bench/shims/ls_rterm_fake.c
    ${FW}/components/apps/tui/ls_wireless_history.c
    ${FW}/components/apps/tui/ls_keyboard.c
    ${FW}/components/apps/tui/ls_glyph.c
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_wireless PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_tui_quick
    ${FW}/components/apps/tui/ls_quick.c
    ${FW}/components/apps/tui/ls_action.c
    ${FW}/components/apps/tui/ls_value.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_quick PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_ls_registry
    ${FW}/components/apps/tui/ls_value.c
    ${FW}/components/apps/tui/ls_action.c)
target_include_directories(test_ls_registry PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_ls_ota_core ${FW}/components/lakeshark/core/ls_ota_core.c)

add_executable(lssim
    tools/lssim_detector.c
    ${FW}/components/apps/tui/ls_detector.c
    ${FW}/components/apps/tui/ls_detector_sinks.c
    ${FW}/components/apps/tui/ls_detector_air.c
    ${FW}/components/apps/tui/ls_detector_ui.c
    tools/lssim_wireless.c
    ${FW}/components/apps/tui/screens/ext/scr_wireless.c
    ${FW}/bench/shims/ls_rterm_fake.c
    ${FW}/components/apps/tui/ls_wireless_history.c
    shims/pagers_settings_fake.c
    ${FW}/components/apps/tui/screens/ext/scr_music.c
    tools/lssim_music.c
    ${FW}/components/apps/tui/ls_splash.c
    ${FW}/components/apps/tui/ls_splash_world.c
    shims/ls_trail_host.c
    ${FW}/components/apps/tui/ls_wordmark.c
    tools/lssim.c
    tools/lssim_term.c
    ${FW}/components/apps/tui/ls_vt.c
    tools/lssim_ota.c
    ${FW}/components/lakeshark/core/ls_ota_core.c
    tools/lssim_radio_preview.c
    tools/lssim_field.c
    ${FW}/components/apps/tui/screens/ext/scr_labs.c
    ${FW}/components/apps/tui/screens/ext/scr_subghz.c
    ${FW}/components/apps/tui/screens/ext/scr_mixrf.c
    ${FW}/components/apps/tui/screens/ext/scr_experiments.c
    ${LS_EXP_SRCS}
    ${FW}/bench/shims/ls_exp_fake.c
    ${FW}/components/apps/tui/screens/ext/scr_nfc.c
    ${FW}/components/lakeshark/nfc/ls_nfc_suite.c
    ${FW}/components/lakeshark/nfc/nfc_classic.c
    ${FW}/components/apps/tui/screens/ext/scr_journal.c
    ${FW}/components/apps/tui/screens/ext/scr_notes.c
    ${FW}/components/apps/tui/screens/ext/scr_compass.c
    ${FW}/components/apps/tui/ls_compass_art.c
    ${FW}/components/apps/tui/ls_df.c
    ${FW}/components/apps/tui/ls_df_fit.c
    ${FW}/components/apps/tui/ls_df_band.c
    ${FW}/components/apps/tui/ls_df_cal.c
    ${FW}/components/apps/tui/ls_df_hits.c
    ${FW}/components/apps/tui/ls_df_view.c
    ${FW}/components/apps/tui/ls_sun.c
    tools/lssim_dfs.c
    ${FW}/components/apps/tui/ls_compass_live.c
    ${FW}/components/apps/tui/ls_compass_learn.c
    ${FW}/components/apps/tui/ls_wmm.c
    ${FW}/components/apps/tui/ls_textbuf.c
    ${FW}/components/apps/tui/ls_keydock.c
    ${FW}/components/apps/tui/ls_notes.c
    ${FW}/components/apps/tui/ls_note_blocks.c
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/ls_tui.c ${FW}/components/apps/tui/ls_font_cyr.c
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
    ${APP}/fm/fm_tone.c
    ${APP}/acars/acars_app.c
    ${FW}/components/apps/tui/screens/main/scr_fm.c
    ${FW}/components/apps/tui/screens/main/fm_same_view.c
    ${FW}/components/apps/tui/screens/main/fm_aprs_view.c
    ${FW}/components/apps/tui/screens/main/fm_ais_view.c
    ${APP}/fm/same.c
    ${APP}/fm/same_store.c
    ${APP}/fm/ais.c
    ${APP}/fm/ais_store.c
    ${APP}/fm/aprs_store.c
    ${APP}/fm/aprs.c
    ${FW}/components/apps/tui/screens/main/scr_adsb.c
    ${FW}/components/apps/tui/screens/ext/scr_rec.c
    ${FW}/components/apps/tui/screens/ext/scr_diag.c
    ${FW}/components/apps/tui/screens/ext/scr_terminal.c
    ${FW}/components/lakeshark/core/ls_vitals.c
    ${FW}/components/lakeshark/core/ls_trend.c
    ${FW}/components/lakeshark/core/ls_version.c
    ${FW}/components/lakeshark/core/ls_safe_mode.c
    ${FW}/components/lakeshark/board/ls_haptic.c
    ${FW}/components/apps/tui/screens/ext/scr_settings.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/apps/tui/screens/ext/scr_update.c
    ${FW}/components/apps/tui/ls_voice_opts.c
    ${FW}/components/apps/tui/screens/ext/scr_gps.c
    ${FW}/components/apps/tui/ls_map_motion.c
    ${FW}/components/apps/tui/ls_follow.c
    ${FW}/components/apps/tui/screens/ext/scr_map.c
    ${FW}/components/apps/tui/ls_map.c
    ${FW}/components/apps/tui/ls_map_ink.c
    ${FW}/components/apps/tui/ls_map_marks.c
    ${FW}/components/apps/tui/ls_map_share.c
    ${FW}/components/apps/tui/ls_stroke.c
    ${FW}/components/apps/tui/screens/ext/scr_falls.c
    ${FW}/components/apps/tui/ls_wf_source.c
    ${FW}/components/apps/tui/screens/ext/scr_mesh.c
    ${FW}/components/apps/tui/screens/ext/scr_radios.c
    tools/lssim_mesh.c
    ${FW}/components/meshcore/ls_mesh_inspect.c
    ${PMT}/upstream/zeromesh_pmtiles.c
    ${CARTO}/src/carto.c
    ${CARTO}/src/framebuffer.c
    ${CARTO}/src/geom.c
    ${CARTO}/src/mvt.c
    ${CARTO}/src/raster.c
    ${CARTO}/src/style.c
    ${FW}/components/apps/tui/ls_value_builtin.c
    ${FW}/components/apps/tui/ls_action_builtin.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
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
target_compile_definitions(lssim PRIVATE LS_CALLS_HOST_AUDIO=1 CALL_ARCHIVE_HOST_TEST=1
    CONFIG_LS_BOARD_T_DISPLAY_P4=1
    LS_WF_SOURCE_REAL
    LS_MAP_AUDIT
    LSSIM_PMTILES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/carto/franklin_z12.pmtiles")
target_sources(lssim PRIVATE ${FW}/components/lakeshark/core/ls_time.c
    ${FW}/components/lakeshark/dsp/lr_iq_spectrum.c
    ${FW}/components/lakeshark/dsp/spectrum.c)
target_compile_definitions(lssim PRIVATE LS_TIME_HOST=1)
target_link_libraries(lssim m)

# Runs the lssim above with -C, which links everything the board registers.
ls_add_test(test_lssim_census)
add_dependencies(test_lssim_census lssim)
target_compile_definitions(test_lssim_census PRIVATE LSSIM_EXE="$<TARGET_FILE:lssim>")

# lssim --term driven like a terminal: a key opens FM, a click opens SET, Ctrl-] quits and restores.
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_lssim_term_key COMMAND "${Python3_EXECUTABLE}" -I
             "${CMAKE_CURRENT_SOURCE_DIR}/tools/term_check.py" "$<TARGET_FILE:lssim>" home --keys 2 --expect "LAKESHARK  FM")
    add_test(NAME test_lssim_term_tap COMMAND "${Python3_EXECUTABLE}" -I
             "${CMAKE_CURRENT_SOURCE_DIR}/tools/term_check.py" "$<TARGET_FILE:lssim>" home --tap 70:1 --expect "-SETTINGS-")
endif()

# MESH driven through lssim: taps, keys and what the mesh stub was asked to send.
ls_add_test(test_lssim_mesh)
add_dependencies(test_lssim_mesh lssim)
target_compile_definitions(test_lssim_mesh PRIVATE LSSIM_EXE="$<TARGET_FILE:lssim>")
ls_add_test(test_lssim_mesh_inspect)
add_dependencies(test_lssim_mesh_inspect lssim)
target_compile_definitions(test_lssim_mesh_inspect PRIVATE LSSIM_EXE="$<TARGET_FILE:lssim>")

ls_add_test(test_lssim_radio_fixtures)
add_dependencies(test_lssim_radio_fixtures lssim)
target_compile_definitions(test_lssim_radio_fixtures PRIVATE
    LSSIM_EXE="$<TARGET_FILE:lssim>"
    RADIO_RENDER="${CMAKE_CURRENT_BINARY_DIR}/radio-fixtures.bmp")

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
    ${FW}/components/apps/tui/ls_tui.c ${FW}/components/apps/tui/ls_font_cyr.c
    ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_font_list.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_tui_transform PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)

ls_add_test(test_tui_touch ${FW}/components/apps/tui/ls_tui_touch.c)
target_include_directories(test_tui_touch PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)

ls_add_test(test_tui_button_centre
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
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
    ${FW}/components/apps/tui/ls_tui.c ${FW}/components/apps/tui/ls_font_cyr.c
    ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_tui_chrome.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/screens/main/scr_p25.c
    ${APP}/p25/p25_tg_observed.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_tui_render_cost PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/board
    ${APP}/p25
    ${FW}/bench/shims)

ls_add_test(test_lora_sx1262
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lora_sx1262 PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_lora_sx1262 PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_lr20xx_proto
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lr20xx_proto PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_lr20xx_proto PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_lr20xx_clock
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lr20xx_clock PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_lr20xx_clock PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_lora_concurrency
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lora_concurrency PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_lora_concurrency PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_lora_chip_detect
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lora_chip_detect PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_lora_chip_detect PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

# Where the LoRa radio is wired: the port itself, then the driver with a
# module on the header (issue 11).
ls_add_test(test_lora_port ${FW}/components/lakeshark/board/ls_lora_port.c)
target_include_directories(test_lora_port PRIVATE
    ${FW}/components/lakeshark/board ${FW}/components/lakeshark/core ${FW}/bench/shims)
target_compile_definitions(test_lora_port PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
ls_add_test(test_lora_port_driver
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_include_directories(test_lora_port_driver PRIVATE
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
target_compile_definitions(test_lora_port_driver PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

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

ls_add_test(test_gps_storage ${FW}/components/lakeshark/core/ls_track.c)
target_include_directories(test_gps_storage PRIVATE
    ${FW}/bench/tests/gps_shims
    ${FW}/components/lakeshark/core
    ${FW}/components/lakeshark/board)

ls_add_test(test_rlog ${FW}/components/lakeshark/core/ls_rlog.c)
# The 7200-point wrap test checks ordering; fault tests cover durable completion.
if(WIN32)
    target_compile_definitions(test_rlog PRIVATE LS_RLOG_TEST_SYNC=1)
endif()
target_include_directories(test_rlog PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_wordmark
    ${FW}/components/apps/tui/ls_wordmark.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
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

ls_add_test(test_gps_nmea ${FW}/components/lakeshark/board/ls_gps.c
    ${FW}/components/lakeshark/core/ls_uart_claim.c)
target_include_directories(test_gps_nmea PRIVATE
    ${FW}/components/lakeshark/core
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

ls_add_test(test_adsb_df18
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c)
target_include_directories(test_adsb_df18 PRIVATE
    ${APP}/adsb
    ${FW}/components/lakeshark/audio
    ${FW}/bench/shims)

ls_add_test(test_adsb_status
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c)
target_include_directories(test_adsb_status PRIVATE
    ${APP}/adsb
    ${FW}/components/lakeshark/audio
    ${FW}/bench/shims)

ls_add_test(test_modes_chips
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c)
target_include_directories(test_modes_chips PRIVATE
    ${APP}/adsb
    ${FW}/components/lakeshark/audio
    ${FW}/bench/shims)

ls_add_test(test_adsb_source
    ${APP}/adsb/adsb_source.c
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c)
target_include_directories(test_adsb_source PRIVATE
    ${APP}/adsb
    ${FW}/components/lakeshark/audio
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)

ls_add_test(test_adsb_cpr
    ${APP}/adsb/adsb_demo.c
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c
    ${FW}/components/apps/tui/ls_follow.c
    ${FW}/components/apps/tui/ls_map_motion.c)
target_include_directories(test_adsb_cpr PRIVATE
    ${APP}/adsb
    ${FW}/components/apps/tui
    ${FW}/components/lakeshark/audio
    ${FW}/bench/shims)

ls_add_test(test_tui_render_dump
    ${LS_TUI_APP_SRCS}
    ${FW}/components/apps/tui/ls_tui.c ${FW}/components/apps/tui/ls_font_cyr.c
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
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/apps/tui/screens/ext/scr_update.c
    ${FW}/components/apps/tui/ls_voice_opts.c
    ${FW}/components/apps/tui/screens/ext/scr_rec.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_sources(test_tui_render_dump PRIVATE ${FW}/components/apps/tui/screens/ext/scr_subghz.c)
target_sources(test_tui_render_dump PRIVATE tools/lssim_field.c)
target_sources(test_tui_p25_screen PRIVATE tools/lssim_field.c)
target_sources(test_tui_render_cost PRIVATE tools/lssim_field.c)
target_sources(test_tui_render_cost PRIVATE shims/ls_lora_chip_weak.c)
target_include_directories(test_tui_render_dump PRIVATE
    ${LS_TUI_APP_INCLUDES}
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/board
    ${FW}/components/lakeshark/core
    ${APP}/p25
    ${FW}/bench/shims)
target_sources(test_tui_render_dump PRIVATE
    ${FW}/components/apps/tui/screens/ext/scr_experiments.c
    ${FW}/components/apps/tui/ls_experiments.c
    ${FW}/components/apps/tui/ls_notes.c
    ${FW}/components/apps/tui/experiments/exp_lr433.c
    ${FW}/components/apps/tui/experiments/lr433_dec.c
    ${FW}/components/apps/tui/experiments/sensor_history.c
    ${FW}/components/apps/tui/experiments/lr433_history.c
    fixtures/sensor_ui_fake.c)
target_compile_definitions(test_tui_render_dump PRIVATE
    LS_SENSOR_HISTORY_TEST=1
    LS_SENSOR_RENDER_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    LS_SENSOR_ROOT="${CMAKE_CURRENT_BINARY_DIR}/sensor-history-render")
target_compile_definitions(test_tui_render_dump PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

ls_add_test(test_pocsag  ${APP}/fm/pocsag.c)
ls_add_test(test_pocsag_stream ${APP}/fm/pocsag.c
    ${FW}/components/apps/tui/experiments/pager_recon.c)
target_include_directories(test_pocsag_stream PRIVATE
    ${FW}/components/apps/tui/experiments)
ls_add_test(test_fm_mode_label ${APP}/fm/fm_mode_label.c)
ls_add_test(test_fm_mode_handoff ${APP}/fm/fm_mode_handoff.c)
ls_add_test(test_flipper_fm_command
    ${FW}/main/flipper_link.c
    ${FW}/main/flipper_link_aux.c
    ${APP}/fm/fm_mode_label.c)
target_compile_options(test_flipper_fm_command PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/shims/flipper_link_host.h)
target_include_directories(test_flipper_fm_command PRIVATE
    ${FW}/components/apps
    ${FW}/components/apps/tui
    ${FW}/components/lakeshark
    ${FW}/components/lakeshark/audio
    ${FW}/components/meshcore
    ${FW}/components/lakeshark/board
    ${FW}/components/lakeshark/core)
ls_add_test(test_fm_lifecycle ${APP}/fm/fm_lifecycle.c)
ls_add_test(test_fm_receive ${APP}/fm/fm_mode_handoff.c)
ls_add_test(test_fm_rx_worker
    ${APP}/fm/fm_rx_worker.c
    ${FW}/components/lakeshark/core/radio_decode_worker.c)
ls_add_test(test_flex ${APP}/fm/flex.c fixtures/flex_gen.c)
ls_add_test(test_rec_storage_label ${APP}/rec/rec_storage_label.c)
ls_add_test(test_rec_unique_name ${APP}/rec/rec_unique_name.c)
ls_add_test(test_files_safety ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/ls_tui_ui.c ${FW}/components/apps/tui/ls_icons.c)
target_include_directories(test_files_safety BEFORE PRIVATE
    ${FW}/bench/tests/files_shims ${LS_TUI_APP_INCLUDES})
target_compile_options(test_files_safety PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_files_safety PRIVATE -Wl,--gc-sections)

ls_add_test(test_subghz_nrz_callers ${APP}/subghz/subghz_nrz.c)
ls_add_test(test_rec_list_format ${APP}/rec/rec_list_format.c)
ls_add_test(test_rec_sidecar     ${APP}/rec/rec_sidecar.c)
ls_add_test(test_df_fit ${FW}/components/apps/tui/ls_df_fit.c ${FW}/components/apps/tui/ls_df.c)
target_include_directories(test_df_fit PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_df_band ${FW}/components/apps/tui/ls_df_band.c)
target_include_directories(test_df_band PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_df_cal ${FW}/components/apps/tui/ls_df_cal.c)
target_include_directories(test_df_cal PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_df_hits ${FW}/components/apps/tui/ls_df_hits.c)
target_include_directories(test_df_hits PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_df_beacon ${FW}/components/apps/tui/ls_df_beacon.c)
target_include_directories(test_df_beacon PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_gauge_soc)
target_include_directories(test_gauge_soc PRIVATE ${FW}/components/lakeshark/board)
ls_add_test(test_tdp4_power ${FW}/components/lakeshark/board/ls_board_hw.c)
target_compile_definitions(test_tdp4_power PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
ls_add_test(test_hosted_board ${FW}/main/ls_hosted_board.c)
target_compile_definitions(test_hosted_board PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
target_include_directories(test_hosted_board PRIVATE
    ${FW}/main ${FW}/bench/shims/hosted_gpio
    ${FW}/managed_components/espressif__esp_hosted/host)
ls_add_test(test_rec_scout_span  ${APP}/rec/rec_scout_span.c)
ls_add_test(test_rec_space       ${APP}/rec/rec_space.c)
ls_add_test(test_rec_lr_iq ${APP}/rec/rec_lr_iq.c ${APP}/rec/rec_lr_ook.c
    ${APP}/rec/rec_ook_packet.c ${APP}/rec/rec_sidecar.c ${APP}/subghz/subghz_file.c
    ${FW}/components/lakeshark/dsp/fsk_packet.c
    ${APP}/rec/rec_unique_name.c
    ${FW}/components/lakeshark/board/ls_lora.c
    ${FW}/components/lakeshark/board/ls_lora_port.c
    ${FW}/components/lakeshark/board/ls_lora_sx126x.c
    ${FW}/components/lakeshark/board/ls_lora_lr20xx.c
    ${FW}/components/lakeshark/board/ls_spi.c)
target_compile_definitions(test_rec_lr_iq PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
ls_add_test(test_rec_lr_ook ${APP}/rec/rec_lr_ook.c)
target_include_directories(test_rec_lr_ook PRIVATE ${APP}/rec)
ls_add_test(test_rec_fsk ${APP}/rec/rec_fsk.c
    ${FW}/components/lakeshark/radio/ls_sub_fsk.c
    ${FW}/components/lakeshark/radio/ls_fsk_capture.c)
target_include_directories(test_rec_fsk PRIVATE ${FW}/components/lakeshark/radio)
# 2-FSK packets (preamble, sync, CRC) from LR2021 I/Q and from REC's slicer.
ls_add_test(test_fsk_packet ${FW}/components/lakeshark/dsp/fsk_packet.c ${APP}/rec/rec_fsk.c)
target_include_directories(test_fsk_packet PRIVATE ${FW}/components/lakeshark/dsp)
# The export writes a 2-FSK preset for a demodulated capture, so it needs
# the CC1101 register arithmetic that builds one.
ls_add_test(test_rec_watch ${APP}/rec/rec_watch.c ${APP}/rec/rec_watch_store.c ${APP}/rec/rec_ook24.c
    ${FW}/components/lakeshark/radio/ls_sub_fsk.c
    ${FW}/components/lakeshark/radio/ls_fsk_capture.c)
target_include_directories(test_rec_watch PRIVATE ${FW}/components/lakeshark/radio)
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
ls_add_test(test_p25_lr_iq ${APP}/p25/p25_lr_iq.c)
ls_add_test(test_p25_lr_frontend ${APP}/p25/p25_lr_frontend.c)
add_executable(lr2021_lab tools/lr2021_lab.c)
target_link_libraries(lr2021_lab PRIVATE m)
add_test(NAME lr2021_offline_lab COMMAND lr2021_lab selftest)
add_executable(test_lr2021_iq_queue tests/test_lr2021_iq_queue.c ${APP}/p25/lr2021_iq_queue.c)
target_include_directories(test_lr2021_iq_queue PRIVATE ${APP}/p25)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_lr2021_iq_queue PRIVATE Threads::Threads)
endif()
add_test(NAME lr2021_stream_queue COMMAND test_lr2021_iq_queue)
set_tests_properties(lr2021_stream_queue PROPERTIES TIMEOUT 20)
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
add_executable(lr2021_p25_replay tools/lr2021_p25_replay.c ${APP}/p25/p25_lr_iq.c ${APP}/p25/p25_lr_frontend.c ${_p25_replay_sources})
target_link_libraries(lr2021_p25_replay ls_harness m)
target_include_directories(lr2021_p25_replay PRIVATE ${FW}/components/imbe_vocoder)
target_compile_options(lr2021_p25_replay PRIVATE -ffunction-sections -fdata-sections)
target_link_options(lr2021_p25_replay PRIVATE -Wl,--gc-sections)
add_test(NAME lr2021_p25_rate_bridge COMMAND lr2021_p25_replay selftest)
set_tests_properties(lr2021_p25_rate_bridge PROPERTIES TIMEOUT 30)
add_test(NAME lr2021_p25_frontend_benchmark COMMAND lr2021_p25_replay benchmark)
set_tests_properties(lr2021_p25_frontend_benchmark PROPERTIES TIMEOUT 60)
ls_add_test(test_p25_tsdu ${APP}/p25/dsd_frame.c ${APP}/p25/p25_tsbk.c
    ${APP}/p25/p25_ess.c ${APP}/p25/p25_lcw.c)
ls_add_test(test_p25_tsbk ${APP}/p25/p25_tsbk.c
    ${APP}/p25/grant_follower.c ${APP}/p25/p25_ess.c
    ${APP}/p25/p25_controls.c)
ls_add_test(test_p25_split ${APP}/p25/p25_split.c ${APP}/p25/p25_tsbk.c
    ${APP}/p25/scan_ctrl.c ${APP}/p25/grant_follower.c
    ${APP}/p25/p25_ess.c ${APP}/p25/p25_controls.c)
ls_add_test(test_p25_ess ${APP}/p25/p25_ess.c)
ls_add_test(test_p25_voice_hold ${APP}/p25/p25_voice_hold.c)
ls_add_test(test_p25_sync_confirm ${APP}/p25/p25_sync_confirm.c)
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
ls_add_test(test_p25_program ${APP}/p25/p25_program.c ${APP}/p25/p25_profile.c ${APP}/p25/p25_geo.c
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
ls_add_test(test_music_transport)
ls_add_test(test_pixel_wf
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/ls_tui.c ${FW}/components/apps/tui/ls_font_cyr.c
    ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_font_list.c
    ${FW}/components/apps/tui/ls_theme.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_pixel_wf PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/board
    ${FW}/bench/shims)
ls_add_test(test_music_spectro)
target_include_directories(test_music_spectro PRIVATE ${FW}/components/apps/tui)
ls_add_test(test_music_backend ${FW}/components/apps/tui/ls_music_backend.c
    ${FW}/components/lakeshark/audio/call_store.c
    ${FW}/components/lakeshark/audio/call_recorder.c
    ${FW}/components/lakeshark/audio/call_archive.c)
target_include_directories(test_music_backend BEFORE PRIVATE
    ${FW}/components/chmorgan__esp-audio-player/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shims/music
    ${FW}/components/apps/tui ${FW}/components/lakeshark)
target_link_options(test_music_backend PRIVATE -Wl,--wrap=fopen -Wl,--wrap=fclose -Wl,--wrap=fseek -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=audio_player_get_state)
ls_add_test(test_wav_decoder ${FW}/components/chmorgan__esp-audio-player/audio_wav.cpp)
target_include_directories(test_wav_decoder PRIVATE ${FW}/components/chmorgan__esp-audio-player)
target_include_directories(test_music_transport PRIVATE ${FW}/components/apps/tui)
ls_add_test(test_tui_music
    ${FW}/components/apps/tui/ls_audio_view.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    ${FW}/components/apps/tui/screens/ext/scr_music.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_tui_music PRIVATE ${LS_TUI_APP_INCLUDES})
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
target_compile_definitions(test_subghz_file PRIVATE
    CC1101_FSK_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/cc1101-fsk-ch2-t99.sub")
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
find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_compass_quality_ui COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compass_quality_ui.py" "$<TARGET_FILE:lssim>")
    add_test(NAME test_compass_accuracy_io COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compass_accuracy_io.py")
    add_test(NAME test_flash_polling COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flash_polling.py")
endif()
ls_add_test(test_wifi_file_stream)
ls_add_test(test_ls_sweep ${FW}/components/lakeshark/dsp/ls_sweep_core.c)
target_include_directories(test_ls_sweep PRIVATE ${FW}/components/lakeshark/dsp)
ls_add_test(test_ls_search_core ${FW}/components/lakeshark/dsp/ls_search_core.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c)
target_include_directories(test_ls_search_core PRIVATE ${FW}/components/lakeshark/dsp)
ls_add_test(test_cell_core ${APP}/cell/cell_core.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c)
target_include_directories(test_cell_core PRIVATE ${APP}/cell ${FW}/components/lakeshark/dsp)
ls_add_test(test_cell_scan ${APP}/cell/cell_core.c)
target_include_directories(test_cell_scan PRIVATE ${APP}/cell)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_cell_review COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cell_review.py")
    add_test(NAME test_cell_worker COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cell_worker.py")
endif()
ls_add_test(test_cell_sweep ${FW}/components/lakeshark/dsp/ls_sweep.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c)
target_include_directories(test_cell_sweep PRIVATE ${FW}/components/lakeshark/dsp ${FW}/components/lakeshark/radio)
ls_add_test(test_cell_monitor ${APP}/cell/cell_core.c ${FW}/components/lakeshark/dsp/ls_sweep_core.c)
target_include_directories(test_cell_monitor PRIVATE ${APP}/cell ${FW}/components/lakeshark/dsp ${FW}/components/lakeshark/radio ${FW}/components/lakeshark/board)
ls_add_test(test_cell_report ${APP}/cell/cell_report_core.c)
target_include_directories(test_cell_report PRIVATE ${APP}/cell)
ls_add_test(test_lte_sync ${APP}/cell/lte_sync.c)
target_include_directories(test_lte_sync PRIVATE ${APP}/cell)
ls_add_test(test_lte_resample ${APP}/cell/lte_resample.c)
target_include_directories(test_lte_resample PRIVATE ${APP}/cell)
ls_add_test(test_lte_mib ${APP}/cell/lte_mib.c)
target_include_directories(test_lte_mib PRIVATE ${APP}/cell)
ls_add_test(test_lte_cells)
target_include_directories(test_lte_cells PRIVATE ${APP}/cell)
ls_add_test(test_lte_crs)
target_include_directories(test_lte_crs PRIVATE ${APP}/cell)
ls_add_test(test_lte_sync_full ${APP}/cell/lte_sync.c)
target_include_directories(test_lte_sync_full PRIVATE ${APP}/cell)
target_compile_definitions(test_lte_sync_full PRIVATE LTE_SYNC_SEARCH_SAMPLES=57600)
ls_add_test(test_ble_link ${FW}/main/ble_link_core.c)
target_include_directories(test_ble_link PRIVATE ${FW}/main)
ls_add_test(test_flipper_telemetry_contract
    ${FW}/main/flipper_link_telemetry.c
    ${APP}/fm/fm_mode_label.c
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
ls_add_test(test_media_playlist
    ${FW}/components/apps/tui/media_playlist.c)
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
ls_add_test(test_radio_ppm
    ${FW}/components/lakeshark/radio/radio_endpoint.c
    ${FW}/components/lakeshark/radio/radio_health.c)
ls_add_test(test_usb_port_cycle
    ${FW}/components/lakeshark/radio/usb_port_cycle.c)
# boot recovery cycles the port for a device that is attached and silent,
# and leaves an empty port alone.
ls_add_test(test_usb_boot_retry
    ${FW}/components/lakeshark/radio/usb_boot_retry.c)
# FM's radio open bounded for io, lifted by an endpoint change.
ls_add_test(test_radio_open_retry
    ${FW}/components/lakeshark/radio/radio_open_retry.c)
ls_add_test(test_usb_stream_start
    ${FW}/components/lakeshark/radio/esp_libusb.c)
target_link_options(test_usb_stream_start PRIVATE
    -Wl,--wrap=calloc -Wl,--wrap=free -Wl,--wrap=xSemaphoreCreateBinary
    -Wl,--wrap=xSemaphoreCreateMutex -Wl,--wrap=vSemaphoreDelete)
ls_add_test(test_receiver_diagnostics
    ${FW}/components/lakeshark/radio/receiver_diagnostics.c)
ls_add_test(test_iq_control_preemption ${FW}/components/lakeshark/radio/iq_app_control.c)
target_include_directories(test_iq_control_preemption BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shims/iq_critical)
ls_add_test(test_iq_app_control
    ${FW}/components/lakeshark/radio/iq_app_control.c
    ${FW}/components/lakeshark/radio/radio_endpoint.c)
target_include_directories(test_media_playlist PRIVATE ${FW}/components/apps/tui)
target_compile_definitions(test_media_playlist PRIVATE
    CONFIG_AUDIO_PLAYER_ENABLE_MP3=1
    CONFIG_AUDIO_PLAYER_ENABLE_WAV=1)
ls_add_test(test_player_state
    ${FW}/components/bsp_extra/src/bsp_extra_player_state.c)


ls_add_test(test_player_open
    ${FW}/components/bsp_extra/src/bsp_extra_player_open.c
    ${FW}/components/bsp_extra/src/bsp_extra_player_state.c
    ${FW}/managed_components/chmorgan__esp-file-iterator/file_iterator.c)
target_compile_options(test_player_open PRIVATE -Wno-format)
target_link_options(test_player_open PRIVATE
    -Wl,--wrap=fopen
    -Wl,--wrap=fclose)


ls_add_test(test_media_supported
    ${FW}/components/apps/tui/media_playlist.c)
target_include_directories(test_media_supported PRIVATE ${FW}/components/apps/tui)
target_compile_definitions(test_media_supported PRIVATE
    CONFIG_AUDIO_PLAYER_ENABLE_MP3=1
    CONFIG_AUDIO_PLAYER_ENABLE_WAV=1)








ls_add_test(test_usb_autoreboot_pref
    ${FW}/components/lakeshark/core/usb_autoreboot_pref.c)
target_include_directories(test_usb_autoreboot_pref PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_p25_ldu2_ess ${APP}/p25/p25p1_ldu2.c ${APP}/p25/p25_ess.c)
ls_add_test(test_p25_os4_decode ${APP}/p25/p25_os4_decode.c ${APP}/p25/p25_os4_lut.c
    ${APP}/p25/p25_os4_nn.c ${APP}/p25/p25_os4_nn_model.c)
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

ls_add_test(test_ls_errlog
    ${FW}/components/lakeshark/core/ls_errlog.c)
target_include_directories(test_ls_errlog PRIVATE
    ${FW}/components/lakeshark/core)

ls_add_test(test_ls_term
    ${FW}/components/apps/tui/ls_term_scroll.c)
target_include_directories(test_ls_term PRIVATE
    ${FW}/components/apps/tui)

ls_add_test(test_ls_term_runner ${FW}/components/apps/tui/ls_term_scroll.c)
target_include_directories(test_ls_term_runner BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/term_shims
    ${FW}/components/apps/tui)

ls_add_test(test_ls_errlog_esp
    ${FW}/components/lakeshark/core/ls_errlog.c
    ${FW}/components/lakeshark/core/ls_errlog_esp.c)
target_include_directories(test_ls_errlog_esp BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/errlog_shims
    ${FW}/components/lakeshark/core)

ls_add_test(test_ls_crash_esp
    ${FW}/components/lakeshark/core/ls_crash.c
    ${FW}/components/lakeshark/core/ls_crash_esp.c)
target_include_directories(test_ls_crash_esp BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/crash_shims
    ${FW}/components/lakeshark/core)
target_compile_definitions(test_ls_crash_esp PRIVATE CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=1)

ls_add_test(test_ui_language
    ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/tuilib/tui_core.c
    ${FW}/components/apps/tui/ls_font_cyr.c
    ${FW}/components/apps/tui/ls_font_mono_14.c
    ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_24.c
    ${FW}/components/lakeshark/core/settings_schema.c)
target_include_directories(test_ui_language PRIVATE
    ${FW}/components/apps/tui
    ${FW}/components/apps/tui/tuilib
    ${FW}/components/lakeshark/core
    ${FW}/bench/shims)

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
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_tui_contrast PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_fm_tone ${APP}/fm/fm_tone.c)
ls_add_test(test_fm_am ${APP}/fm/fm_dsp.c)
# A diagnostic, like p25_iq_replay: an FM-rate IQ capture through NFM and POCSAG.
add_executable(fm_iq_replay tools/fm_iq_replay.c ${APP}/fm/pocsag.c ${APP}/fm/fm_dsp.c)
target_link_libraries(fm_iq_replay ls_harness m)

ls_add_test(test_field ${FW}/components/apps/tui/ls_field.c ${FW}/components/apps/tui/ls_compass.c ${FW}/components/apps/tui/ls_compass_learn.c ${FW}/components/apps/tui/ls_notes.c)
ls_add_test(test_labs_limits)
target_include_directories(test_labs_limits PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${FW}/components/meshcore)
ls_add_test(test_labs_numbers)
target_include_directories(test_labs_numbers PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${FW}/components/meshcore)
# Only the numpad callback is linked; the screen's drawing dependencies are unused.
target_compile_options(test_labs_numbers PRIVATE -fwhole-program -ffunction-sections -fdata-sections)
target_link_options(test_labs_numbers PRIVATE -Wl,--gc-sections)
target_include_directories(test_field PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
ls_add_test(test_wmm ${FW}/components/apps/tui/ls_wmm.c)
target_include_directories(test_wmm PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_compass_art ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
target_include_directories(test_compass_art PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_compass_live ${FW}/components/apps/tui/ls_compass_live.c ${FW}/components/apps/tui/ls_compass.c ${FW}/components/apps/tui/ls_wmm.c tests/stubs_compass_live.c)
target_include_directories(test_compass_live PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
ls_add_test(test_compass_learn ${FW}/components/apps/tui/ls_compass_learn.c ${FW}/components/apps/tui/ls_compass.c)
ls_add_test(test_compass_accuracy ${FW}/components/apps/tui/ls_compass_live.c
    ${FW}/components/apps/tui/ls_compass.c ${FW}/components/apps/tui/ls_compass_learn.c
    ${FW}/components/apps/tui/ls_wmm.c tests/stubs_compass_live.c)
target_include_directories(test_compass_accuracy PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_definitions(test_compass_accuracy PRIVATE COMPASS_TRACE="${FW}/bench/tests/data/compass_trace_lr.txt")
target_include_directories(test_compass_learn PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
ls_add_test(test_df ${FW}/components/apps/tui/ls_df.c ${FW}/components/apps/tui/ls_compass_live.c ${FW}/components/apps/tui/ls_compass.c ${FW}/components/apps/tui/ls_wmm.c tests/stubs_compass_live.c)
target_include_directories(test_df PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
ls_add_test(test_ble_heard ${FW}/components/apps/tui/ls_ble_heard.c)
target_include_directories(test_ble_heard PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_sun ${FW}/components/apps/tui/ls_sun.c)
target_include_directories(test_sun PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_textbuf ${FW}/components/apps/tui/ls_textbuf.c)
ls_add_test(test_notes ${FW}/components/apps/tui/ls_notes.c)
target_include_directories(test_notes PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_definitions(test_notes PRIVATE LS_NOTES_TEST=1)
ls_add_test(test_lssim_notes)
add_dependencies(test_lssim_notes lssim)
target_compile_definitions(test_lssim_notes PRIVATE LSSIM_EXE="$<TARGET_FILE:lssim>")
ls_add_test(test_note_blocks ${FW}/components/apps/tui/ls_note_blocks.c ${FW}/components/apps/tui/ls_notes.c ${FW}/components/apps/tui/ls_value.c)
target_include_directories(test_note_blocks PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_definitions(test_note_blocks PRIVATE LS_NOTES_TEST=1)
target_include_directories(test_textbuf PRIVATE ${LS_TUI_APP_INCLUDES})
ls_add_test(test_compass ${FW}/components/apps/tui/ls_compass.c)
target_include_directories(test_compass PRIVATE ${LS_TUI_APP_INCLUDES})
target_compile_definitions(test_field PRIVATE LS_FIELD_TEST=1)

ls_add_test(test_map_motion ${FW}/components/apps/tui/ls_map_motion.c)
target_include_directories(test_map_motion PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_adsb_follow ${FW}/components/apps/tui/ls_follow.c)
target_include_directories(test_adsb_follow PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_map_marks
    ${FW}/components/apps/tui/ls_map_marks.c
    ${FW}/components/apps/tui/ls_geo.c)
target_include_directories(test_map_marks PRIVATE
    ${FW}/components/apps/tui ${FW}/bench/shims)
target_include_directories(test_tui_screens PRIVATE ${CARTO}/include)

ls_add_test(test_nfc_classic ${FW}/components/lakeshark/nfc/nfc_classic.c)

add_executable(test_p25_phase2 tests/test_p25_phase2.cpp)
target_link_libraries(test_p25_phase2 p25_phase2_host ls_harness m)
add_test(NAME test_p25_phase2 COMMAND test_p25_phase2)
ls_add_test(test_p25_phase2_dsp ${APP}/p25/dsp_pipeline.c ${APP}/p25/p25_cqpsk_controls.c fixtures/lsm_gen.c)
target_compile_definitions(test_p25_phase2_dsp PRIVATE LSM_GEN_BAUD=6000)

foreach(target test_tui_picker test_radio_select test_options test_tui_wireless test_tui_waterfall_claim
               test_tui_waterfall_scale test_tui_quick test_tui_waterfall_stall
               test_tui_keyboard test_scan_shell)
    target_compile_definitions(${target} PRIVATE LS_TUI_CORNER_STUB=1)
endforeach()
foreach(target test_tui_picker test_radio_select test_options test_tui_quick test_tui_keyboard
               test_scan_shell)
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
ls_add_test(test_touch_idle ${FW}/components/lakeshark/board/ls_touch.c)
target_compile_definitions(test_touch_idle PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
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
    ${APP}/p25/p25_symbol_synth.c
    ${APP}/p25/imbe_chase.c
    ${FW}/components/mbelib/mbelib.c
    ${FW}/components/mbelib/ecc.c
    ${FW}/components/mbelib/imbe7200x4400.c
    ${P25_IMBE_TEST_SOURCES}
    ${FW}/components/imbe_vocoder/imbe_shim.cpp)
add_executable(p25_voice_replay tools/p25_voice_replay.c ${APP}/p25/p25_lr_frontend.c ${P25_VOICE_REPLAY_SOURCES})
ls_add_test(test_mbe_hamming ${FW}/components/mbelib/ecc.c)
ls_add_test(test_imbe_chase ${APP}/p25/imbe_chase.c ${FW}/components/mbelib/ecc.c)
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

# Phase II from IQ: the public recording re-modulated at 6000 baud through
# dsp_pipeline, p25_p2_slice and the decoder. Needs LS_P25P2_CAPTURE, which
# bench/tools/check_p25p2_sample.py sets; passes with a note without it.
add_executable(test_p25_phase2_iq tests/test_p25_phase2_iq.c
    ${APP}/p25/dsp_pipeline.c ${APP}/p25/p25_cqpsk_controls.c
    ${APP}/p25/p25_p2_slice.c fixtures/lsm_gen.c)
target_compile_definitions(test_p25_phase2_iq PRIVATE LSM_GEN_BAUD=6000)
target_link_libraries(test_p25_phase2_iq p25_phase2_host ls_harness m)
add_test(NAME test_p25_phase2_iq COMMAND test_p25_phase2_iq)

# Phase II follow policy; recording cases need LS_P25P2_CAPTURE.
add_executable(test_p25_p2_follow tests/test_p25_p2_follow.c
    ${APP}/p25/p25_p2_follow.c)
target_include_directories(test_p25_p2_follow PRIVATE ${APP}/p25)
target_link_libraries(test_p25_p2_follow p25_phase2_host ls_harness m)
add_test(NAME test_p25_p2_follow COMMAND test_p25_p2_follow)

ls_add_test(test_speech_level
    ${FW}/components/lakeshark/audio/speech_engine.cpp
    ${FW}/components/lakeshark/audio/audio_eq.c)
target_include_directories(test_speech_level PRIVATE
    ${FW}/components/lakeshark/audio ${FW}/components/lakeshark/core)
target_link_libraries(test_speech_level klatt_tts_host)

foreach(exp_id aviation uat)
    ls_add_test(test_exp_${exp_id}
        $<IF:$<STREQUAL:${exp_id},uat>,${APP}/adsb/uat_decode.c,${FW}/components/apps/tui/experiments/${exp_id}_decode.c>
        ${FW}/components/apps/tui/experiments/exp_${exp_id}.c
        ${FW}/components/apps/tui/ls_experiments.c)
    target_include_directories(test_exp_${exp_id} PRIVATE ${LS_TUI_APP_INCLUDES}
        ${FW}/components/apps/tui/experiments)
endforeach()

foreach(exp_id loramon zwave wisun)
    ls_add_test(test_exp_${exp_id} ${FW}/components/apps/tui/experiments/lorafam_decode.c
        ${FW}/components/apps/tui/experiments/exp_${exp_id}.c
        fixtures/lorafam_module_fake.c)
    target_include_directories(test_exp_${exp_id} PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/apps/tui/experiments)
endforeach()

ls_add_test(test_same ${APP}/fm/same.c ${APP}/fm/same_store.c)

ls_add_test(test_same_notice ${APP}/fm/same.c ${APP}/fm/same_store.c)
target_include_directories(test_same_notice PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark)
target_compile_options(test_same_notice PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_same_notice PRIVATE -Wl,--gc-sections)

set_source_files_properties(tests/test_same_notice.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")

ls_add_test(test_aprs ${APP}/fm/aprs.c ${APP}/fm/aprs_store.c ${APP}/fm/fm_dsp.c)
ls_add_test(test_aprs_mode_action ${APP}/fm/fm_mode_label.c)
target_include_directories(test_aprs_mode_action PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${CARTO}/include)
target_compile_options(test_aprs_mode_action PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_aprs_mode_action PRIVATE -Wl,--gc-sections)
set_source_files_properties(tests/test_aprs_mode_action.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")
ls_add_test(test_call_recorder ${FW}/components/lakeshark/audio/call_recorder.c)
ls_add_test(test_call_store ${FW}/components/lakeshark/audio/call_recorder.c
    ${FW}/components/lakeshark/audio/call_store.c)
ls_add_test(test_call_archive ${FW}/components/lakeshark/audio/call_archive.c
    ${FW}/components/lakeshark/audio/call_recorder.c ${FW}/components/lakeshark/audio/call_store.c)
target_compile_definitions(test_call_archive PRIVATE CALL_ARCHIVE_HOST_TEST=1)
ls_add_test(test_call_archive_idle ${FW}/components/lakeshark/audio/call_archive.c
    ${FW}/components/lakeshark/audio/call_recorder.c ${FW}/components/lakeshark/audio/call_store.c)
target_compile_definitions(test_call_archive_idle PRIVATE CALL_ARCHIVE_HOST_TEST=1)

ls_add_test(test_tui_calls ${FW}/components/apps/tui/ls_calls.c
    ${FW}/components/apps/tui/ls_audio_view.c
    ${FW}/components/apps/tui/ls_pixel_wf.c
    tools/lssim_music.c
    ${FW}/components/apps/tui/ls_icons.c ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_motion.c ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/ls_tui.c ${FW}/components/apps/tui/ls_font_cyr.c ${FW}/components/apps/tui/ls_tui_inset.c
    ${FW}/components/apps/tui/ls_theme.c ${FW}/components/apps/tui/ls_font_list.c
    ${FW}/components/apps/tui/ls_font_mono_14.c ${FW}/components/apps/tui/ls_font_mono_16.c
    ${FW}/components/apps/tui/ls_font_mono_24.c)
target_include_directories(test_tui_calls PRIVATE ${LS_TUI_APP_INCLUDES})
target_compile_definitions(test_tui_calls PRIVATE CALLS_RENDER_TEST=1 LS_CALLS_HOST_AUDIO=1)
ls_add_test(test_survey ${FW}/components/apps/tui/ls_survey.c)
target_include_directories(test_survey PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_survey_runtime ${FW}/main/ls_survey_runtime.c
    ${FW}/components/apps/tui/ls_survey.c
    ${FW}/components/lakeshark/core/ls_track.c)
target_include_directories(test_survey_runtime PRIVATE ${LS_TUI_APP_INCLUDES})
target_compile_definitions(test_survey_runtime PRIVATE
    LS_SURVEY_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}/survey-host")

ls_add_test(test_route ${FW}/components/lakeshark/core/ls_route.c)

ls_add_test(test_route_live
    ${FW}/components/apps/tui/ls_route_live.c
    ${FW}/components/lakeshark/core/ls_route.c)
target_include_directories(test_route_live PRIVATE ${LS_TUI_APP_INCLUDES})

target_link_options(test_route_live PRIVATE -Wl,--wrap=esp_timer_get_time)

foreach(route_ui_target test_tui_router test_tui_p25_screen test_tui_render_cost test_tui_render_dump)
    target_sources(${route_ui_target} PRIVATE shims/ls_route_hw_fake.c)
endforeach()
target_sources(test_tui_screens PRIVATE shims/ls_route_hw_fake.c)
target_compile_definitions(test_tui_screens PRIVATE LS_ROUTE_FAKE_START_ONLY=1)

ls_add_test(test_lssim_route)
add_dependencies(test_lssim_route lssim)
target_compile_definitions(test_lssim_route PRIVATE
    LSSIM_EXE="$<TARGET_FILE:lssim>"
    ROUTE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/route.gpx"
    ROUTE_RENDER="${CMAKE_CURRENT_BINARY_DIR}/route-test.bmp")

foreach(merge_test test_map_layers test_notify_policy test_compass_storage)
    ls_add_test(${merge_test})
    target_include_directories(${merge_test} PRIVATE ${LS_TUI_APP_INCLUDES}
        ${FW}/components/lakeshark ${CARTO}/include)
    target_compile_options(${merge_test} PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(${merge_test} PRIVATE -Wl,--gc-sections)
    set_source_files_properties(tests/${merge_test}.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")
endforeach()

target_sources(test_map_layers PRIVATE ${FW}/components/apps/tui/ls_geo.c ${FW}/components/apps/tui/ls_theme.c ${CARTO}/src/raster.c)

ls_add_test(test_ais ${APP}/fm/ais.c ${APP}/fm/ais_store.c ${APP}/fm/fm_mode_label.c)

ls_add_test(test_lssim_ais)
add_dependencies(test_lssim_ais lssim)
target_compile_definitions(test_lssim_ais PRIVATE
    LSSIM_EXE="$<TARGET_FILE:lssim>"
    AIS_RENDER="${CMAKE_CURRENT_BINARY_DIR}/ais-test.bmp")

ls_add_test(test_dmr_listen ${DMR_WATCH_SRCS})

target_sources(lssim PRIVATE fixtures/dmr_gen.c)

ls_add_test(test_rs41 ${FW}/components/apps/tui/experiments/rs41_decode.c
    ${FW}/components/apps/tui/experiments/rs41_stream.c
    ${FW}/components/apps/tui/experiments/rs41_store.c)
target_include_directories(test_rs41 PRIVATE ${FW}/components/apps/tui/experiments)

target_sources(lssim PRIVATE tools/lssim_rs41.c)

ls_add_test(test_exp_rs41
    ${FW}/components/apps/tui/experiments/exp_rs41.c
    ${FW}/components/apps/tui/experiments/rs41_decode.c
    ${FW}/components/apps/tui/experiments/rs41_stream.c
    ${FW}/components/apps/tui/experiments/rs41_store.c
    ${FW}/components/apps/tui/ls_experiments.c)
target_include_directories(test_exp_rs41 PRIVATE ${LS_TUI_APP_INCLUDES})

ls_add_test(test_lssim_rs41)
add_dependencies(test_lssim_rs41 lssim)
target_compile_definitions(test_lssim_rs41 PRIVATE
    LSSIM_EXE="$<TARGET_FILE:lssim>"
    RS41_RENDER="${CMAKE_CURRENT_BINARY_DIR}/rs41-test.bmp"
    RS41_ROUTE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/route.gpx")

# TILES uses its production screen with explicit transfer fixtures on the host.
target_sources(lssim PRIVATE tools/lssim_tiles.c ${FW}/components/apps/tui/screens/ext/scr_tiles.c ${FW}/components/apps/tui/ls_place_search.c ${FW}/components/apps/tui/ls_globe.c)
target_compile_definitions(lssim PRIVATE LS_TILES_HOST=1)

# Real CartoCore MAP renderer, pinned in-tree unless overridden for development.
set(CARTOCORE_DIR "$ENV{CARTOCORE_DIR}" CACHE PATH "CartoCore engine checkout for host MAP (defaults to vendored snapshot)")
if(NOT CARTOCORE_DIR)
    set(CARTOCORE_DIR "${FW}/components/cartocore/upstream")
endif()
if(CARTOCORE_DIR)
    set(CC_ROOT "${CARTOCORE_DIR}")
    include("${CC_ROOT}/components/cartocore/sources.cmake")
    if(NOT CC_EMBED_API_VERSION EQUAL 2 OR NOT CC_CTILE_FORMAT_MAX EQUAL 7)
        message(FATAL_ERROR "LakeShark CartoCore version mismatch: requires embedded API 2 and CTILE through v7")
    endif()
    add_library(cartocore_host STATIC ${CC_CORE_SRCS} ${CC_OUT_SRCS})
    target_include_directories(cartocore_host PUBLIC ${CC_PUBLIC_INCLUDE_DIRS})
    add_executable(test_carto_budget tests/test_carto_budget.c)
    target_include_directories(test_carto_budget PRIVATE "${FW}/components/apps/tui")
    add_test(NAME test_carto_budget COMMAND test_carto_budget)
    add_executable(test_cartocore_cells "${CC_ROOT}/tests/cells.c" "${CC_ROOT}/src/ingest/host_cellset.c")
    target_link_libraries(test_cartocore_cells cartocore_host m)
    add_executable(test_cartocore_decoded_cache "${CC_ROOT}/tests/decoded_cache.c")
    target_link_libraries(test_cartocore_decoded_cache cartocore_host m)
    add_test(NAME test_cartocore_decoded_cache COMMAND test_cartocore_decoded_cache "${FW}/data/franklin_mini.ctile")
    file(READ "${FW}/data/franklin_mini.ctile" CC_MINI_HEX HEX)
    string(REGEX REPLACE "(..)" "0x\\1," CC_MINI_BYTES "${CC_MINI_HEX}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/lssim_carto_mini.h" "static const uint8_t carto_mini[]={${CC_MINI_BYTES}};\n")
    set(CC_IDF "$ENV{IDF_PATH}")
    if(NOT CC_IDF)
        set(CC_IDF "C:/esp/v5.5.4/esp-idf")
    endif()
    set(CC_MBED "${CC_IDF}/components/mbedtls/mbedtls" CACHE PATH "Host SHA256 implementation")
    add_library(cells_sha STATIC "${CC_MBED}/library/sha256.c" "${CC_MBED}/library/platform_util.c" "${CC_MBED}/library/platform.c")
    target_include_directories(cells_sha PUBLIC "${CC_MBED}/include" "${CMAKE_CURRENT_SOURCE_DIR}/tools")
    target_compile_definitions(cells_sha PUBLIC MBEDTLS_CONFIG_FILE="cells_mbedtls_config.h")
    target_link_libraries(lssim cells_sha)
    target_sources(lssim PRIVATE ${FW}/components/apps/tui/ls_cartocore.c)
    target_include_directories(lssim PRIVATE tools "${CMAKE_CURRENT_BINARY_DIR}")
    set_source_files_properties(${FW}/components/apps/tui/ls_cartocore.c ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c PROPERTIES COMPILE_OPTIONS "-fstack-usage;-fdump-ipa-cgraph;-fno-inline")
    target_compile_definitions(lssim PRIVATE LS_CARTOCORE_HOST=1
        LSSIM_CTILE_DEFAULT="${FW}/data/franklin_mini.ctile")
    target_link_libraries(lssim cartocore_host)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME test_cartocore_cells_fixture COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/carto_cells_fixture.py" "${CC_ROOT}/tests/cells_fixture.py" "${CMAKE_CURRENT_BINARY_DIR}/cells-fixture")
    set_tests_properties(test_cartocore_cells_fixture PROPERTIES FIXTURES_SETUP carto_cells_inputs)
    add_test(NAME test_cartocore_cells COMMAND test_cartocore_cells "${CMAKE_CURRENT_BINARY_DIR}/cells-fixture")
    set_tests_properties(test_cartocore_cells PROPERTIES FIXTURES_REQUIRED carto_cells_inputs)
    add_custom_target(test_carto_sd
        COMMAND "${CMAKE_COMMAND}" -E env
            "CARTOCORE_DIR=${CC_ROOT}"
            "CARTOCORE_LIBRARY=$<TARGET_FILE:cartocore_host>"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_carto_sd.py"
        DEPENDS cartocore_host
        WORKING_DIRECTORY "${FW}"
        COMMENT "Testing production SD loader and archive re-download transactions")
    add_custom_target(test_carto_psram
        COMMAND "${CMAKE_COMMAND}" -E env "CARTOCORE_DIR=${CC_ROOT}"
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_carto_psram.py" "$<TARGET_FILE:lssim>"
        DEPENDS lssim
        WORKING_DIRECTORY "${FW}"
        COMMENT "Testing real MAP renders with board-sized PSRAM")
    # Stack/boundary audit describes production calls. Sanitizer-generated calls
    # belong to the separately instrumented lssim runtime, not this call graph.
    add_library(carto_stack_audit OBJECT
        ${FW}/components/apps/tui/ls_cartocore.c
        ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c)
    target_include_directories(carto_stack_audit PRIVATE
        $<TARGET_PROPERTY:lssim,INCLUDE_DIRECTORIES>)
    target_compile_definitions(carto_stack_audit PRIVATE
        $<TARGET_PROPERTY:lssim,COMPILE_DEFINITIONS>)
    target_link_libraries(carto_stack_audit PRIVATE
        $<TARGET_PROPERTY:lssim,LINK_LIBRARIES>)
    target_compile_options(carto_stack_audit PRIVATE -fno-sanitize=all)
    add_dependencies(lssim carto_stack_audit)
    add_custom_command(TARGET lssim POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_carto_stack.py" "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Checking CartoCore TUI stack and file/flash boundary")
endif()

ls_add_test(test_map_marks_async ${FW}/components/apps/tui/ls_geo.c)
target_include_directories(test_map_marks_async PRIVATE ${FW}/components/apps/tui)
ls_add_test(test_rid ${FW}/components/apps/tui/ls_rid.c)
target_include_directories(test_rid PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_sweep ${FW}/components/apps/tui/ls_sweep.c ${FW}/components/apps/tui/ls_rid.c)
target_include_directories(test_sweep PRIVATE ${LS_TUI_APP_INCLUDES})

target_sources(lssim PRIVATE tools/lssim_sweep.c)
ls_add_test(test_sweep_runtime ${FW}/main/ls_sweep_runtime.c ${FW}/components/apps/tui/ls_sweep.c ${FW}/components/apps/tui/ls_rid.c)
ls_add_test(test_sdio_rx_watchdog)
ls_add_test(test_ble_link_observer ${FW}/main/ble_link_observer.c ${FW}/components/apps/tui/ls_rid.c)
target_include_directories(test_ble_link_observer PRIVATE ${FW}/main ${LS_TUI_APP_INCLUDES})
target_include_directories(test_sweep_runtime PRIVATE ${LS_TUI_APP_INCLUDES})

target_compile_definitions(test_sweep_runtime PRIVATE LS_SWEEP_DIR="${CMAKE_CURRENT_BINARY_DIR}/sweep-runtime")

ls_add_test(test_sweep_nvs ${FW}/components/apps/tui/ls_sweep.c ${FW}/components/apps/tui/ls_rid.c)
target_include_directories(test_sweep_nvs BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sweep_platform)
target_include_directories(test_sweep_nvs PRIVATE ${LS_TUI_APP_INCLUDES})
target_compile_definitions(test_sweep_nvs PRIVATE ESP_PLATFORM=1)

ls_add_test(test_sweep_sound shims/notification_settings_fake.c)
target_compile_options(test_sweep_sound PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_sweep_sound PRIVATE -Wl,--gc-sections)

# Remote ID UI also links in router-only hosts that have no field worker.
target_compile_definitions(test_tui_router PRIVATE LS_FIELD_SAMPLE_STUB=1)

# SWEEP uses the shared vector canvas in the real TUI render tests.
foreach(sweep_render_target test_tui_render_cost test_tui_render_dump)
    target_sources(${sweep_render_target} PRIVATE ${FW}/components/apps/tui/ls_map_ink.c)
endforeach()

ls_add_test(test_sweep_find)
target_include_directories(test_sweep_find PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_options(test_sweep_find PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_sweep_find PRIVATE -Wl,--gc-sections)
set_source_files_properties(tests/test_sweep_find.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")

if(Python3_Interpreter_FOUND)
    add_test(NAME test_compass_regressions COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_compass_regressions.py")
    add_test(NAME test_ble_scan COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ble_scan.py")
    add_test(NAME test_audio_reinit COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio_reinit.py")
    add_test(NAME test_audio_drop_accounting COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_audio_drop_accounting.py")
endif()

# Optional round-trip uses the actual, read-only Flipper app parser.
set(LS_FLIPPER_APP_DIR "${FW}/../ls-flipper-29" CACHE PATH "Flipper v2.10 app checkout for AUX decoder tests")
if(EXISTS "${LS_FLIPPER_APP_DIR}/ls_link.c")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME test_flipper_aux_roundtrip
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_flipper_aux_roundtrip.py ${LS_FLIPPER_APP_DIR})
    endif()
endif()

ls_add_test(test_wifi_store ${FW}/main/ls_wifi_sta_core.c)

# Host fixtures use repo-relative paths, as does verify.ps1's historical runner.
# Declare the cwd so direct CTest and the release gate exercise the same graph.
get_property(LS_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
foreach(LS_TEST IN LISTS LS_REGISTERED_TESTS)
    get_property(LS_TEST_CWD TEST ${LS_TEST} PROPERTY WORKING_DIRECTORY)
    if(NOT LS_TEST_CWD)
        set_tests_properties(${LS_TEST} PROPERTIES WORKING_DIRECTORY "${FW}")
    endif()
endforeach()

# Identity-based live overlay placement regressions.
ls_add_test(test_map_label_sticky)

ls_add_test(test_adsb_rx
    ${APP}/adsb/uat_decode.c ${APP}/adsb/uat_stream.c ${APP}/adsb/uat_weather.c
    ${APP}/adsb/adsb_source.c
    ${FW}/components/lakeshark/radio/iq_app_control.c)
target_include_directories(test_adsb_rx PRIVATE
    ${APP}/adsb ${FW}/components/lakeshark/core)

ls_add_test(test_subghz_runtime tests/subghz_runtime/radio_fake.c
    ${APP}/rec/rec_lr_iq.c ${APP}/rec/rec_sidecar.c
    ${APP}/subghz/subghz_file.c
    ${APP}/rec/rec_ook24.c ${APP}/rec/rec_watch.c ${APP}/rec/rec_lr_ook.c ${FW}/components/lakeshark/radio/ls_fsk_capture.c)
target_include_directories(test_subghz_runtime PRIVATE tests/subghz_runtime)
target_compile_options(test_subghz_runtime PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_subghz_runtime PRIVATE -Wl,--gc-sections)
ls_add_test(test_wireless_backend ${FW}/components/apps/tui/ls_wireless_history.c)
target_include_directories(test_wireless_backend PRIVATE ${FW}/main ${FW}/components/apps ${LS_TUI_APP_INCLUDES})
target_compile_definitions(test_wireless_backend PRIVATE CONFIG_LS_C6_LINK=1 CONFIG_LS_BLE_HEAD=1)

add_test(NAME test_gps_ui COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gps_ui.py $<TARGET_FILE:lssim>)

target_sources(test_flipper_fm_command PRIVATE ${FW}/components/lakeshark/core/ls_uart_claim.c)

ls_add_test(test_set_async)
# scr_settings.c draws its words through ls_tr (ls_i18n.h). A library of its
# own keeps -fwhole-program, which this suite compiles with, off that code.
add_library(ls_i18n_host STATIC ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/tuilib/tui_core.c)
target_include_directories(ls_i18n_host PRIVATE ${FW}/components/apps/tui ${FW}/bench/shims)
target_link_libraries(test_set_async ls_i18n_host)
target_include_directories(test_set_async PRIVATE
    tests/set_shims ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark
    ${FW}/components/apps/tui ${FW}/components/lakeshark/board)
target_compile_options(test_set_async PRIVATE -O2 -fwhole-program -ffunction-sections -fdata-sections)
target_link_options(test_set_async PRIVATE -Wl,--gc-sections)
file(GLOB MESH_HOST_ED ${FW}/components/meshcore/upstream/ed25519/*.c)
# Upstream ref10 ed25519 left-shifts negative values by design (ge.c select).
if(LS_SANITIZE)
    set_source_files_properties(${MESH_HOST_ED} PROPERTIES COMPILE_OPTIONS "-fno-sanitize=shift")
endif()
add_executable(test_mesh_backend tests/test_mesh_backend.cpp tests/mesh_backend_fakes.cpp
    ${FW}/components/meshcore/upstream/src/Dispatcher.cpp
    ${FW}/components/meshcore/upstream/src/Mesh.cpp
    ${FW}/components/meshcore/upstream/src/Packet.cpp
    ${FW}/components/meshcore/upstream/src/Identity.cpp
    ${FW}/components/meshcore/upstream/src/helpers/AdvertDataHelpers.cpp
    ${FW}/components/meshcore/ls_mesh_inspect.c
    ${MESH_HOST_ED})
target_link_libraries(test_mesh_backend ls_harness m)
target_include_directories(test_mesh_backend BEFORE PRIVATE
    ${FW}/bench/shims/mesh ${FW}/bench/shims
    ${FW}/components/meshcore/compat ${FW}/components/meshcore/upstream/src
    ${FW}/components/meshcore/upstream/ed25519 ${FW}/components/meshcore
    ${FW}/components/lakeshark ${FW}/components/lakeshark/core
    ${FW}/components/lakeshark/board ${FW}/components/lakeshark/radio)
target_compile_definitions(test_mesh_backend PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)
target_compile_options(test_mesh_backend PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_mesh_backend PRIVATE -Wl,--gc-sections)
add_test(NAME test_mesh_backend COMMAND test_mesh_backend)

ls_add_test(test_map_place_search
    ${FW}/components/apps/tui/ls_motion.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/bench/shims/tui_hw_stubs.c)
target_include_directories(test_map_place_search PRIVATE ${LS_TUI_APP_INCLUDES} ${CARTO}/include)
target_compile_definitions(test_map_place_search PRIVATE LS_CARTOCORE_HOST=1)
target_compile_options(test_map_place_search PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_map_place_search PRIVATE -Wl,--gc-sections)
ls_add_test(test_tiles_open)
target_include_directories(test_tiles_open PRIVATE ${LS_TUI_APP_INCLUDES} ${CARTO}/include)
target_compile_definitions(test_tiles_open PRIVATE LS_CARTOCORE_HOST=1)
target_compile_options(test_tiles_open PRIVATE -ffunction-sections -fdata-sections -fwhole-program)
target_link_options(test_tiles_open PRIVATE -Wl,--gc-sections)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_tiles_queue COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_tiles_queue.py")
    add_test(NAME test_tiles_state COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_tiles_state.py")
endif()

ls_add_test(test_rec_catch_core ${APP}/rec/rec_catch_core.c ${APP}/subghz/subghz_file.c
    ${FW}/components/lakeshark/radio/ls_sub_fsk.c)
add_test(NAME test_rec_catch_ui COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rec_catch_ui.py $<TARGET_FILE:lssim>)
ls_add_test(test_rec_catch_runtime ${APP}/rec/rec_catch_core.c ${APP}/rec/rec_catch_chan.c
    ${APP}/subghz/subghz_file.c ${FW}/components/lakeshark/radio/ls_sub_fsk.c
    ${FW}/components/apps/tui/experiments/lr433_dec.c)
target_include_directories(test_rec_catch_runtime PRIVATE ${FW}/components/lakeshark)
target_compile_options(test_rec_catch_runtime PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_rec_catch_runtime PRIVATE -Wl,--gc-sections)
get_property(_catch_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _catch_targets)
    get_target_property(_sources ${_target} SOURCES)
    if("${_sources}" MATCHES "scr_rec\\.c")
        target_sources(${_target} PRIVATE
            ${FW}/components/apps/tui/ls_catch_ui.c
            ${FW}/components/lakeshark/apps/rec/rec_catch_chan.c
            ${FW}/bench/shims/rec_catch_unavailable.c)
    endif()
endforeach()
set(UAT_CORE ${APP}/adsb/uat_decode.c ${APP}/adsb/uat_stream.c ${APP}/adsb/uat_weather.c)
target_sources(lssim PRIVATE ${APP}/adsb/uat_stream.c)
ls_add_test(test_uat ${UAT_CORE} ${APP}/adsb/adsb_state.c)
add_executable(uat_replay uat_replay.c ${UAT_CORE} ${APP}/adsb/adsb_state.c)
target_link_libraries(uat_replay m)

ls_add_test(test_rec_catch_chan ${APP}/rec/rec_catch_chan.c ${APP}/rec/rec_catch_core.c)
ls_add_test(test_rec_catch_budget ${APP}/rec/rec_catch_core.c)

ls_add_test(test_rec_catch_fsk ${APP}/rec/rec_catch_core.c)
ls_add_test(test_rec_catch_spurs ${APP}/rec/rec_catch_core.c)
# The waterfall without a panel: its history draws in cells.
foreach(wf_cells test_tui_waterfall_claim test_falls_display test_tui_waterfall_scale test_tui_waterfall_stall)
    target_compile_definitions(${wf_cells} PRIVATE LS_TUI_IMAGE_STUB=1)
endforeach()
foreach(wf_cells test_tui_router test_tui_p25_screen)
    target_compile_definitions(${wf_cells} PRIVATE LS_TUI_THEME_STUB=1)
endforeach()

# Optional read-only historical adapter comparison, using the same finite producer.
set(CATCH_BASELINE_DIR "" CACHE PATH "CATCH adapter/core snapshot for cadence regression")
if(CATCH_BASELINE_DIR)
    add_executable(test_rec_catch_baseline tests/test_rec_catch_runtime.c
        ${CATCH_BASELINE_DIR}/rec_catch_core.c ${APP}/subghz/subghz_file.c
        ${FW}/components/lakeshark/radio/ls_sub_fsk.c
        ${FW}/components/apps/tui/experiments/lr433_dec.c)
    target_link_libraries(test_rec_catch_baseline ls_harness m)
    target_include_directories(test_rec_catch_baseline PRIVATE
        ${FW}/components/lakeshark ${APP}/rec)
    target_compile_definitions(test_rec_catch_baseline PRIVATE
        CATCH_ADAPTER_SOURCE="${CATCH_BASELINE_DIR}/rec_catch.c")
    target_compile_options(test_rec_catch_baseline PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(test_rec_catch_baseline PRIVATE -Wl,--gc-sections)
    add_test(NAME test_rec_catch_baseline COMMAND test_rec_catch_baseline finite_producer_board_cost)
endif()

ls_add_test(test_pocsag_iq ${APP}/fm/pocsag.c
    ${FW}/components/apps/tui/experiments/pager_iq.c)
target_include_directories(test_pocsag_iq PRIVATE
    ${FW}/components/apps/tui/experiments)

# Compile the real ESP capture adapter with a controlled host producer task.
add_executable(test_pager_iq_radio tests/test_pager_iq_radio.c
    ${FW}/components/apps/tui/experiments/pager_iq_radio.c
    ${FW}/components/apps/tui/experiments/pager_iq.c
    ${FW}/components/lakeshark/dsp/lr_iq_spectrum.c
    ${FW}/components/lakeshark/dsp/spectrum.c)
target_include_directories(test_pager_iq_radio PRIVATE
    ${FW}/components/apps/tui/experiments ${FW}/components/lakeshark/dsp)
target_compile_definitions(test_pager_iq_radio PRIVATE ESP_PLATFORM=1)
target_link_libraries(test_pager_iq_radio PRIVATE Threads::Threads m)
add_test(NAME test_pager_iq_radio COMMAND test_pager_iq_radio)
set_tests_properties(test_pager_iq_radio PROPERTIES TIMEOUT 30)

ls_add_test(test_fm_lr ${APP}/fm/fm_lr_dsp.c ${APP}/fm/fm_dsp.c)

add_library(fm_lr_backend_compile OBJECT ${APP}/fm/app_fm.c)
target_include_directories(fm_lr_backend_compile PRIVATE
    ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark
    ${APP}/acars ${FW}/components/acars ${FW}/components/meshcore)

ls_add_test(test_fm_lr_runtime ${APP}/fm/fm_lr_dsp.c ${APP}/fm/fm_dsp.c
    ${APP}/fm/fm_tone.c)
target_include_directories(test_fm_lr_runtime PRIVATE
    ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark
    ${APP}/acars ${FW}/components/acars ${FW}/components/meshcore)
target_compile_options(test_fm_lr_runtime PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_fm_lr_runtime PRIVATE -Wl,--gc-sections)

set_source_files_properties(tests/test_fm_lr_runtime.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")
# Existing pager adapter now taps checked IQ; link that dependency on host too.
target_sources(test_pager_iq_radio PRIVATE
    ${FW}/components/lakeshark/dsp/lr_iq_spectrum.c
    ${FW}/components/lakeshark/dsp/spectrum.c)

ls_add_test(test_gauge_refresh ${FW}/components/lakeshark/board/ls_gauge.c)
target_compile_definitions(test_gauge_refresh PRIVATE CONFIG_LS_BOARD_T_DISPLAY_P4=1)

foreach(posture portrait landscape)
    set(status_rotation)
    if(posture STREQUAL "landscape")
        set(status_rotation -l)
    endif()
    add_test(NAME test_switch_status_${posture}
        COMMAND ${CMAKE_COMMAND} -E env LSSIM_STATUS_AUDIT=1
        $<TARGET_FILE:lssim> fm ${status_rotation} -f 15
        -x "+o,@esc,@esc,@tab,~15,@tab,~15"
        -o "${CMAKE_CURRENT_BINARY_DIR}/switch-status-${posture}.bmp")
endforeach()

# Link-time allocation interception includes libc and heap_caps shim allocations.
foreach(switch_target test_tui_screens test_tui_p25_screen test_wf_source test_pixel_wf test_lr_iq_spectrum)
    target_sources(${switch_target} PRIVATE tests/switch_budget.c)
    target_link_options(${switch_target} PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
endforeach()

target_link_options(test_radio_endpoint PRIVATE -Wl,--wrap=xSemaphoreTake)

foreach(boundary nvs_get_u8 nvs_get_i8 nvs_get_u32 nvs_get_i32 nvs_get_u64)
    target_link_options(test_tui_p25_screen PRIVATE "-Wl,--wrap=${boundary}")
endforeach()

foreach(boundary ls_lora_hw_lock spi_device_transmit spi_device_polling_transmit
        nvs_set_u8 nvs_set_i8 nvs_set_u32 nvs_set_i32 nvs_set_u64 nvs_commit
        nvs_get_u8 nvs_get_i8 nvs_get_u32 nvs_get_i32 nvs_get_u64)
    target_link_options(test_tui_screens PRIVATE "-Wl,--wrap=${boundary}")
endforeach()

# Phase 1 DETECTOR: real parser/policy, adapters and sinks, no radio hardware.
ls_add_test(test_detector
    ${FW}/components/apps/tui/tuilib/tui_core.c ${FW}/components/apps/tui/ls_i18n.c
    ${FW}/components/apps/tui/ls_tui_ui.c
    ${FW}/components/apps/tui/ls_icons.c
    ${FW}/components/apps/tui/ls_detector.c
    ${FW}/components/apps/tui/ls_detector_sinks.c
    ${FW}/components/apps/tui/ls_detector_air.c
    ${APP}/adsb/adsb_state.c
    ${FW}/components/apps/tui/ls_detector_ui.c
    ${FW}/components/apps/tui/ls_notify.c
    ${FW}/components/apps/tui/ls_sweep.c
    ${FW}/components/apps/tui/ls_rid.c)
target_include_directories(test_detector PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_definitions(test_detector PRIVATE
    LS_DETECTOR_DIR="${CMAKE_CURRENT_BINARY_DIR}/detector-test"
    LS_DETECTOR_ROTATE=4096
    LS_NOTIFY_EXTERNAL_ALERT_HW=1)
target_compile_options(test_detector PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_detector PRIVATE -Wl,--gc-sections)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_detector_json COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_detector_json.py"
        "${CMAKE_CURRENT_BINARY_DIR}/detector-test/events")
    set_tests_properties(test_detector_json PROPERTIES DEPENDS test_detector)
    add_test(NAME test_detector_ui_sim COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_detector_ui_sim.py" "$<TARGET_FILE:lssim>")
endif()

ls_add_test(test_detector_runtime
    ${FW}/components/apps/tui/ls_detector.c
    ${FW}/components/apps/tui/ls_detector_air.c
    ${APP}/adsb/adsb_state.c
    ${FW}/components/apps/tui/ls_sweep.c
    ${FW}/components/apps/tui/ls_rid.c
    ${FW}/main/ls_sweep_runtime.c)
target_include_directories(test_detector_runtime PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_definitions(test_detector_runtime PRIVATE
    LS_DETECTOR_DIR="${CMAKE_CURRENT_BINARY_DIR}/detector-runtime"
    LS_SWEEP_DIR="${CMAKE_CURRENT_BINARY_DIR}/detector-sweep-runtime")

# DETECTOR aircraft rules: the real decoder fills the shared table, no receiver.
ls_add_test(test_detector_air
    ${FW}/components/apps/tui/ls_detector.c
    ${FW}/components/apps/tui/ls_detector_air.c
    ${FW}/components/apps/tui/ls_sweep.c
    ${FW}/components/apps/tui/ls_rid.c
    ${APP}/adsb/mode-s.c
    ${APP}/adsb/adsb_decode.c
    ${APP}/adsb/adsb_state.c)
target_include_directories(test_detector_air PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${APP}/adsb ${FW}/components/lakeshark/audio)
target_compile_definitions(test_detector_air PRIVATE
    LS_DETECTOR_DIR="${CMAKE_CURRENT_BINARY_DIR}/detector-air"
    LS_SWEEP_DIR="${CMAKE_CURRENT_BINARY_DIR}/detector-air-sweep")

ls_add_test(test_ble_range ${FW}/main/ble_link_legacy.c)
target_include_directories(test_ble_range PRIVATE ${FW}/main)

ls_add_test(test_dronelink ${FW}/components/apps/tui/ls_dronelink.c)
target_include_directories(test_dronelink PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark ${FW}/components/meshcore)
set_tests_properties(test_detector test_ble_range test_dronelink PROPERTIES WORKING_DIRECTORY "${FW}")
ls_add_test(test_map_share
    ${FW}/components/apps/tui/ls_map_share.c
    ${FW}/components/apps/tui/ls_map_marks.c
    ${FW}/components/apps/tui/ls_geo.c)
target_include_directories(test_map_share PRIVATE ${FW}/components/apps/tui)

ls_add_test(test_map_share_ui ${FW}/components/apps/tui/ls_map_share.c
    ${FW}/components/apps/tui/ls_map_marks.c ${FW}/components/apps/tui/ls_geo.c)
target_include_directories(test_map_share_ui PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${CARTO}/include)
target_compile_options(test_map_share_ui PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_map_share_ui PRIVATE -Wl,--gc-sections)
set_source_files_properties(tests/test_map_share_ui.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")

ls_add_test(test_map_share_mesh)
target_include_directories(test_map_share_mesh PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${CARTO}/include)
target_compile_options(test_map_share_mesh PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_map_share_mesh PRIVATE -Wl,--gc-sections)
set_source_files_properties(tests/test_map_share_mesh.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")

# Production MAP touch journeys and portrait/landscape review stills.
add_test(NAME test_map_touch COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_map_touch.py" "$<TARGET_FILE:lssim>")

# Opt-in example: normal simulator builds keep the production directory.
option(LS_EXAMPLE_APP "Enable the native example in lssim" OFF)
if(LS_EXAMPLE_APP)
    target_sources(lssim PRIVATE ${FW}/components/apps/tui/apps/example_app.c)
    target_compile_definitions(lssim PRIVATE CONFIG_LS_EXAMPLE_APP=1)
    add_test(NAME test_example_app COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_example_app.py
        $<TARGET_FILE:lssim> ${CMAKE_CURRENT_BINARY_DIR}/example-renders
        ${CMAKE_C_COMPILER} ${CMAKE_CXX_COMPILER} ${CMAKE_NM})
endif()

# ACARS through each receive chain: AM envelope from RTL and LR2021 I/Q.
ls_add_test(test_acars_am_chain
    ${FW}/bench/fixtures/acars_gen.c
    ${APP}/acars/acars.c ${APP}/acars/acars_msk.c ${APP}/acars/acars_resample.c
    ${APP}/fm/fm_dsp.c ${APP}/fm/fm_lr_dsp.c)
target_include_directories(test_acars_am_chain PRIVATE
    ${FW}/bench/fixtures ${APP}/acars ${APP}/fm ${FW}/components/lakeshark)
ls_add_test(test_acars ${FW}/bench/fixtures/acars_gen.c
    ${APP}/acars/acars.c ${APP}/acars/acars_msk.c)
target_include_directories(test_acars PRIVATE ${FW}/bench/fixtures ${APP}/acars)

# FM glass displays the real DSP squelch threshold, including on the host.
target_sources(lssim PRIVATE ${APP}/fm/fm_dsp.c)

add_test(NAME test_fm_vfo_ui COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_fm_vfo_ui.py $<TARGET_FILE:lssim>)

# Count shared scan dispatch without replacing the scanner fixture's behavior.
foreach(tune_action next skip hold stop)
    target_link_options(test_tui_p25_screen PRIVATE "-Wl,--wrap=scan_engine_${tune_action}")
endforeach()
add_test(NAME test_p25_vfo_ui COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_p25_vfo_ui.py $<TARGET_FILE:lssim>)

# EXT_RAM_BSS_ATTR is zeroed at boot on the board; an initializer there is silently lost.
add_test(NAME test_ext_ram_bss_no_init COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ext_ram_bss_no_init.py ${FW})

# Drone Watch: real evidence engine and passive RID with fake RF/session feeds.
ls_add_test(test_drone_watch
    ${FW}/components/apps/tui/ls_dronelink.c
    ${FW}/components/apps/tui/ls_rid.c
    ${FW}/components/lakeshark/core/settings_schema.c)
target_include_directories(test_drone_watch PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${FW}/components/meshcore)
target_compile_definitions(test_drone_watch PRIVATE
    DRONE_DEFAULTS="${FW}/data/drone-watch/defaults.json")

target_sources(test_drone_watch PRIVATE ${FW}/components/apps/tui/ls_df.c)
target_compile_options(test_drone_watch PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_drone_watch PRIVATE -Wl,--gc-sections)

ls_add_test(test_drone_watch_find)
target_include_directories(test_drone_watch_find PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_options(test_drone_watch_find PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_drone_watch_find PRIVATE -Wl,--gc-sections)
ls_add_test(test_drone_watch_detector)
target_include_directories(test_drone_watch_detector PRIVATE ${LS_TUI_APP_INCLUDES} ${FW}/components/lakeshark)
target_compile_definitions(test_drone_watch_detector PRIVATE LS_DETECTOR_DIR="${CMAKE_CURRENT_BINARY_DIR}/drone-detector")
target_compile_options(test_drone_watch_detector PRIVATE -ffunction-sections -fdata-sections)
target_link_options(test_drone_watch_detector PRIVATE -Wl,--gc-sections)
set_source_files_properties(tests/test_drone_watch_find.c tests/test_drone_watch_detector.c
    PROPERTIES COMPILE_OPTIONS "-fwhole-program")
add_library(drone_watch_firmware_compile OBJECT ${FW}/components/apps/tui/ls_drone_watch.c)
target_include_directories(drone_watch_firmware_compile PRIVATE ${LS_TUI_APP_INCLUDES}
    ${FW}/components/lakeshark ${FW}/components/meshcore)
target_compile_definitions(drone_watch_firmware_compile PRIVATE ESP_PLATFORM=1)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_drone_watch_settings COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_drone_watch_settings.py")
endif()
# Backend-only notification synthesis, persistence and worker dispatch.
ls_add_test(test_notification_sound
    ${FW}/components/lakeshark/audio/notification_tone.c
    ${FW}/components/lakeshark/core/settings_schema.c
    ${FW}/components/lakeshark/core/home_widget_pref.c

    ${FW}/components/lakeshark/core/p25_demod_pref_cache.c)
target_include_directories(test_notification_sound PRIVATE
    tests/set_shims ${FW}/components/lakeshark/core ${FW}/components/lakeshark/board)
target_compile_definitions(test_notification_sound PRIVATE
    NOTIFICATION_DEFAULTS="${FW}/data/audio/notification.json")
set_source_files_properties(tests/test_notification_sound.c PROPERTIES COMPILE_OPTIONS "-fwhole-program")
target_compile_options(test_notification_sound PRIVATE -O2 -ffunction-sections -fdata-sections)
target_link_options(test_notification_sound PRIVATE -Wl,--gc-sections)

# P25 state packs: firmware rules shared by production and tests.
ls_add_test(test_p25_pack ${APP}/p25/p25_pack.c ${APP}/p25/p25_profile.c
    ${APP}/p25/p25_geo.c ${APP}/p25/p25_cqpsk_controls.c
    ${FW}/components/lakeshark/core/ls_ota_core.c)
add_executable(p25pack_validate p25pack/validate.c ${APP}/p25/p25_pack.c
    ${APP}/p25/p25_profile.c ${APP}/p25/p25_cqpsk_controls.c
    ${FW}/components/lakeshark/core/ls_ota_core.c)
target_link_libraries(p25pack_validate ls_harness m)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(P25PACK_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}" CACHE FILEPATH
        "Python interpreter with cryptography for the P25 publisher tests")
    add_test(NAME test_p25pack_tools COMMAND ${P25PACK_PYTHON_EXECUTABLE} -B
        ${FW}/bench/tests/test_p25pack_tools.py $<TARGET_FILE:p25pack_validate>
        ${FW}/bench/build-p25gps-evidence)
endif()

ls_add_test(test_p25_sites_runtime ${APP}/p25/p25_pack.c ${APP}/p25/p25_profile.c
    ${APP}/p25/p25_geo.c ${APP}/p25/p25_cqpsk_controls.c
    ${FW}/components/lakeshark/core/ls_ota_core.c)
target_include_directories(test_p25_sites_runtime BEFORE PRIVATE ${FW}/bench/p25pack/shims)
ls_add_test(test_p25_packs_runtime ${APP}/p25/p25_pack.c ${APP}/p25/p25_profile.c
    ${APP}/p25/p25_cqpsk_controls.c ${FW}/components/lakeshark/core/ls_ota_core.c
    ${FW}/components/apps/tui/ls_value.c ${FW}/components/apps/tui/ls_action.c)
target_include_directories(test_p25_packs_runtime BEFORE PRIVATE ${FW}/bench/p25pack/shims
    ${FW}/components/apps/tui ${FW}/components/apps)
add_library(p25_program_adapter_check OBJECT ${APP}/p25/p25_program_sd.c)
target_include_directories(p25_program_adapter_check BEFORE PRIVATE ${FW}/bench/p25pack/shims
    ${FW}/components/lakeshark/core)

target_compile_definitions(test_p25_packs_runtime PRIVATE
    TEST_PACK_EVIDENCE_DIR="${FW}/bench/build-p25gps-evidence")

# The file hotspot's per-board password (main/ls_wifi_ap_pass.h).
ls_add_test(test_wifi_ap_pass)
target_include_directories(test_wifi_ap_pass PRIVATE ${FW}/main)

if(TARGET lssim AND TARGET test_cartocore_cells)
  add_test(NAME test_maps_states COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_maps_states.py" "$<TARGET_FILE:lssim>"
    "${CMAKE_CURRENT_BINARY_DIR}/cells-fixture")
  set_tests_properties(test_maps_states PROPERTIES FIXTURES_REQUIRED carto_cells_inputs)
endif()

if(TARGET lssim)
  add_test(NAME test_maps_outlines COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_maps_outlines.py" "$<TARGET_FILE:lssim>"
    "${CMAKE_CURRENT_BINARY_DIR}/cells-fixture")
endif()

# The US map builder lives in the CartoCore repository: set CARTOCORE_DIR to run its test.
if(DEFINED ENV{CARTOCORE_DIR} AND EXISTS "$ENV{CARTOCORE_DIR}/tools/build_us.py")
  add_test(NAME test_maps_outlines_builder COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_maps_outlines_builder.py"
    "$ENV{CARTOCORE_DIR}/tools/build_us.py" "${CMAKE_CURRENT_BINARY_DIR}")
endif()

# Native SD apps: only these targets are needed for the backend bench.
set(LAP_TUI ${FW}/components/apps/tui)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lap-probe.lap
    COMMAND ${Python3_EXECUTABLE} ${FW}/app-sdk/lap.py build ${FW}/bench/fixtures/lap -o ${CMAKE_CURRENT_BINARY_DIR}/lap-probe.lap
    DEPENDS ${FW}/app-sdk/lap.py ${FW}/app-sdk/exports.txt ${FW}/app-sdk/include/lakeshark_app.h ${FW}/bench/fixtures/lap/probe.c)
add_custom_target(lap_fixture DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/lap-probe.lap)
add_executable(test_lap_manifest tests/test_lap_manifest.c ${LAP_TUI}/ls_lap_manifest.c ${LAP_TUI}/ls_lap_signature.c ${LAP_TUI}/ls_lap.c)
target_include_directories(test_lap_manifest PRIVATE ${LAP_TUI})
add_dependencies(test_lap_manifest lap_fixture)
add_test(NAME test_lap_manifest COMMAND test_lap_manifest ${CMAKE_CURRENT_BINARY_DIR}/lap-probe.lap)
add_executable(lap_inspect tests/lap_inspect.c ${LAP_TUI}/ls_lap_manifest.c)
target_include_directories(lap_inspect PRIVATE ${LAP_TUI} ${FW}/components/lakeshark)
add_executable(test_lap_transfer tests/test_lap_transfer.c ${LAP_TUI}/ls_lap_transfer.c ${LAP_TUI}/ls_lap_manifest.c ${FW}/components/lakeshark/core/ls_base64.c)
target_include_directories(test_lap_transfer PRIVATE ${LAP_TUI} ${FW}/components/lakeshark)
add_test(NAME test_lap_transfer COMMAND test_lap_transfer)
add_executable(test_lap_api tests/test_lap_api.c ${LAP_TUI}/ls_lap_api.c ${LAP_TUI}/ls_lap_sys.c ${LAP_TUI}/ls_lap_net.c ${LAP_TUI}/ls_lap_manifest.c ${LAP_TUI}/ls_lap_storage.c ${LAP_TUI}/ls_glyph.c shims/lap_tui_core.c shims/screen_full_stub.c)
target_include_directories(test_lap_api PRIVATE ${LAP_TUI} ${FW}/components/lakeshark)
target_link_libraries(test_lap_api m)
target_compile_definitions(test_lap_api PRIVATE LS_LAP_SD_ROOT="lap-sd")
add_test(NAME test_lap_api COMMAND test_lap_api)

# Optional SDK checkout location, used only by the new LAP signature test.
set(LAP_MBEDTLS_DIR "" CACHE PATH "Mbed TLS source root for the LAP host verifier")
if(LAP_MBEDTLS_DIR)
    file(GLOB LAP_MBED_SOURCES ${LAP_MBEDTLS_DIR}/library/*.c)
    add_library(lap_mbedtls STATIC ${LAP_MBED_SOURCES})
    target_include_directories(lap_mbedtls PUBLIC ${LAP_MBEDTLS_DIR}/include ${FW}/bench/fixtures)
    target_compile_definitions(lap_mbedtls PUBLIC MBEDTLS_CONFIG_FILE="lap_mbedtls_config.h")
    add_executable(lap_verify tests/lap_verify.c ${LAP_TUI}/ls_lap_manifest.c ${LAP_TUI}/ls_lap_signature.c ${FW}/components/lakeshark/core/ls_signature.c)
    target_include_directories(lap_verify PRIVATE ${LAP_TUI} ${FW}/components/lakeshark)
    target_link_libraries(lap_verify lap_mbedtls)
    add_test(NAME test_lap_sdk COMMAND ${Python3_EXECUTABLE} ${FW}/bench/tests/test_lap_sdk.py $<TARGET_FILE:lap_inspect> ${CMAKE_CURRENT_BINARY_DIR} $<TARGET_FILE:lap_verify>)
else()
    add_test(NAME test_lap_sdk COMMAND ${Python3_EXECUTABLE} ${FW}/bench/tests/test_lap_sdk.py $<TARGET_FILE:lap_inspect> ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(LS_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang")
    # Stock MinGW clang accepts UBSan at link time without shipping its runtime.
    # These new targets trap on UB, so instrumentation never silently goes unused.
    foreach(lap_test test_lap_manifest test_lap_api test_lap_transfer lap_inspect)
        target_compile_options(${lap_test} PRIVATE -fsanitize-trap=undefined)
    endforeach()
endif()

if(LAP_MBEDTLS_DIR)
    add_executable(test_lap_host tests/test_lap_host.c ${LAP_TUI}/ls_lap_api.c ${LAP_TUI}/ls_lap_sys.c ${LAP_TUI}/ls_lap_net.c ${LAP_TUI}/ls_lap_manifest.c ${LAP_TUI}/ls_lap_signature.c ${LAP_TUI}/ls_lap_storage.c ${LAP_TUI}/ls_app.c ${LAP_TUI}/ls_glyph.c shims/lap_tui_core.c ${FW}/components/lakeshark/core/ls_signature.c)
    target_include_directories(test_lap_host BEFORE PRIVATE ${FW}/bench/shims/lap ${LAP_TUI} ${FW}/components/lakeshark)
    target_compile_definitions(test_lap_host PRIVATE ESP_PLATFORM=1 LS_LAP_HOST_TEST=1)
    target_compile_options(test_lap_host PRIVATE -falign-functions=2)
    target_link_libraries(test_lap_host lap_mbedtls m)
    add_dependencies(test_lap_host lap_fixture)
    add_test(NAME test_lap_host COMMAND test_lap_host ${CMAKE_CURRENT_BINARY_DIR}/lap-probe.lap)
endif()

target_sources(lssim PRIVATE ${LAP_TUI}/ls_lap.c shims/ls_lap_screen.c)
target_sources(lssim PRIVATE ${APP}/adsb/adsb_demo.c fixtures/carrier_preview.c)
# The API resolver references ADS-B for every app, including non-radio test apps.
target_sources(test_lap_api PRIVATE ${APP}/adsb/adsb_state.c ${APP}/adsb/adsb_demo.c)
target_sources(test_lap_api PRIVATE shims/lap_adsb_time.c)
if(TARGET test_lap_host)
    target_sources(test_lap_host PRIVATE ${APP}/adsb/adsb_state.c ${APP}/adsb/adsb_demo.c)
endif()
# The example apps, compiled for the host into lssim's APP screen so their
# frames can be looked at (LAPSIM=<id>; see shims/ls_lap_screen.c). Each
# app's lap_main and lap_app are renamed so they can sit side by side.
file(GLOB LAPSIM_APP_DIRS LIST_DIRECTORIES true ${FW}/app-sdk/apps/*)
set(LAPSIM_TABLE "")
set(LAPSIM_DECLS "")
set(LAPSIM_COUNT 0)
foreach(dir ${LAPSIM_APP_DIRS})
    if(IS_DIRECTORY ${dir})
        get_filename_component(app ${dir} NAME)
        file(GLOB app_src ${dir}/*.c)
        target_sources(lssim PRIVATE ${app_src})
        set_source_files_properties(${app_src} PROPERTIES COMPILE_DEFINITIONS
            "lap_main=lap_main_${app};lap_app=lap_app_${app};lap_icon_px=lap_icon_px_${app};LAP_ICON_PX_ATTR=__attribute__((used))")
        string(APPEND LAPSIM_DECLS "const lap_app_t *lap_main_${app}(void);
extern const lap_icon_px_t lap_icon_px_${app};
")
        string(APPEND LAPSIM_TABLE "    {\"${app}\", lap_main_${app}, &lap_icon_px_${app}},
")
        math(EXPR LAPSIM_COUNT "${LAPSIM_COUNT} + 1")
    endif()
endforeach()
# Host evidence only: exercise lap3d through real lap_dots and the cell renderer.
target_sources(lssim PRIVATE fixtures/lap3d_preview.c)
string(APPEND LAPSIM_DECLS "const lap_app_t *lap_main_lap3d(void);\nextern const lap_icon_px_t lap_icon_px_lap3d;\n")
string(APPEND LAPSIM_TABLE "    {\"lap3d\", lap_main_lap3d, &lap_icon_px_lap3d},\n")
math(EXPR LAPSIM_COUNT "${LAPSIM_COUNT} + 1")
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/lapsim_apps.c CONTENT
"#include \"lakeshark_app.h\"
${LAPSIM_DECLS}typedef struct { const char *id; const lap_app_t *(*main)(void); const lap_icon_px_t *icon; } lapsim_app_t;
const lapsim_app_t lapsim_apps[] = {
${LAPSIM_TABLE}    {0, 0, 0}};
const int lapsim_app_count = ${LAPSIM_COUNT};
")
target_sources(lssim PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lapsim_apps.c ${LAP_TUI}/ls_lap_api.c ${LAP_TUI}/ls_lap_sys.c ${LAP_TUI}/ls_lap_net.c ${LAP_TUI}/ls_lap_storage.c ${LAP_TUI}/ls_lap_manifest.c)
target_include_directories(lssim PRIVATE ${FW}/app-sdk/include)
add_executable(test_lap_registry tests/test_lap_registry.c ${LAP_TUI}/ls_app.c)
target_include_directories(test_lap_registry PRIVATE ${LAP_TUI})
add_test(NAME test_lap_registry COMMAND test_lap_registry)

add_executable(test_lap_crumb tests/test_lap_crumb.c)
target_include_directories(test_lap_crumb BEFORE PRIVATE ${FW}/bench/shims/lap ${FW}/components/lakeshark/core)
add_test(NAME test_lap_crumb COMMAND test_lap_crumb)

# Firmware dot engine, shared verbatim by lssim, its targeted test and benchmark.
file(GLOB LAP3D_SRCS CONFIGURE_DEPENDS ${LAP_TUI}/lap3d/*.c)
add_library(lap3d_host STATIC ${LAP3D_SRCS})
target_compile_options(lap3d_host PRIVATE -O2 -fstack-usage -Wdouble-promotion -Werror)
target_link_libraries(lssim lap3d_host)
target_link_libraries(test_lap_api lap3d_host)

# CARRIER: bounded systems tests share the real app API/cache and receiver state.
file(GLOB CARRIER_SYSTEMS ${FW}/app-sdk/apps/carrier/*.c)
list(REMOVE_ITEM CARRIER_SYSTEMS ${FW}/app-sdk/apps/carrier/main.c)
list(FILTER CARRIER_SYSTEMS EXCLUDE REGEX "/ui_[a-z]+\.c$")
set(CARRIER_API_TEST_SOURCES
    ${LAP_TUI}/ls_lap_api.c ${LAP_TUI}/ls_lap_sys.c ${LAP_TUI}/ls_lap_net.c
    ${LAP_TUI}/ls_lap_manifest.c ${LAP_TUI}/ls_lap_storage.c ${LAP_TUI}/ls_glyph.c
    shims/lap_tui_core.c shims/lap_adsb_time.c fixtures/carrier_io.c
    ${APP}/adsb/adsb_state.c ${APP}/adsb/adsb_demo.c)
add_executable(test_carrier tests/test_carrier.c ${CARRIER_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_compile_options(test_carrier PRIVATE -fstack-usage)
target_link_libraries(test_carrier lap3d_host m)
foreach(system universe traffic trade save flight combat jump render walk)
    add_test(NAME test_carrier_${system} COMMAND test_carrier ${system})
endforeach()
add_executable(test_lap_aircraft tests/test_lap_aircraft.c ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_lap_aircraft PRIVATE ${LAP_TUI} ${FW}/components/lakeshark)
target_link_libraries(test_lap_aircraft lap3d_host m)
add_test(NAME test_lap_aircraft COMMAND test_lap_aircraft)
add_test(NAME test_carrier_data COMMAND ${Python3_EXECUTABLE} ${FW}/bench/tests/test_carrier_data.py)
add_executable(test_carrier_limits tests/test_carrier_limits.c ${CARRIER_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_limits PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_link_libraries(test_carrier_limits lap3d_host m)
target_link_options(test_carrier_limits PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
add_test(NAME test_carrier_limits COMMAND test_carrier_limits)
add_test(NAME test_carrier_lssim COMMAND ${Python3_EXECUTABLE}
    ${FW}/bench/tests/test_carrier_lssim.py $<TARGET_FILE:lssim> ${CMAKE_CURRENT_BINARY_DIR}/carrier-preview)
add_executable(test_carrier_rescue tests/test_carrier_rescue.c ${CARRIER_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_rescue PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_link_libraries(test_carrier_rescue lap3d_host m)
add_test(NAME test_carrier_rescue COMMAND test_carrier_rescue)

# Exploration uses the real cached app files and receiver snapshots; no UI.
set(CARRIER_POI_SYSTEMS ${CARRIER_SYSTEMS})
list(FILTER CARRIER_POI_SYSTEMS EXCLUDE REGEX "/render[.]c$")
add_executable(test_carrier_poi tests/test_carrier_poi.c ${CARRIER_POI_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_poi PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_compile_definitions(test_carrier_poi PRIVATE EXPLORE_DEFAULTS="${FW}/data/carrier/explore.json")
target_link_libraries(test_carrier_poi lap3d_host m)
target_link_options(test_carrier_poi PRIVATE -Wl,--wrap=lap_time
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
foreach(poi generation nebula gravity comet relics signals salvage weather scan persistence eggs config limits)
    add_test(NAME test_carrier_poi_${poi} COMMAND test_carrier_poi ${poi})
endforeach()
if(TARGET test_lap_host)
    target_link_libraries(test_lap_host lap3d_host)
endif()
ls_add_test(test_lap3d shims/lap3d_io.c)
target_include_directories(test_lap3d PRIVATE ${FW}/app-sdk/include ${LAP_TUI}/lap3d)
target_link_libraries(test_lap3d lap3d_host)
target_link_options(test_lap3d PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
add_executable(l3bench tools/l3bench.c shims/lap3d_io.c)
target_include_directories(l3bench PRIVATE ${FW}/app-sdk/include)
target_link_libraries(l3bench lap3d_host m)

add_test(NAME test_lap3d_exports COMMAND ${Python3_EXECUTABLE}
    ${FW}/bench/tests/test_lap3d_exports.py)

# CartoCore L3M1 tiles as a 3D map, rendered by lap3d to PNG (host preview).
add_executable(l3map tools/l3map.c shims/lap3d_io.c)
target_include_directories(l3map PRIVATE ${FW}/app-sdk/include)
target_link_libraries(l3map lap3d_host m)


# CARRIER's hulls one by one, rendered by lap3d to PNG (host preview).
add_executable(carrier_ships tools/carrier_ships.c shims/lap3d_io.c)
target_include_directories(carrier_ships PRIVATE ${FW}/app-sdk/include)
target_link_libraries(carrier_ships lap3d_host m)

# Part 2: isolated deterministic sensor snapshots; existing assertions untouched.
set(CARRIER_BACKEND_SYSTEMS ${CARRIER_SYSTEMS})
list(FILTER CARRIER_BACKEND_SYSTEMS EXCLUDE REGEX "/(render|ui[^/]*)[.]c$")
add_executable(test_carrier_systems tests/test_carrier_systems.c ${CARRIER_BACKEND_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_systems PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_link_libraries(test_carrier_systems lap3d_host m)
target_link_options(test_carrier_systems PRIVATE -Wl,--wrap=lap_wifi_aps -Wl,--wrap=lap_wifi_scan
    -Wl,--wrap=lap_ble_scan -Wl,--wrap=lap_ble_devices -Wl,--wrap=lap_mesh_nodes -Wl,--wrap=lap_time
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
foreach(system talk missions mining beacons packs ranks events time system_config save_capacity targets)
    add_test(NAME test_carrier_${system} COMMAND test_carrier_systems ${system})
endforeach()

# Encounter director: isolated radio/heap boundaries, production backend logic.
add_executable(test_carrier_encounters tests/test_carrier_encounters.c ${CARRIER_BACKEND_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_encounters PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_link_libraries(test_carrier_encounters lap3d_host m)
target_link_options(test_carrier_encounters PRIVATE -Wl,--wrap=lap_aircraft
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
foreach(encounter cadence modes pool formations engagement departures life config)
    add_test(NAME test_carrier_encounters_${encounter} COMMAND test_carrier_encounters ${encounter})
endforeach()

add_test(NAME test_carrier_encounters_data COMMAND ${Python3_EXECUTABLE}
    ${FW}/bench/tests/test_carrier_encounter_data.py)

# main.c owns the screen state on device; focused host tests omit that entry.
add_executable(test_carrier_shipyard tests/test_carrier_shipyard.c ${CARRIER_BACKEND_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_shipyard PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_link_libraries(test_carrier_shipyard lap3d_host m)
target_link_options(test_carrier_shipyard PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
foreach(yard offers transactions persistence insurance config)
    add_test(NAME test_carrier_shipyard_${yard} COMMAND test_carrier_shipyard ${yard})
endforeach()
add_executable(test_carrier_lore tests/test_carrier_lore.c ${CARRIER_BACKEND_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_lore PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_link_libraries(test_carrier_lore lap3d_host m)
add_test(NAME test_carrier_lore COMMAND test_carrier_lore)

# CRUISE is a backend rules module; keep renderer and heap boundaries explicit.
add_executable(test_carrier_cruise tests/test_carrier_cruise.c ${CARRIER_BACKEND_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_cruise PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_link_libraries(test_carrier_cruise lap3d_host m)
target_link_options(test_carrier_cruise PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
foreach(cruise refusals spool safety bleed eta interdiction director limits)
    add_test(NAME test_carrier_cruise_${cruise} COMMAND test_carrier_cruise ${cruise})
endforeach()

foreach(carrier_test test_carrier test_carrier_limits test_carrier_rescue)
    target_sources(${carrier_test} PRIVATE fixtures/carrier_screen_state.c)
endforeach()

# Public-API pilot: production backend and the same host/cache as carrier tests.
add_executable(carrier_bot tools/carrier_bot.c ${CARRIER_BACKEND_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(carrier_bot PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_compile_options(carrier_bot PRIVATE -O2 -Wall -Wextra)
target_link_libraries(carrier_bot lap3d_host m)
target_link_options(carrier_bot PRIVATE -Wl,--wrap=lap_gps -Wl,--wrap=lap_time)
add_test(NAME test_carrier_bot_smoke COMMAND carrier_bot --smoke 1
    ${CMAKE_CURRENT_BINARY_DIR}/carrier-bot-smoke.csv ${FW}/data/carrier)

# Combat's public contract, fixed pools and typed simulation rules; no screen.
add_executable(test_carrier_combat_rules tests/test_carrier_combat_rules.c
    ${CARRIER_BACKEND_SYSTEMS} ${CARRIER_API_TEST_SOURCES})
target_include_directories(test_carrier_combat_rules PRIVATE ${LAP_TUI} ${FW}/components/lakeshark
    ${FW}/app-sdk/apps/carrier ${FW}/app-sdk/include)
target_compile_options(test_carrier_combat_rules PRIVATE -O2 -Wall -Wextra)
target_link_libraries(test_carrier_combat_rules lap3d_host m)
target_link_options(test_carrier_combat_rules PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
foreach(combat damage weapons mounts distributor defence modules canopy scanning hud balance persistence limits config migration reverse aim)
    add_test(NAME test_carrier_combat_rules_${combat} COMMAND test_carrier_combat_rules ${combat})
endforeach()

# The same TCP service on desktop loopback and device.
add_executable(test_lap_net tests/test_lap_net.c ${LAP_TUI}/ls_lap_net.c)
target_include_directories(test_lap_net PRIVATE ${LAP_TUI})
add_test(NAME test_lap_net COMMAND test_lap_net)
add_executable(test_lap_net_dns tests/test_lap_net.c ${LAP_TUI}/ls_lap_net.c)
target_include_directories(test_lap_net_dns PRIVATE ${LAP_TUI})
target_compile_definitions(test_lap_net_dns PRIVATE LS_LAP_NET_TEST_DNS=1)
add_test(NAME test_lap_net_dns COMMAND test_lap_net_dns)
if(WIN32)
  get_property(lap_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(t IN LISTS lap_targets)
    get_target_property(lap_sources ${t} SOURCES)
    if("${lap_sources}" MATCHES "ls_lap_net\\.c")
      target_link_libraries(${t} ws2_32)
    endif()
  endforeach()
endif()
