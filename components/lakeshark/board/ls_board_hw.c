/* See ls_board_hw.h. */
#include "ls_board_hw.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ls_board.h"
#include "ls_caps.h"

#if LS_HAS_IO_EXPANDER
#include "ls_xl9535.h"
#endif

static const char *TAG = "board_hw";

/* Which mechanism drives C6 enable on this board, if any. */
#if LS_HAS_IO_EXPANDER && defined(LS_BOARD_XL_C6_EN)
#define C6_VIA_EXPANDER 1
#else
#define C6_VIA_EXPANDER 0
#endif
#if defined(LS_BOARD_C6_EN_GPIO) && (LS_BOARD_C6_EN_GPIO >= 0)
#define C6_VIA_GPIO 1
#else
#define C6_VIA_GPIO 0
#endif

esp_err_t ls_board_hw_c6_enable(bool on)
{
#if C6_VIA_EXPANDER
    if (!ls_xl9535_ready()) return ESP_ERR_INVALID_STATE;
    return on ? ls_board_hw_c6_release() : ls_xl9535_out(LS_BOARD_XL_C6_EN, false);
#elif C6_VIA_GPIO
    gpio_config_t out = {
        .pin_bit_mask = 1ULL << (unsigned)LS_BOARD_C6_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out);
    return gpio_set_level((gpio_num_t)LS_BOARD_C6_EN_GPIO, on ? 1 : 0);
#else
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* Leave EN undriven when high so the physical reset button works. */
esp_err_t ls_board_hw_c6_release(void)
{
#if C6_VIA_EXPANDER
    if (!ls_xl9535_ready()) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(ls_xl9535_set(LS_BOARD_XL_C6_EN, true), TAG, "C6 EN latch");
    return ls_xl9535_set_dir(LS_BOARD_XL_C6_EN, false);
#elif C6_VIA_GPIO
    return gpio_set_direction((gpio_num_t)LS_BOARD_C6_EN_GPIO, GPIO_MODE_INPUT);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int ls_board_hw_c6_state(void)
{
#if C6_VIA_EXPANDER
    bool level = false;
    if (!ls_xl9535_ready()) return -1;
    if (ls_xl9535_get(LS_BOARD_XL_C6_EN, &level) != ESP_OK) return -1;
    return level ? 1 : 0;
#elif C6_VIA_GPIO
    gpio_config_t in = {
        .pin_bit_mask = 1ULL << (unsigned)LS_BOARD_C6_EN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in);
    return gpio_get_level((gpio_num_t)LS_BOARD_C6_EN_GPIO);
#else
    return -1;
#endif
}

bool ls_board_hw_ready(void)
{
#if LS_HAS_IO_EXPANDER
    return ls_xl9535_ready();
#else
    return true;
#endif
}

/* See the header. One pin, and the level convention is the vendor's:
   1 is the internal antenna, 0 is external through MMCX1. */
#ifdef LS_BOARD_XL_RF_SW_VCTL
static bool s_ant_external;
#endif

esp_err_t ls_board_hw_antenna_external(bool external)
{
#ifdef LS_BOARD_XL_RF_SW_VCTL
    const esp_err_t err = ls_xl9535_out(LS_BOARD_XL_RF_SW_VCTL, !external);
    if (err == ESP_OK) s_ant_external = external;
    return err;
#else
    (void)external;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool ls_board_hw_antenna_is_external(void)
{
#ifdef LS_BOARD_XL_RF_SW_VCTL
    return s_ant_external;
#else
    return false;
#endif
}

esp_err_t ls_board_hw_touch_reset(void)
{
#if LS_HAS_IO_EXPANDER && defined(LS_BOARD_XL_TOUCH_RST)
    /* Match vendor InitGt9895 at a47cf73e: INT is an input,
       reset stays low for 30 ms, then allow 100 ms before I2C access.
       Do not guess a GT911 address-strapping sequence for the GT9895. */
    ESP_RETURN_ON_ERROR(ls_xl9535_set_dir(LS_BOARD_XL_TOUCH_INT, false), TAG, "touch INT");
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_TOUCH_RST, false), TAG, "touch reset low");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(ls_xl9535_set(LS_BOARD_XL_TOUCH_RST, true), TAG, "touch reset high");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t ls_board_hw_early_init(void)
{
#if !LS_HAS_IO_EXPANDER
    return ESP_OK;
#else
    esp_err_t err = ls_xl9535_init((ls_i2c_bus_id_t)LS_BOARD_IO_EXPANDER_BUS,
                                   LS_BOARD_IO_EXPANDER_ADDR);
    if (err != ESP_OK) {
        /* Deliberately not fatal.  A board whose rails are all behind the
           expander will fail later anyway, and it fails somewhere that names
           the actual peripheral - which is more use than an abort here.  The
           safe-mode stage record is what turns that into a diagnosis. */
        ESP_LOGE(TAG, "IO expander did not come up (%s) - rails stay off",
                 esp_err_to_name(err));
        return err;
    }

#ifdef LS_BOARD_XL_SCREEN_RST
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_SCREEN_RST, false), TAG, "rail/reset write failed");
#endif
#ifdef LS_BOARD_XL_TOUCH_RST
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_TOUCH_RST, false), TAG, "rail/reset write failed");
#endif

