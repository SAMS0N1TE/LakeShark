cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32p4)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../app-sdk/native/e3d"
    "${CMAKE_CURRENT_LIST_DIR}/../app-sdk/native/bsp30"
    "${CMAKE_CURRENT_LIST_DIR}/../app-sdk/native/tk21"
    "${CMAKE_CURRENT_LIST_DIR}/../app-sdk/native/waveshare_p4_sd")
set(LAKESHARK_GPSP_DIR "" CACHE PATH "Optional checkout of the pinned gpSP ESP32 source")
if(LAKESHARK_GPSP_DIR)
    get_filename_component(LAKESHARK_GPSP_DIR "${LAKESHARK_GPSP_DIR}" ABSOLUTE)
    if(NOT EXISTS "${LAKESHARK_GPSP_DIR}/components/gpsp/core/include/gpsp_rv32_platform.h")
        message(FATAL_ERROR "LAKESHARK_GPSP_DIR needs the gpSP ESP32 source checkout")
    endif()
    list(APPEND EXTRA_COMPONENT_DIRS "${LAKESHARK_GPSP_DIR}/components/gpsp")
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;gba/sdkconfig.defaults")
    endif()
endif()
set(ENV{LAKESHARK_GAME_GPSP_DIR} "${LAKESHARK_GPSP_DIR}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
set(PROJECT_VER "0.2.0")
project(lakeshark_game)
include(../app-sdk/native/waveshare_p4_sd/fatfs-exfat.cmake)

if(LAKESHARK_GPSP_DIR)
    if(CONFIG_SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION)
        message(FATAL_ERROR "gpSP needs executable PSRAM. Apply gba/sdkconfig.defaults to a fresh build or disable SPIRAM_PRE_CONFIGURE_MEMORY_PROTECTION.")
    endif()
    idf_component_get_property(gpsp_target gpsp COMPONENT_LIB)
    get_target_property(gpsp_options ${gpsp_target} COMPILE_OPTIONS)
    if(IDF_VERSION_MAJOR EQUAL 5)
        # GCC 14 recognizes the atomic subsets through "a", without individual names.
        string(REPLACE "_zaamo_zalrsc" "" gpsp_options "${gpsp_options}")
    endif()
    if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3)
        # Pre-v3 P4 lacks the standard bit-manipulation instructions used on newer chips.
        # Apply the same ceiling to GCC and to instructions emitted by the RV32 JIT.
        string(REPLACE "_zba_zbb_zbs_zcb_xesploop" "" gpsp_options "${gpsp_options}")
        get_target_property(gpsp_defs ${gpsp_target} COMPILE_DEFINITIONS)
        string(REGEX REPLACE "(GPSP_RV32_USE_Z(BA|BB|BS|CB))=[01]" "\\1=0"
               gpsp_defs "${gpsp_defs}")
        set_property(TARGET ${gpsp_target} PROPERTY COMPILE_DEFINITIONS "${gpsp_defs}")
    endif()
    set_property(TARGET ${gpsp_target} PROPERTY COMPILE_OPTIONS "${gpsp_options}")
endif()
