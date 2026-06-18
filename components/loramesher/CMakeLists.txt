# LoRaMesher + RadioLib vendored as ONE Arduino-style IDF component.
#
# Why together: RadioLib only selects its ArduinoHal (the HAL that works on the
# ESP32-P4) when ARDUINO is defined at its own compile time. Compiling RadioLib
# inside this component — which defines ARDUINO — guarantees that, mirroring the
# PlatformIO single-library setup that already runs on the gateway_p4 board.
# The vendored sx1262.cpp carries the required setDio2AsRfSwitch(true) patch.

file(GLOB_RECURSE LM_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE RL_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/radiolib/*.cpp")

idf_component_register(
    SRCS ${LM_SRCS} ${RL_SRCS}
    INCLUDE_DIRS "src" "include" "radiolib"
    REQUIRES driver esp_timer espressif__arduino-esp32
)

# ARDUINO selects the Arduino build path in LoRaMesher (system_config.hpp) and
# RadioLib (BuildOpt.h -> ArduinoHal). 10812 matches arduino-esp32 3.x.
target_compile_definitions(${COMPONENT_LIB} PUBLIC ARDUINO=10812)

# Third-party sources: silence their warnings so the project's strict flags
# don't fail the build on code we don't own.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