#ifdef LS_BOARD_XL_PWR_3V3_EN
    /* IO0 is active-low: vendor InitXl9535 at a47cf73e drives
       kPowerEn3v3 to 0 and warns that this rail must remain enabled.
       Treating every *_EN as active-high disabled it before reset release. */
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_PWR_3V3_EN, false), TAG, "rail/reset write failed");
    /* The rail needs to be up before anything on it is released.  10 ms is
       the vendor's own settling time for this board. */
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

#ifdef LS_BOARD_XL_AUDIO_PWR_EN
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_AUDIO_PWR_EN, true), TAG, "rail/reset write failed");
#endif
#ifdef LS_BOARD_XL_SD_PWR_EN
    /* SD power is also active-low. Vendor SD init drives IO15
       low to power the card and high on teardown. */
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_SD_PWR_EN, false), TAG, "rail/reset write failed");
#endif
#ifdef LS_BOARD_XL_USB_PHY_EN
    /* Retain the vendor's PHY-enable sequence. This does not generate USB-A
       VBUS: the AMOLED V1.0 schematic (202601061148, sheet 8) feeds U19 from
       VCC_5V, while the internal battery feeds INPUT_5V. No boost connects
       the battery to VCC_5V. See docs/T_DISPLAY_P4_USB_POWER.md. */
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_USB_PHY_EN, true), TAG, "rail/reset write failed");
#endif

    /* Release resets now that the rail is up. */
#ifdef LS_BOARD_XL_SCREEN_RST
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ls_xl9535_set(LS_BOARD_XL_SCREEN_RST, true), TAG, "rail/reset write failed");
#endif
#ifdef LS_BOARD_XL_TOUCH_RST
    ESP_RETURN_ON_ERROR(ls_board_hw_touch_reset(), TAG, "touch reset");
#endif
#if defined(LS_BOARD_XL_SCREEN_RST) || defined(LS_BOARD_XL_TOUCH_RST)
    vTaskDelay(pdMS_TO_TICKS(20));
#endif

#if C6_VIA_EXPANDER
    /* Restart the companion with the host so SDIO receives a fresh init event. */
    ESP_RETURN_ON_ERROR(ls_board_hw_c6_enable(false), TAG, "C6 reset assertion failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(ls_board_hw_c6_release(), TAG, "C6 reset release failed");
#endif

    /* Radios stay in reset.  There is no driver for either yet, and holding
       a part in reset is the honest default for something the firmware
       cannot talk to. */
#ifdef LS_BOARD_XL_RF_SW_VCTL
    /* Park the antenna switch before anything can transmit. */

    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_RF_SW_VCTL, true), TAG,
                        "rf switch park failed");
#endif
#ifdef LS_BOARD_XL_RADIO_RST
    ESP_RETURN_ON_ERROR(ls_xl9535_out(LS_BOARD_XL_RADIO_RST, false), TAG, "rail/reset write failed");
#endif

    ESP_LOGI(TAG, "IO expander rails up");
    return ESP_OK;
#endif
}
