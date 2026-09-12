set(SRCS "")
list(APPEND SRCS
    "src/bsp_board_extra.c"
    "src/bsp_extra_player_state.c"
    "src/bsp_extra_player_open.c"
)

set(INCLUDE_DIRS "")
list(APPEND INCLUDE_DIRS "include")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES driver
    PRIV_REQUIRES esp_timer fatfs esp_psram esp_mm
)
