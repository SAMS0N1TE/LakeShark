cmake_minimum_required(VERSION 3.5)

set(CMAKE_OBJECT_PATH_MAX 512)

set(CARTOCORE_DIR "$ENV{CARTOCORE_DIR}" CACHE PATH "CartoCore engine checkout (defaults to the vendored snapshot)")
# An empty setting means the snapshot in components/cartocore/upstream, which
# components/cartocore builds itself; only an external checkout adds its own
# component directory.
set(_ls_cc_external FALSE)
if(CARTOCORE_DIR)
    set(_ls_cc_external TRUE)
else()
    set(CARTOCORE_DIR "${CMAKE_CURRENT_LIST_DIR}/components/cartocore/upstream")
endif()

# Local CartoCore bench: discover the engine only for the T-Display-P4 profile.
set(_ls_p4_profile "${SDKCONFIG_DEFAULTS}")
if(DEFINED SDKCONFIG AND EXISTS "${SDKCONFIG}")
    file(STRINGS "${SDKCONFIG}" _ls_p4_board REGEX "^CONFIG_LS_BOARD_T_DISPLAY_P4=y$")
endif()
if(_ls_p4_profile MATCHES "t_display_p4.defaults" OR _ls_p4_board)
    if(NOT EXISTS "${CARTOCORE_DIR}/include/cartocore/render.h" OR NOT EXISTS "${CARTOCORE_DIR}/components")
        message(FATAL_ERROR "CartoCore missing: set -DCARTOCORE_DIR=<engine checkout> or the CARTOCORE_DIR environment variable")
    endif()
    if(_ls_cc_external)
        list(APPEND EXTRA_COMPONENT_DIRS "${CARTOCORE_DIR}/components")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXCLUDE_COMPONENTS chmorgan__esp-libhelix-mp3)

set(CXX_COMPILE_OPTIONS
    -Wno-deprecated-enum-enum-conversion
    -Wno-write-strings
)

