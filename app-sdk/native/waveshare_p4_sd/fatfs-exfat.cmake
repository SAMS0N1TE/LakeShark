# ESP-IDF 5.5 bundles exFAT support but disables it in ffconf.h. FatFs and
# its consumers need identical settings because exFAT changes public types.
if(NOT CONFIG_FATFS_LFN_HEAP AND NOT CONFIG_FATFS_LFN_STACK)
    message(STATUS "microSD exFAT support requires FatFs long filenames")
    return()
endif()
idf_component_get_property(fatfs_lib fatfs COMPONENT_LIB)
idf_component_get_property(fatfs_dir fatfs COMPONENT_DIR)

# Sources beside the upstream ff.h resolve it locally. Apply the override
# there and put the forwarding header first for external consumers.
set(fatfs_overlay "${CMAKE_CURRENT_BINARY_DIR}/fatfs-exfat")
configure_file("${CMAKE_CURRENT_LIST_DIR}/ff.h.in" "${fatfs_overlay}/ff.h" @ONLY)
target_compile_options(${fatfs_lib} PRIVATE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-include${fatfs_overlay}/ff.h>")
target_include_directories(${fatfs_lib} BEFORE PUBLIC "${fatfs_overlay}")
