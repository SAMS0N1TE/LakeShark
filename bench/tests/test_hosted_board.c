/* LS_TEST_SOURCES: ${FW}/main/ls_hosted_board.c */
#include "ls_test.h"
#include "driver/gpio.h"
#include "ls_hosted_board.h"
#include "ls_board_hw.h"
#include "port_esp_hosted_host_config.h"
#include "esp_hosted_os_abstraction.h"

hosted_osi_funcs_t g_hosted_osi_funcs;
static unsigned gpio_calls, board_calls;
static int reset_level = 1;
static bool released;
static esp_err_t board_result;

esp_err_t ls_board_hw_c6_enable(bool on)
{
    ++board_calls;
    reset_level = on;
    return board_result;
}
esp_err_t ls_board_hw_c6_release(void)
{
    ++board_calls;
    released = true;
    return board_result;
}
int ls_board_hw_c6_state(void) { return reset_level; }

static int gpio2(void *port, uint32_t pin)
{
    (void)port; (void)pin;
    ++gpio_calls;
    return 37;
}
static int gpio3(void *port, uint32_t pin, uint32_t value)
{
    (void)value;
    return gpio2(port, pin);
}
static int gpio4(void *port, uint32_t pin, uint32_t value, uint32_t enable)
{
    (void)enable;
    return gpio3(port, pin, value);
}

LS_CASE(hosted_reset_uses_expander_and_preserves_other_gpio_operations)
{
    g_hosted_osi_funcs._h_config_gpio = gpio3;
    g_hosted_osi_funcs._h_read_gpio = gpio2;
    g_hosted_osi_funcs._h_write_gpio = gpio3;
    g_hosted_osi_funcs._h_hold_gpio = gpio3;
    g_hosted_osi_funcs._h_pull_gpio = gpio4;
    ls_hosted_board_init();
    ls_hosted_board_init();
    hosted_osi_funcs_t *ops = &g_hosted_osi_funcs;
    LS_EQ_INT(ops->_h_config_gpio(NULL, 54, GPIO_MODE_OUTPUT), ESP_OK);
    LS_EQ_INT(ops->_h_write_gpio(NULL, 54, 0), ESP_OK);
    LS_EQ_INT(ops->_h_read_gpio(NULL, 54), 0);
    LS_EQ_INT(ops->_h_write_gpio(NULL, 54, 1), ESP_OK);
    LS_EQ_INT(ops->_h_read_gpio(NULL, 54), 1);
    LS_EQ_INT(ops->_h_config_gpio(NULL, 54, GPIO_MODE_INPUT), ESP_OK);
    LS_CHECK(released);
    LS_EQ_INT(ops->_h_hold_gpio(NULL, 54, 1), ESP_OK);
    LS_EQ_INT(ops->_h_pull_gpio(NULL, 54, 0, 0), ESP_OK);
    LS_EQ_INT(ops->_h_pull_gpio(NULL, 54, 0, 1), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(gpio_calls, 0);
    LS_EQ_INT(board_calls, 4);

    LS_EQ_INT(ops->_h_config_gpio(NULL, 18, GPIO_MODE_OUTPUT), 37);
    LS_EQ_INT(ops->_h_read_gpio(NULL, 18), 37);
    LS_EQ_INT(ops->_h_write_gpio(NULL, 18, 0), 37);
    LS_EQ_INT(ops->_h_hold_gpio(NULL, 18, 1), 37);
    LS_EQ_INT(ops->_h_pull_gpio(NULL, 18, 0, 1), 37);
    LS_EQ_INT(ops->_h_write_gpio(&reset_level, 54, 0), 37);
    LS_EQ_INT(gpio_calls, 6);
    LS_EQ_INT(board_calls, 4);

    board_result = ESP_ERR_INVALID_STATE;
    LS_EQ_INT(ops->_h_write_gpio(NULL, 54, 0), board_result);
    LS_EQ_INT(ops->_h_config_gpio(NULL, 54, GPIO_MODE_INPUT), board_result);
    reset_level = -1;
    LS_EQ_INT(ops->_h_config_gpio(NULL, 54, GPIO_MODE_OUTPUT), board_result);
    LS_EQ_INT(gpio_calls, 6);
}
