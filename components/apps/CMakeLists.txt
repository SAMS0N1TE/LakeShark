set(APPS_DIR ./)
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE APPS_C_SRCS ${APPS_DIR}/*.c)
else()
    file(GLOB_RECURSE APPS_C_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.c)
endif()
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE APPS_CPP_SRCS ${APPS_DIR}/*.cpp)
else()
    file(GLOB_RECURSE APPS_CPP_SRCS CONFIGURE_DEPENDS ${APPS_DIR}/*.cpp)
endif()

if(NOT CONFIG_LS_BOARD_T_DISPLAY_P4 AND NOT CONFIG_LS_PANEL_RUNTIME)   # TILES and CartoCore: the T-Display and a Nano with the TUI
    list(FILTER APPS_C_SRCS EXCLUDE REGEX "/ls_cartocore.c$")
    list(FILTER APPS_C_SRCS EXCLUDE REGEX "/scr_tiles.c$")
endif()

if(CONFIG_LS_LR2021_ONLY)
    list(FILTER APPS_C_SRCS EXCLUDE REGEX "/(scr_cell|ls_catch_ui)\.c$")
else()
    list(FILTER APPS_C_SRCS EXCLUDE REGEX "/ls_catch_stub\.c$")
endif()

idf_component_register(
    SRCS ${APPS_C_SRCS} ${APPS_CPP_SRCS}
    INCLUDE_DIRS ${APPS_DIR}
    REQUIRES espressif__elf_loader mbedtls console esp_timer libcarto pmtiles meshcore esp_event esp_wifi esp_netif lwip esp_hw_support esp_app_format nvs_flash esp_mm bsp_extra esp32_p4_wifi6_touch_lcd_4b espressif__esp_lcd_touch_gt911 esp_http_client esp_http_server json vfs lakeshark esp_adc esp_driver_jpeg chmorgan__esp-file-iterator chmorgan__esp-audio-player david-cermak__libssh)

target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/../../data/drone-watch/defaults.json" TEXT)

# The web screen's page (ls_rterm.c), its scripts inline and gzipped here so
# that the readable files are the ones kept; the icons as they are.
set(LS_WEB ${CMAKE_CURRENT_LIST_DIR}/tui/web)
set(LS_WEB_PAGE ${CMAKE_CURRENT_BINARY_DIR}/screen.html.gz)
idf_build_get_property(LS_PYTHON PYTHON)
add_custom_command(OUTPUT ${LS_WEB_PAGE}
    COMMAND ${LS_PYTHON} ${LS_WEB}/pack.py ${LS_WEB}/screen.html ${LS_WEB_PAGE}
    DEPENDS ${LS_WEB}/pack.py ${LS_WEB}/screen.html ${LS_WEB}/screen.js ${LS_WEB}/xterm-headless.js
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} ${LS_WEB_PAGE} BINARY)
foreach(s 180 192 512)
    target_add_binary_data(${COMPONENT_LIB} ${LS_WEB}/icon-${s}.png BINARY)
endforeach()
if(CONFIG_LS_BOARD_T_DISPLAY_P4 OR CONFIG_LS_PANEL_RUNTIME)
    # CONFIG is available at registration time, not in IDF's early dependency scan.
    idf_component_optional_requires(PRIVATE cartocore console esp_timer mbedtls)
endif()

# -Wformat and the two pointer checks are turned back ON here, against the
# project-wide -Wno-format in the root CMakeLists.  That flag is there for
# ESP-IDF and the vendored trees - esp_log_color.h alone accounts for ten
# diagnostics, and with -Werror on 1840 of 1841 translation units it cannot
# simply be dropped - but it was covering this component too, which is how a
# %f handed an int in scr_subghz.c reached a device without the build saying
# a word.  Asking the whole build what these cost found fifteen, of which one
# was ours; none at all were in here, so switching them on is free today and
# is the point at which the next one gets caught.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wformat
        -Wno-format-truncation
        -Wincompatible-pointer-types
        -Wint-conversion
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wformat
        -Wno-format-truncation
        -Wno-conversion-null
        -Wno-attributes
        -Wno-write-strings
    >
)

# Size build (CONFIG_COMPILER_OPTIMIZATION_SIZE): the screens are compiled for
# size, but what runs every frame keeps -O2 - the TUI renderer, fonts and
# glyphs, the dot canvas, waterfalls, the radio deck, the globe and MAP (its
# screen and the FIELD tile cache). -Os everywhere doubled UI-task CPU
# (6.6 -> 12 %); MAP left at -Os took 46 ms a frame where 2.16.4 took 8.
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    file(GLOB LS_HOT_TUI ${CMAKE_CURRENT_LIST_DIR}/tui/tuilib/*.c
         ${CMAKE_CURRENT_LIST_DIR}/tui/lap3d/*.c)
    foreach(f ls_tui.c ls_tui_screen.c ls_tui_chrome.c ls_tui_ui.c ls_tui_touch.c ls_tui_inset.c
              ls_glyph.c ls_font_mono_14.c ls_font_mono_16.c ls_font_mono_24.c ls_font_cyr.c
              ls_font_list.c ls_text.c ls_textbuf.c ls_map_ink.c ls_pixel_wf.c ls_waterfall.c
              ls_wf_source.c ls_radio_panel.c ls_stroke.c ls_anim.c ls_tui_png.c ls_globe.c
              ls_cartocore.c ls_theme.c ls_i18n.c ls_map.c screens/ext/scr_map.c ls_lap_api.c)
        list(APPEND LS_HOT_TUI ${CMAKE_CURRENT_LIST_DIR}/tui/${f})
    endforeach()
    set_source_files_properties(${LS_HOT_TUI} PROPERTIES COMPILE_OPTIONS "-O2")
endif()

# Recursive APPS_C_SRCS includes lap3d; keep its public SDK header independent
# of any firmware screen. Emit stack evidence for the small-stack TUI caller.
file(GLOB LS_LAP3D ${CMAKE_CURRENT_LIST_DIR}/tui/lap3d/*.c)
set_property(SOURCE ${LS_LAP3D} APPEND PROPERTY COMPILE_OPTIONS
    -O2 -fstack-usage -Wdouble-promotion -Wfloat-conversion)
option(L3_PROFILE "Profile lap3d passes every 50 frames" OFF)
if(L3_PROFILE)
    set_property(SOURCE ${LS_LAP3D} APPEND PROPERTY COMPILE_DEFINITIONS L3_PROFILE)
endif()
