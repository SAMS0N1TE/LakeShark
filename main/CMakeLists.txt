set(SOURCES
    ../core/app_main.c
    ../core/settings.c
    ../core/app_registry.c
    ../core/event_bus.c
    ../core/perf.c

    ../radio/usb_host.c
    ../radio/rtlsdr_dev.c
    ../radio/esp_libusb.c
    ../radio/stream.c
    ../radio/librtlsdr.c
    ../radio/tuner_e4k.c
    ../radio/tuner_fc0012.c
    ../radio/tuner_fc0013.c
    ../radio/tuner_fc2580.c
    ../radio/tuner_r82xx.c

    ../audio/audio_out.c
    ../audio/audio_events.c
    ../audio/tone.c
    ../audio/sam_tts.c
    ../audio/plane_audio.c
    ../audio/sam/sam.c
    ../audio/sam/render.c
    ../audio/sam/reciter.c
    ../audio/sam/processframes.c
    ../audio/sam/createtransitions.c
    ../audio/sam/debug.c
)

if(CONFIG_APP_ADSB)
    list(APPEND SOURCES
        ../apps/adsb/app_adsb.c
        ../apps/adsb/adsb_decode.c
        ../apps/adsb/adsb_state.c
        ../apps/adsb/mode-s.c
        ../apps/adsb/demod1090.c
    )
    if(CONFIG_ENABLE_TUI)
        list(APPEND SOURCES ../apps/adsb/adsb_render.c)
    endif()
endif()

if(CONFIG_APP_VHF)
    list(APPEND SOURCES
        ../apps/vhf/app_vhf.c
        ../apps/vhf/vhf_demod.c
    )
endif()

if(CONFIG_APP_P25)
    list(APPEND SOURCES
        ../apps/p25/app_p25.c
        ../apps/p25/scanner.c
        ../apps/p25/cqpsk_demod.c
        ../apps/p25/dsd_main.c
        ../apps/p25/dsd_symbol.c
        ../apps/p25/dsd_dibit.c
        ../apps/p25/dsd_frame_sync.c
        ../apps/p25/dsd_frame.c
        ../apps/p25/dsd_filters.c
        ../apps/p25/dsp_pipeline.c
        ../apps/p25/p25p1_heuristics.c
        ../apps/p25/p25p1_hdu.c
        ../apps/p25/p25p1_ldu.c
        ../apps/p25/p25p1_ldu1.c
        ../apps/p25/p25p1_ldu2.c
        ../apps/p25/p25p1_tdu.c
        ../apps/p25/p25p1_tdulc.c
        ../apps/p25/p25_lcw.c
        ../apps/p25/p25p1_check_nid.c
        ../apps/p25/p25p1_check_hdu.cpp
        ../apps/p25/p25p1_check_ldu.cpp
        ../apps/p25/Hamming.cpp
        ../apps/p25/diag.c
    )
    if(CONFIG_ENABLE_TUI)
        list(APPEND SOURCES ../apps/p25/p25_tui.c)
    endif()
endif()

if(CONFIG_ENABLE_TUI)
    list(APPEND SOURCES
        ../ui/tui.c
        ../ui/tui_task.c
        ../ui/tui_log_hook.c
    )
endif()

if(CONFIG_OUTPUT_HUMAN_LOG)
    list(APPEND SOURCES ../output/log_out.c)
endif()

# diaglog: general firmware-event channel on UART3 GPIO2 → Landshark API.
# Always compiled, always emits — separate from the per-record P25
# telemetry stack (diag_emit/diag_kv/diag_json) below.
list(APPEND SOURCES ../output/diaglog.c)

# Per-record P25 telemetry stack. Always compiled.
#   diag_emit:  typed-field public API, dispatch, counter state, 1Hz periodic
#   diag_kv:    key=value text formatter on UART1 (GPIO 32)
#   diag_json:  JSONL formatter on USB-Serial-JTAG (+ RX cmd parser for set_time)
#   diag_time:  wall-clock offset, RFC3339 helper
list(APPEND SOURCES
    ../output/diag_emit.c
    ../output/diag_kv.c
    ../output/diag_json.c
    ../output/diag_time.c
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS
        ../core
        ../radio
        ../audio
        ../audio/sam
        ../apps/adsb
        ../apps/vhf
        ../apps/p25
        ../ui
        ../output
    REQUIRES
        usb
        driver
        esp_timer
        esp_driver_uart
        nvs_flash
        es8311
        esp_psram
        mbelib
)

if(CONFIG_APP_P25)
    # Inherited-from-DSD warning suppression. Applies ONLY to the
    # ported DSD/P25 sources that have known sloppy patterns from
    # upstream. New code (diag_emit/kv/json/time) is excluded — it's
    # written from scratch and should compile clean under -Wall.
    set(P25_C_SOURCES
        ../apps/p25/scanner.c
        ../apps/p25/cqpsk_demod.c
        ../apps/p25/dsd_main.c
        ../apps/p25/dsd_symbol.c
        ../apps/p25/dsd_dibit.c
        ../apps/p25/dsd_frame_sync.c
        ../apps/p25/dsd_frame.c
        ../apps/p25/dsd_filters.c
        ../apps/p25/dsp_pipeline.c
        ../apps/p25/p25p1_heuristics.c
        ../apps/p25/p25p1_hdu.c
        ../apps/p25/p25p1_ldu.c
        ../apps/p25/p25p1_ldu1.c
        ../apps/p25/p25p1_ldu2.c
        ../apps/p25/p25p1_tdu.c
        ../apps/p25/p25p1_tdulc.c
        ../apps/p25/p25_lcw.c
        ../apps/p25/p25p1_check_nid.c
        ../apps/p25/diag.c
    )
    set_source_files_properties(${P25_C_SOURCES}
        PROPERTIES COMPILE_FLAGS
        "-Wno-unused-function -Wno-unused-variable -Wno-incompatible-pointer-types -Wno-format -Wno-int-conversion -Wno-return-type -Wno-implicit-function-declaration"
    )
    set_source_files_properties(
        ../apps/p25/p25p1_check_hdu.cpp
        ../apps/p25/p25p1_check_ldu.cpp
        ../apps/p25/Hamming.cpp
        PROPERTIES COMPILE_FLAGS
        "-Wno-unused-function -Wno-unused-variable -Wno-register"
    )
endif()
