# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# The configuration is to prevent compilation errors on the Windows system.
set(CMAKE_OBJECT_PATH_MAX 512)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

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

set(LS_PRODUCT_VERSION "1.0.1")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/ls_build_identity.cmake")
ls_identity_query(PROJECT_VER "${CMAKE_CURRENT_LIST_DIR}" "${LS_PRODUCT_VERSION}")
project(lakeshark VERSION ${LS_PRODUCT_VERSION})

# LS-220: project(VERSION) takes precedence over IDF's Git fallback. A plain
# 1.0.1 therefore hid every revision and dirty build. Keep app_desc current
# even when an ordinary source edit does not trigger CMake reconfiguration.
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
