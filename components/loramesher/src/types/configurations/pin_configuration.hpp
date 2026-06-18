// src/types/configurations/pin_configuration.hpp
#pragma once

#include <cstdint>
#include <string>

namespace loramesher {

/**
 * @brief Configuration class for hardware pin assignments
 *
 * Manages the pin configuration for LoRa radio modules, including
 * chip select (NSS), reset, interrupt pins (DIO0, DIO1), and optional
 * SPI bus data pins (SCK, MISO, MOSI). SPI pins default to -1, which
 * means "use the platform default SPI bus configuration". Set them to
 * a valid pin number to override the default SPI wiring.
 */
class PinConfig {
   public:
    // TODO: Add RADIOLIB_NC for unused pins mapping
    /**
     * @brief Constructs a PinConfig object with specified pin assignments
     *
     * @param nss Chip select pin number
     * @param reset Reset pin number
     * @param dio0 Interrupt pin DIO0 number
     * @param dio1 Interrupt pin DIO1 number
     * @param sck SPI clock pin number (-1 = use platform default)
     * @param miso SPI MISO pin number (-1 = use platform default)
     * @param mosi SPI MOSI pin number (-1 = use platform default)
     */
    explicit PinConfig(int8_t nss = 18, int8_t reset = 23, int8_t dio0 = 26,
                       int8_t dio1 = 33, int8_t sck = -1, int8_t miso = -1,
                       int8_t mosi = -1);

    /**
     * @brief Gets the chip select pin number
     * @return Current NSS pin assignment
     */
    int8_t getNss() const { return nss_; }

    /**
     * @brief Gets the reset pin number
     * @return Current reset pin assignment
     */
    int8_t getReset() const { return reset_; }

    /**
     * @brief Gets the DIO0 interrupt pin number
     * @return Current DIO0 pin assignment
     */
    int8_t getDio0() const { return dio0_; }

    /**
     * @brief Gets the DIO1 interrupt pin number
     * @return Current DIO1 pin assignment
     */
    int8_t getDio1() const { return dio1_; }

    /**
     * @brief Sets the chip select pin with validation
     * @param nss New NSS pin number to assign
     */
    void setNss(int8_t nss);

    /**
     * @brief Sets the reset pin with validation
     * @param reset New reset pin number to assign
     */
    void setReset(int8_t reset);

    /**
     * @brief Sets the DIO0 interrupt pin with validation
     * @param dio0 New DIO0 pin number to assign
     */
    void setDio0(int8_t dio0);

    /**
     * @brief Sets the DIO1 interrupt pin with validation
     * @param dio1 New DIO1 pin number to assign
     */
    void setDio1(int8_t dio1);

    /**
     * @brief Gets the SPI clock pin number
     * @return Current SCK pin assignment (-1 = use platform default)
     */
    int8_t getSck() const { return sck_; }

    /**
     * @brief Gets the SPI MISO pin number
     * @return Current MISO pin assignment (-1 = use platform default)
     */
    int8_t getMiso() const { return miso_; }

    /**
     * @brief Gets the SPI MOSI pin number
     * @return Current MOSI pin assignment (-1 = use platform default)
     */
    int8_t getMosi() const { return mosi_; }

    /**
     * @brief Sets the SPI clock pin
     * @param sck SCK pin number (-1 = use platform default)
     */
    void setSck(int8_t sck) { sck_ = sck; }

    /**
     * @brief Sets the SPI MISO pin
     * @param miso MISO pin number (-1 = use platform default)
     */
    void setMiso(int8_t miso) { miso_ = miso; }

    /**
     * @brief Sets the SPI MOSI pin
     * @param mosi MOSI pin number (-1 = use platform default)
     */
    void setMosi(int8_t mosi) { mosi_ = mosi; }

    /**
     * @brief Checks whether any custom SPI data pin has been configured
     * @return True if at least one of SCK, MISO, or MOSI is not -1
     */
    bool HasCustomSpiPins() const {
        return sck_ != -1 || miso_ != -1 || mosi_ != -1;
    }

    /**
     * @brief Creates a pin configuration with default values
     * @return Default pin configuration object
     */
    static PinConfig CreateDefault();

    /**
     * @brief Validates the pin configuration
     * @return True if configuration is valid, false otherwise
     */
    bool IsValid() const;

    /**
     * @brief Validates the pin configuration and provides error details
     * @return Empty string if valid, otherwise error description
     */
    std::string Validate() const;

   private:
    int8_t nss_;    ///< Chip select pin number
    int8_t reset_;  ///< Reset pin number
    int8_t dio0_;   ///< Interrupt pin DIO0 number
    int8_t dio1_;   ///< Interrupt pin DIO1 number
    int8_t sck_;    ///< SPI clock pin (-1 = platform default)
    int8_t miso_;   ///< SPI MISO pin (-1 = platform default)
    int8_t mosi_;   ///< SPI MOSI pin (-1 = platform default)
};

}  // namespace loramesher