add_compile_options(
    -Wno-format
    -Wno-unused-variable
    -Wno-missing-field-initializers
    -Wno-unused-but-set-variable
    -Wno-cast-function-type
    -Wno-ignored-qualifiers
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated-enum-enum-conversion")

set(LS_PRODUCT_VERSION "2.20.0-rc1")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ls_build_identity.cmake")
set(PROJECT_VER "${LS_PRODUCT_VERSION}")
set(LS_PROJECT_VERSION "2.20.0")
project(lakeshark VERSION ${LS_PROJECT_VERSION})

# Selective placement avoids moving the whole FreeRTOS kernel back to SRAM.
# Both task lookups in panic_crumb's IRAM tick hook and the flash IPC scheduler
# handoff must remain executable when the other core suspends shared cache.
if(CONFIG_IDF_TARGET_ESP32P4 AND CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH)
    idf_component_get_property(ls_freertos_dir freertos COMPONENT_DIR)
    idf_component_get_property(ls_freertos_lib freertos COMPONENT_LIB)
    foreach(ls_cache_safe_source
            "${ls_freertos_dir}/FreeRTOS-Kernel/tasks.c"
            "${ls_freertos_dir}/FreeRTOS-Kernel/portable/riscv/port.c")
        set_property(SOURCE "${ls_cache_safe_source}"
            TARGET_DIRECTORY ${ls_freertos_lib} APPEND PROPERTY COMPILE_OPTIONS
            "-include${CMAKE_CURRENT_LIST_DIR}/main/ls_freertos_cache_safe.h")
    endforeach()
endif()

if(CONFIG_IDF_TARGET_ESP32P4 AND CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM)
    idf_component_get_property(ls_usb_dir usb COMPONENT_DIR)
    idf_component_get_property(ls_usb_lib usb COMPONENT_LIB)
    set_property(SOURCE "${ls_usb_dir}/hcd_dwc.c"
        TARGET_DIRECTORY ${ls_usb_lib} APPEND PROPERTY COMPILE_OPTIONS
        "-include${CMAKE_CURRENT_LIST_DIR}/components/lakeshark/board/ls_usb_descriptors.h")
endif()

if(CONFIG_APP_PROJECT_VER_FROM_CONFIG OR CONFIG_APP_EXCLUDE_PROJECT_VER_VAR)
    message(FATAL_ERROR "LakeShark build identity requires project version override/exclusion disabled")
endif()
set(_ls_identity_header "${CMAKE_BINARY_DIR}/generated/ls_build_identity.h")
ls_identity_write_header("${_ls_identity_header}" "${PROJECT_VER}")
add_custom_target(ls_refresh_build_identity
    COMMAND "${CMAKE_COMMAND}"
        "-DLS_IDENTITY_SOURCE=${CMAKE_SOURCE_DIR}"
        "-DLS_IDENTITY_PRODUCT=${LS_PRODUCT_VERSION}"
        "-DLS_IDENTITY_HEADER=${_ls_identity_header}"
        "-DLS_IDENTITY_METADATA=${CMAKE_BINARY_DIR}/project_description.json"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P "${CMAKE_SOURCE_DIR}/cmake/ls_refresh_identity.cmake"
    BYPRODUCTS "${_ls_identity_header}"
    VERBATIM)
add_dependencies(__idf_esp_app_format ls_refresh_build_identity)
target_compile_options(__idf_esp_app_format PRIVATE -include "${_ls_identity_header}")

# Cold ESP-IDF libraries build for size. The project default is -O2
# (CONFIG_COMPILER_OPTIMIZATION_PERF) for the radio, audio and render paths;
# none of these sit in a timing-critical loop, and -Os gives the same
# behaviour in less flash. The options come after the global -O2, so they win.
# Placement attributes (IRAM_ATTR and friends) are not touched by -Os.
set(LS_SIZE_OPT_COMPONENTS
    bt lwip esp_netif esp_http_client esp_http_server tcp_transport esp-tls
    console nvs_flash espcoredump spiffs json http_parser
    espressif__esp_hosted)
foreach(ls_cold_comp IN LISTS LS_SIZE_OPT_COMPONENTS)
    idf_component_get_property(ls_cold_lib ${ls_cold_comp} COMPONENT_LIB)
    if(ls_cold_lib)
        get_target_property(ls_cold_type ${ls_cold_lib} TYPE)
        if(NOT ls_cold_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${ls_cold_lib} PRIVATE -Os)
        endif()
    endif()
endforeach()

# C++ exceptions stay on (CONFIG_COMPILER_CXX_EXCEPTIONS) for the one place
# that catches: speech_engine.cpp catches std::bad_alloc out of klatt_tts.
# The C++ in these components never catches, so an exception thrown there
# ends in std::terminate either way; without unwind tables and landing pads
# they are about 30 KB smaller. ezpwd (P25 Phase II Reed-Solomon) is built
# with EZPWD_NO_EXCEPTS: its checks (a constant table, codeword sizes fixed
# by P25) abort or return an error where they would have thrown.
set(LS_NO_EXCEPT_COMPONENTS
    main meshcore imbe_vocoder p25_phase2 chmorgan__esp-audio-player nvs_flash)
foreach(ls_ne_comp IN LISTS LS_NO_EXCEPT_COMPONENTS)
    idf_component_get_property(ls_ne_lib ${ls_ne_comp} COMPONENT_LIB)
    if(ls_ne_lib)
        target_compile_options(${ls_ne_lib} PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>)
    endif()
endforeach()

# Size build: the code compiled for size also calls shared register
# save/restore routines (libgcc millicode, linked into IRAM by IDF) in place
# of inline prologues and epilogues: about 3% less code for a few cycles per
# call. The per-frame, DSP and radio files keep -O2 and add -mno-save-restore
# (components/apps and components/lakeshark CMakeLists.txt). The IDF list is
# LS_SIZE_OPT_COMPONENTS above; the audio decoders, klatt_tts and the vocoders
# are left alone.
if(CONFIG_COMPILER_OPTIMIZATION_SIZE)
    set(LS_SAVE_RESTORE_COMPONENTS apps lakeshark main meshcore ${LS_SIZE_OPT_COMPONENTS})
    foreach(ls_sr_comp IN LISTS LS_SAVE_RESTORE_COMPONENTS)
        idf_component_get_property(ls_sr_lib ${ls_sr_comp} COMPONENT_LIB)
        if(ls_sr_lib)
            get_target_property(ls_sr_type ${ls_sr_lib} TYPE)
            if(NOT ls_sr_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${ls_sr_lib} PRIVATE -msave-restore)
            endif()
        endif()
    endforeach()
    # More cold libraries, none on a sample or frame path: TLS and
    # certificates (handshakes, OTA), the FAT and VFS layers (file calls, not
    # the SD transfers underneath), OTA partition writes, and the text packs.
    # mbedtls builds three CMake targets of its own beside the IDF wrapper.
    set(LS_SAVE_RESTORE_MORE fatfs vfs wear_levelling app_update esp_https_ota
        esp_partition ls_textpack mbedtls)
    foreach(ls_sr_comp IN LISTS LS_SAVE_RESTORE_MORE)
        idf_component_get_property(ls_sr_lib ${ls_sr_comp} COMPONENT_LIB)
        if(ls_sr_lib)
            get_target_property(ls_sr_type ${ls_sr_lib} TYPE)
            if(NOT ls_sr_type STREQUAL "INTERFACE_LIBRARY")
                target_compile_options(${ls_sr_lib} PRIVATE -msave-restore)
            endif()
        endif()
    endforeach()
    foreach(ls_sr_lib mbedcrypto mbedx509 mbedtls)
        if(TARGET ${ls_sr_lib})
            target_compile_options(${ls_sr_lib} PRIVATE -msave-restore)
        endif()
    endforeach()
endif()

# esp_hosted's protobuf-c tables name every message, field and enum value,
# about 40 KB of strings. Nothing reads them: packing goes by number and
# protobuf-c's unpack error macro is built empty. The library compiles a copy
# with the names blank (cmake/ls_pb_strip_names.py); the RpcId value names
# stay, because rpc_core.c logs them on a timeout.
idf_component_get_property(ls_hosted_lib espressif__esp_hosted COMPONENT_LIB)
if(ls_hosted_lib)
    get_target_property(ls_hosted_srcs ${ls_hosted_lib} SOURCES)
    list(FILTER ls_hosted_srcs INCLUDE REGEX "/esp_hosted_rpc\\.pb-c\\.c$")
    list(LENGTH ls_hosted_srcs ls_pb_n)
    if(ls_pb_n EQUAL 1)
        list(GET ls_hosted_srcs 0 ls_pb_src)
        set(ls_pb_tool "${CMAKE_CURRENT_LIST_DIR}/cmake/ls_pb_strip_names.py")
        set(ls_pb_out "${CMAKE_BINARY_DIR}/ls_hosted/esp_hosted_rpc.pb-c.c")
        idf_build_get_property(ls_python PYTHON)
        execute_process(COMMAND ${ls_python} ${ls_pb_tool} ${ls_pb_src} ${ls_pb_out}
                rpc_id__enum_values_by_number
            RESULT_VARIABLE ls_pb_rc)
        if(NOT ls_pb_rc EQUAL 0)
            message(FATAL_ERROR "ls_pb_strip_names.py failed on ${ls_pb_src}")
        endif()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${ls_pb_tool} ${ls_pb_src})
        get_target_property(ls_hosted_all ${ls_hosted_lib} SOURCES)
        list(TRANSFORM ls_hosted_all REPLACE "^.*/esp_hosted_rpc\\.pb-c\\.c$" "${ls_pb_out}")
        set_property(TARGET ${ls_hosted_lib} PROPERTY SOURCES ${ls_hosted_all})
    endif()
endif()

idf_component_get_property(ls_p2_lib p25_phase2 COMPONENT_LIB)
if(ls_p2_lib)
    target_compile_definitions(${ls_p2_lib} PRIVATE EZPWD_NO_EXCEPTS)
endif()
