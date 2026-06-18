idf_component_register(
    SRCS mesh_gateway.cpp
    INCLUDE_DIRS include
    REQUIRES loramesher espressif__arduino-esp32 driver
             esp_driver_uart vfs)
