set(fw "${CMAKE_CURRENT_LIST_DIR}/../../c6_firmware")
idf_component_register(SRCS c6_setup.c
    INCLUDE_DIRS .
    REQUIRES esp-serial-flasher esp_driver_i2c esp_driver_sdmmc esp_timer mbedtls
    EMBED_FILES "${fw}/bootloader.bin" "${fw}/partition-table.bin"
                "${fw}/ota_data_initial.bin" "${fw}/network_adapter.bin")
