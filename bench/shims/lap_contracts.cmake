# Wrappers are generated from the same registry used by Studio and docs.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${FW}/app-sdk/tools/sim_contracts.py ${CMAKE_CURRENT_BINARY_DIR}
    RESULT_VARIABLE LAP_CONTRACT_RESULT)
if(NOT LAP_CONTRACT_RESULT EQUAL 0)
    message(FATAL_ERROR "Cannot generate simulator contract wrappers")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${FW}/app-sdk/exports.txt ${FW}/app-sdk/include/lakeshark_app.h ${FW}/app-sdk/include/lakeshark_ui.h
    ${FW}/app-sdk/apidoc.py ${FW}/app-sdk/api_registry.py ${FW}/app-sdk/tools/sim_contracts.py)
target_sources(lssim PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lap_contract_wrappers.c)
include(${CMAKE_CURRENT_BINARY_DIR}/lap_contract_wrappers.cmake)
target_link_options(lssim PRIVATE -Wl,--wrap=ls_lap_api_owns -Wl,--wrap=ls_lap_api_readable -Wl,--wrap=ls_lap_poll)

target_compile_definitions(lssim PRIVATE LS_LAP_SD_ROOT="sdcard")

if(WIN32 AND LSSIM_DYNAMIC_APPS)
    configure_file(${CMAKE_CURRENT_BINARY_DIR}/lssim_contracts.def ${CMAKE_CURRENT_BINARY_DIR}/lssim.def COPYONLY)
endif()
add_executable(test_sdk_libraries ${FW}/app-sdk/tests/test_sdk_libraries.c
    ${FW}/app-sdk/lib/lap_loop.c ${FW}/app-sdk/lib/lap_json.c ${FW}/app-sdk/lib/lap_csv.c)
target_include_directories(test_sdk_libraries PRIVATE ${FW}/app-sdk/lib ${FW}/app-sdk/include)
target_compile_options(test_sdk_libraries PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_sdk_libraries COMMAND test_sdk_libraries)
