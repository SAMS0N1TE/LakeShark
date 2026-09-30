# ESP-IDF v5.4.3's esp_driver_i2c, overriding the IDF copy, with one change
# from v5.5.3: the wait for an idle bus after a NACK times out instead of
# spinning forever (i2c_master.c, s_i2c_send_commands). Delete this directory
# when the project moves to an IDF with that fix.
idf_build_get_property(target IDF_TARGET)

set(srcs)
set(include "include")

# I2C related source files
if(CONFIG_SOC_I2C_SUPPORTED)
    list(APPEND srcs
                     "i2c_master.c"
                     "i2c_common.c"
        )
    if(CONFIG_SOC_I2C_SUPPORT_SLAVE)
        if(CONFIG_I2C_ENABLE_SLAVE_DRIVER_VERSION_2)
            list(APPEND srcs "i2c_slave_v2.c")
        else()
            list(APPEND srcs "i2c_slave.c")
        endif()
    endif()

endif()

if(${target} STREQUAL "linux")
    set(priv_requires esp_ringbuf)
else()
    set(priv_requires esp_driver_gpio esp_pm esp_ringbuf)
endif()

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS ${include}
                    PRIV_REQUIRES "${priv_requires}"
                    LDFRAGMENTS "linker.lf"
                    )
