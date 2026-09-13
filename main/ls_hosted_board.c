#include "ls_hosted_board.h"
#include "ls_board.h"
#include "ls_board_hw.h"

#if defined(ESP_PLATFORM) && LS_HAS_IO_EXPANDER && defined(LS_BOARD_XL_C6_EN) && CONFIG_LS_C6_LINK
#if !defined(CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6) || !defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE) || !defined(CONFIG_ESP_HOSTED_SDIO_SLOT_1)
#error "T-Display-P4 requires the C6 coprocessor on SDIO slot 1"
#endif
#endif

#if LS_HAS_IO_EXPANDER && defined(LS_BOARD_XL_C6_EN)
#include "driver/gpio.h"
#include "port_esp_hosted_host_config.h"
#include "esp_hosted_os_abstraction.h"

static int (*s_config)(void *, uint32_t, uint32_t);
static int (*s_read)(void *, uint32_t);
static int (*s_write)(void *, uint32_t, uint32_t);
static int (*s_hold)(void *, uint32_t, uint32_t);
static int (*s_pull)(void *, uint32_t, uint32_t, uint32_t);

static bool c6_reset(void *port, uint32_t pin)
{
    return port == NULL && pin == CONFIG_ESP_HOSTED_GPIO_SLAVE_RESET_SLAVE;
}

static int config_gpio(void *port, uint32_t pin, uint32_t mode)
{
    if (!c6_reset(port, pin)) return s_config(port, pin, mode);
    if (mode == GPIO_MODE_INPUT) return ls_board_hw_c6_release();
    if (mode != GPIO_MODE_OUTPUT) return ESP_ERR_NOT_SUPPORTED;
    int level = ls_board_hw_c6_state();
    return level < 0 ? ESP_ERR_INVALID_STATE : ls_board_hw_c6_enable(level != 0);
}

static int read_gpio(void *port, uint32_t pin)
{
    return c6_reset(port, pin) ? ls_board_hw_c6_state() : s_read(port, pin);
}

static int write_gpio(void *port, uint32_t pin, uint32_t value)
{
    return c6_reset(port, pin) ? ls_board_hw_c6_enable(value != 0)
                              : s_write(port, pin, value);
}

static int hold_gpio(void *port, uint32_t pin, uint32_t value)
{
    /* Expander outputs retain their state without a P4 GPIO hold. */
    return c6_reset(port, pin) ? ESP_OK : s_hold(port, pin, value);
}

static int pull_gpio(void *port, uint32_t pin, uint32_t value, uint32_t enable)
{
    if (!c6_reset(port, pin)) return s_pull(port, pin, value, enable);
    return enable ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}
#endif

void ls_hosted_board_init(void)
{
#if LS_HAS_IO_EXPANDER && defined(LS_BOARD_XL_C6_EN)
    if (s_config) return;
    /* Hosted's GPIO54 reset fallback belongs on XL9535 IO14 here. */
    s_config = g_hosted_osi_funcs._h_config_gpio;
    s_read = g_hosted_osi_funcs._h_read_gpio;
    s_write = g_hosted_osi_funcs._h_write_gpio;
    s_hold = g_hosted_osi_funcs._h_hold_gpio;
    s_pull = g_hosted_osi_funcs._h_pull_gpio;
    g_hosted_osi_funcs._h_config_gpio = config_gpio;
    g_hosted_osi_funcs._h_read_gpio = read_gpio;
    g_hosted_osi_funcs._h_write_gpio = write_gpio;
    g_hosted_osi_funcs._h_hold_gpio = hold_gpio;
    g_hosted_osi_funcs._h_pull_gpio = pull_gpio;
#endif
}
