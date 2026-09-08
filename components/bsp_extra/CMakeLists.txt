set(SRCS "")
# Listed explicitly, and it has to be: SRC_DIRS in the foreach below is never
# set anywhere, so that loop has always been dead code. A .c dropped into src/
# is not compiled and nothing says so until the link fails. Add new sources
# here.
list(APPEND SRCS
    "src/bsp_board_extra.c"
    "src/bsp_extra_player_state.c"
    "src/bsp_extra_player_open.c"
)

set(INCLUDE_DIRS "")
list(APPEND INCLUDE_DIRS "include")

foreach(SRC_DIR IN LISTS SRC_DIRS)
    if(CMAKE_SCRIPT_MODE_FILE)
    # ESP-IDF evaluates component CMakeLists twice, and the requirements pass
    # runs in script mode, where CONFIGURE_DEPENDS is a hard error. Only ask
    # for it on the pass that can act on it.
    file(GLOB_RECURSE SRC ${SRC_DIR}/*.c)
else()
    file(GLOB_RECURSE SRC CONFIGURE_DEPENDS ${SRC_DIR}/*.c)
endif()
    list(APPEND SRCS ${SRC})
    list(APPEND INCLUDE_DIRS ${SRC_DIR}/include)
endforeach()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES driver
    PRIV_REQUIRES esp_timer fatfs esp_psram esp_mm
)
