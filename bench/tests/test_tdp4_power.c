/* LS_TEST_SOURCES: ${FW}/components/lakeshark/board/ls_board_hw.c */
#include "ls_test.h"
#include "ls_board_hw.h"
#include "ls_xl9535.h"
#include "ls_board.h"
#include "freertos/task.h"

static bool levels[16], outputs[16], ready;
static unsigned low_delay, high_delay;
static esp_err_t init_result;

LS_CASE(a_capability_reached_through_the_expander_still_counts)
{
    /* Touch: the panel answers at 0x5D and its reset is XL9535 IO3. */
    LS_EQ_INT(1, LS_HAS_TOUCH);
    /* The C6 enable is XL9535 IO14, and the direct pin is explicitly -1 -
       so this is only true if the expander route is being counted. */
    LS_EQ_INT(-1, LS_BOARD_C6_EN_GPIO);
    LS_EQ_INT(1, LS_HAS_C6);

    LS_EQ_INT(1, LS_HAS_RF_SWITCH);
}

LS_CASE(codec_sda_must_not_be_claimed_by_battery_adc)
{
    LS_EQ_INT(LS_BOARD_I2C2_SDA_GPIO,20);
    LS_EQ_INT(LS_HAS_BATTERY_ADC,0);
}

esp_err_t ls_xl9535_init(ls_i2c_bus_id_t bus, uint8_t address)
{
    LS_EQ_INT(bus, LS_I2C_PRIMARY);
    LS_EQ_INT(address, 0x20);
    ready = init_result == ESP_OK;
    return init_result;
}
bool ls_xl9535_ready(void) { return ready; }
esp_err_t ls_xl9535_set(int pin, bool high)
{ levels[pin] = high; return ESP_OK; }
esp_err_t ls_xl9535_set_dir(int pin, bool output)
{ outputs[pin] = output; return ESP_OK; }
esp_err_t ls_xl9535_out(int pin, bool high)
{ levels[pin] = high; outputs[pin] = true; return ESP_OK; }
esp_err_t ls_xl9535_get(int pin, bool *high)
{ *high = levels[pin]; return ESP_OK; }
void vTaskDelay(TickType_t ticks)
{
    if (levels[LS_XL9535_IO3]) high_delay += ticks;
    else low_delay += ticks;
}

LS_CASE(boot_enables_active_low_rails_and_releases_resets)
{
    memset(levels, 1, sizeof(levels));
    memset(outputs, 0, sizeof(outputs));
    init_result = ESP_OK;
    low_delay = high_delay = 0;
    LS_EQ_INT(ls_board_hw_early_init(), ESP_OK);
    LS_CHECK(outputs[0] && !levels[0]); /* 3V3 enable, active low */
    LS_CHECK(outputs[13] && !levels[13]); /* IO15: SD enable */
    LS_CHECK(outputs[6] && levels[6]); /* audio power, active high */
    LS_CHECK(outputs[8] && levels[8]); /* IO10: USB PHY */
    LS_CHECK(!outputs[12] && levels[12]); /* IO14: C6 EN, external pull-up */
    LS_CHECK(outputs[14] && !levels[14]); /* IO16: radio reset */

    LS_CHECK(outputs[1] && levels[1]);
    LS_CHECK(levels[2] && levels[3]);
    LS_CHECK(!outputs[4]);

    LS_CHECK_MSG(!outputs[5],
                 "boot drove IO5 - that is the Ethernet PHY reset");
    LS_CHECK(low_delay >= pdMS_TO_TICKS(30));
    LS_CHECK(high_delay >= pdMS_TO_TICKS(100));
}

LS_CASE(c6_control_uses_bank_one_bit_four)
{
    ready = true;
    levels[14] = false;
    LS_EQ_INT(ls_board_hw_c6_enable(true), ESP_OK);
    LS_EQ_INT(ls_board_hw_c6_state(), 1);
    LS_CHECK(!levels[14]);
    LS_EQ_INT(ls_board_hw_c6_enable(false), ESP_OK);
    LS_EQ_INT(ls_board_hw_c6_state(), 0);
}

LS_CASE(expander_failure_is_returned_before_enabling_rails)
{
    memset(outputs, 0, sizeof(outputs));
    init_result = ESP_ERR_NOT_FOUND;
    LS_EQ_INT(ls_board_hw_early_init(), ESP_ERR_NOT_FOUND);
    for (unsigned i = 0; i < 16; ++i) LS_CHECK(!outputs[i]);
    init_result = ESP_OK;
}

LS_CASE(c6_release_leaves_reset_button_in_control)
{
    ready = true;
    memset(outputs, 1, sizeof(outputs));
    levels[12] = false;
    LS_EQ_INT(ls_board_hw_c6_release(), ESP_OK);
    LS_CHECK(levels[12]);
    LS_CHECK(!outputs[12]);
    for (unsigned i = 0; i < 16; ++i)
        if (i != 12) LS_CHECK(outputs[i]);
    LS_EQ_INT(ls_board_hw_c6_enable(false), ESP_OK);
    LS_CHECK(outputs[12] && !levels[12]);
    ready = false;
    LS_EQ_INT(ls_board_hw_c6_release(), ESP_ERR_INVALID_STATE);
    LS_CHECK(outputs[12] && !levels[12]);
}
