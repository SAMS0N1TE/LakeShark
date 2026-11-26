set(house_style "../../components/apps/tui")
set(game_sources main.c display.c flight.c menu.c menu_art.c settings.c
    "${house_style}/ls_font_mono_11.c" "${house_style}/ls_font_mono_13.c" "${house_style}/ls_theme.c")
set(game_requires e3d bsp30 tk21 esp_driver_gpio esp_driver_ledc esp_driver_ppa esp_driver_uart
    esp_lcd esp_timer esp_hw_support nvs_flash waveshare_p4_sd vfs)
if(NOT "$ENV{LAKESHARK_GAME_GPSP_DIR}" STREQUAL "")
    list(APPEND game_sources gba.c)
    list(APPEND game_requires gpsp esp_mm)
    set(gba_assets EMBED_FILES "../gba/demo/signal_catch.gba"
        LDFRAGMENTS "$ENV{LAKESHARK_GAME_GPSP_DIR}/components/gpsp_platform/gpsp_memory.lf")
endif()
idf_component_register(SRCS ${game_sources} INCLUDE_DIRS "." PRIV_INCLUDE_DIRS "${house_style}"
    PRIV_REQUIRES ${game_requires} ${gba_assets})
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
if(NOT "$ENV{LAKESHARK_GAME_GPSP_DIR}" STREQUAL "")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LAKESHARK_GBA=1)
endif()
