// src/hardware/hal_factory.hpp
#pragma once

#include "hal.hpp"
#include "types/configurations/pin_configuration.hpp"

#ifdef ARDUINO
#include "arduino/arduino_hal.hpp"
#else
#include "native/native_hal.hpp"
#endif

#include <memory>

namespace loramesher {
namespace hal {

/**
 * @brief Factory class for creating platform-specific HAL instances.
 */
class HalFactory {
   public:
    /**
     * @brief Create an appropriate HAL instance based on the current platform.
     *
     * @param pin_config Pin configuration, used to initialize the SPI bus
     *                   with custom SCK/MISO/MOSI pins when configured.
     * @return std::unique_ptr<IHal> A unique pointer to the created HAL instance.
     */
    static std::unique_ptr<IHal> createHal(const PinConfig& pin_config) {
#ifdef ARDUINO
        return std::unique_ptr<IHal>(new LoraMesherArduinoHal(pin_config));
#else
        return std::unique_ptr<IHal>(new NativeHal(pin_config));
#endif
    }
};

}  // namespace hal
}  // namespace loramesher