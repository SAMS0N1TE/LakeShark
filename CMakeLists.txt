cmake_minimum_required(VERSION 3.5)

set(CMAKE_OBJECT_PATH_MAX 512)

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

set(LS_PRODUCT_VERSION "2.0.0")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ls_build_identity.cmake")
ls_identity_query(PROJECT_VER "${CMAKE_CURRENT_LIST_DIR}" "${LS_PRODUCT_VERSION}")
project(lakeshark VERSION ${LS_PRODUCT_VERSION})

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
