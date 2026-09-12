/* LS_TEST_SOURCES: ${FW}/components/lakeshark/board/ls_xl9535.c */

#include "ls_test.h"
#include "ls_xl9535.h"
#include "ls_board.h"
#include "driver/i2c_master.h"

#include <string.h>

/* --------------------------------------------------------- part model --- */

#define R_INPUT0  0x00
#define R_INPUT1  0x01
#define R_OUTPUT0 0x02
#define R_OUTPUT1 0x03
#define R_CONFIG0 0x06
#define R_CONFIG1 0x07

static uint8_t s_reg[8];
static int     s_writes;
static uint8_t s_write_log[64][2];
static int     s_write_n;
static bool    s_bus_ok = true;

static void part_reset(void)
{
    memset(s_reg, 0, sizeof(s_reg));
    /* Reset default: every pin an input, which is what the datasheet says and
       what makes "make this an output" a real state change. */
    s_reg[R_CONFIG0] = 0xFF;
    s_reg[R_CONFIG1] = 0xFF;
    s_writes = 0;
    s_write_n = 0;
    s_bus_ok = true;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *buf,
                              size_t len, int timeout)
{
    (void)dev; (void)timeout;
    if (!s_bus_ok) return ESP_FAIL;
    if (len != 2) return ESP_ERR_INVALID_ARG;
    if (buf[0] < 8) s_reg[buf[0]] = buf[1];
    if (s_write_n < 64) {
        s_write_log[s_write_n][0] = buf[0];
        s_write_log[s_write_n][1] = buf[1];
        s_write_n++;
    }
    s_writes++;
    return ESP_OK;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,
                                      const uint8_t *tx, size_t tx_len,
                                      uint8_t *rx, size_t rx_len, int timeout)
{
    (void)dev; (void)timeout;
    if (!s_bus_ok) return ESP_FAIL;
    if (tx_len != 1 || rx_len != 1) return ESP_ERR_INVALID_ARG;
    *rx = tx[0] < 8 ? s_reg[tx[0]] : 0;
    return ESP_OK;
}

/* The driver asks ls_i2c for a device handle; any non-null one will do. */
static struct { int x; } s_fake_dev;

esp_err_t ls_i2c_probe(ls_i2c_bus_id_t id, uint8_t addr, int ms)
{
    (void)id; (void)addr; (void)ms;
    return s_bus_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t ls_i2c_device(ls_i2c_bus_id_t id, uint8_t addr, uint32_t hz,
                        i2c_master_dev_handle_t *out)
{
    (void)id; (void)addr; (void)hz;
    if (!s_bus_ok) return ESP_FAIL;
    if (out) *out = (i2c_master_dev_handle_t)&s_fake_dev;
    return ESP_OK;
}

static void ensure_up(void)
{
    static bool once;
    if (!once) {
        part_reset();
        LS_CHECK(ls_xl9535_init(LS_I2C_PRIMARY, 0x20) == ESP_OK);
        once = true;
    }
    LS_CHECK(ls_xl9535_ready());
    s_bus_ok = true;
    s_writes = 0;
    s_write_n = 0;
}

/* Snapshot the four registers the driver touches. */
typedef struct { uint8_t out0, out1, cfg0, cfg1; } regs_t;
static regs_t snapshot(void)
{
    regs_t r = { s_reg[R_OUTPUT0], s_reg[R_OUTPUT1],
                 s_reg[R_CONFIG0], s_reg[R_CONFIG1] };
    return r;
}

/* Which register and mask a pin should land on, derived independently of the
   driver so the two have to agree. */
static uint8_t expect_mask(int pin) { return (uint8_t)(1u << (pin & 7)); }
static uint8_t expect_out_reg(int pin) { return pin < 8 ? R_OUTPUT0 : R_OUTPUT1; }
static uint8_t expect_cfg_reg(int pin) { return pin < 8 ? R_CONFIG0 : R_CONFIG1; }

/* ---------------------------------------------------------------- cases -- */

LS_CASE(the_second_bank_starts_at_bit_eight_not_bit_ten)
{
    /* The whole trap in one case. IO10 is the ninth pin, so it is bit 0 of
       the second register - not bit 2, and not bit 10 of anything. */
    LS_EQ_INT(0,  LS_XL9535_IO0);
    LS_EQ_INT(7,  LS_XL9535_IO7);
    LS_EQ_INT(8,  LS_XL9535_IO10);
    LS_EQ_INT(15, LS_XL9535_IO17);
    LS_EQ_INT(16, LS_XL9535_PIN_COUNT);
}

LS_CASE(every_pin_writes_its_own_bit_in_its_own_register)
{
    /* Sweep all sixteen. A pin that lands in the wrong register or on the
       wrong bit resets a neighbour, and on this board the neighbours are the
       LoRa radio and the Ethernet PHY. */
    ensure_up();
    for (int pin = 0; pin < LS_XL9535_PIN_COUNT; pin++) {
        uint8_t reg = expect_out_reg(pin), mask = expect_mask(pin);
        uint8_t other = (reg == R_OUTPUT0) ? R_OUTPUT1 : R_OUTPUT0;

        /* Drive it low then high, so the bit definitely changes whatever it
           held before and the change is attributable to this call. */
        ls_xl9535_set(pin, false);
        regs_t before = snapshot();
        LS_EQ_INT(ESP_OK, ls_xl9535_set(pin, true));

        LS_CHECK_MSG((s_reg[reg] & mask) != 0,
                     "pin %d did not set bit 0x%02X of reg 0x%02X",
                     pin, mask, reg);
        LS_CHECK_MSG(s_reg[other] == (reg == R_OUTPUT0 ? before.out1 : before.out0),
                     "pin %d disturbed reg 0x%02X", pin, other);

        uint8_t was = (reg == R_OUTPUT0) ? before.out0 : before.out1;
        LS_CHECK_MSG((s_reg[reg] & (uint8_t)~mask) == (was & (uint8_t)~mask),
                     "pin %d disturbed other bits of reg 0x%02X", pin, reg);
    }
}

LS_CASE(clearing_a_pin_leaves_its_neighbours_set)
{
    /* Read-modify-write. A driver that writes the whole register instead of
       one bit turns everything else off, which on this board is the 3V3 rail
       and the panel reset. */
    ensure_up();
    for (int pin = 0; pin < 8; pin++) ls_xl9535_set(pin, true);
    LS_EQ_INT(0xFF, s_reg[R_OUTPUT0]);

    ls_xl9535_set(3, false);
    LS_EQ_INT(0xF7, s_reg[R_OUTPUT0]);

    /* Put it back so later cases start from a known bank. */
    ls_xl9535_set(3, true);
}

LS_CASE(making_a_pin_an_output_clears_its_config_bit_only)
{

    ensure_up();
    for (int pin = 0; pin < LS_XL9535_PIN_COUNT; pin++) {
        uint8_t reg = expect_cfg_reg(pin), mask = expect_mask(pin);

        ls_xl9535_set_dir(pin, false);          /* back to an input */
        regs_t before = snapshot();
        LS_EQ_INT(ESP_OK, ls_xl9535_set_dir(pin, true));

        LS_CHECK_MSG((s_reg[reg] & mask) == 0,
                     "pin %d did not clear config bit 0x%02X", pin, mask);
        uint8_t was = (reg == R_CONFIG0) ? before.cfg0 : before.cfg1;
        LS_CHECK_MSG((s_reg[reg] & (uint8_t)~mask) == (was & (uint8_t)~mask),
                     "pin %d disturbed other config bits", pin);
    }
}

LS_CASE(a_rail_is_driven_to_its_level_before_it_is_enabled)
{

    ensure_up();
    /* Leave the pin low and an input so both writes are real changes. */
    ls_xl9535_set(LS_XL9535_IO16, false);
    ls_xl9535_set_dir(LS_XL9535_IO16, false);
    s_write_n = 0;
    LS_EQ_INT(ESP_OK, ls_xl9535_out(LS_XL9535_IO16, true));

    LS_CHECK_MSG(s_write_n >= 2, "only %d writes for an out()", s_write_n);
    LS_CHECK_MSG(s_write_log[0][0] == R_OUTPUT1,
                 "the first write was to reg 0x%02X, not the output register",
                 s_write_log[0][0]);
    LS_CHECK_MSG(s_write_log[1][0] == R_CONFIG1,
                 "the second write was to reg 0x%02X, not the config register",
                 s_write_log[1][0]);
}

LS_CASE(writing_a_value_a_pin_already_has_touches_no_bus)
{
    /* The expander sits on the shared I2C bus with the touch controller and
       the RTC, and it has NACKed under load before. A driver that rewrites
       an unchanged value doubles its traffic for nothing. */
    ensure_up();
    ls_xl9535_set(LS_XL9535_IO2, false);
    s_writes = 0;
    ls_xl9535_set(LS_XL9535_IO2, true);
    int after_first = s_writes;

    ls_xl9535_set(LS_XL9535_IO2, true);
    LS_EQ_INT(after_first, s_writes);

    ls_xl9535_set(LS_XL9535_IO2, false);
    LS_CHECK(s_writes > after_first);
}

LS_CASE(the_named_board_pins_land_where_the_schematic_says)
{

    static const struct { int pin; uint8_t reg, mask; const char *what; } MAP[] = {
        { LS_BOARD_XL_PWR_3V3_EN,   R_OUTPUT0, 0x01, "3V3 enable"      },
        { LS_BOARD_XL_RF_SW_VCTL,   R_OUTPUT0, 0x02, "RF switch"       },
        { LS_BOARD_XL_SCREEN_RST,   R_OUTPUT0, 0x04, "panel reset"     },
        { LS_BOARD_XL_TOUCH_RST,    R_OUTPUT0, 0x08, "touch reset"     },
        { LS_BOARD_XL_ETH_PHY_RST,  R_OUTPUT0, 0x20, "ethernet reset"  },
        { LS_BOARD_XL_AUDIO_PWR_EN, R_OUTPUT0, 0x40, "audio power"     },
        { LS_BOARD_XL_USB_PHY_EN,   R_OUTPUT1, 0x01, "usb phy power"   },
        { LS_BOARD_XL_GPS_WAKE,     R_OUTPUT1, 0x02, "gps wake"        },
        { LS_BOARD_XL_C6_EN,        R_OUTPUT1, 0x10, "c6 enable"       },
        { LS_BOARD_XL_SD_PWR_EN,    R_OUTPUT1, 0x20, "sd power"        },
        { LS_BOARD_XL_RADIO_RST,    R_OUTPUT1, 0x40, "lora reset"      },
    };

    ensure_up();
    for (unsigned i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        ls_xl9535_set(MAP[i].pin, false);
        regs_t before = snapshot();
        LS_EQ_INT(ESP_OK, ls_xl9535_set(MAP[i].pin, true));

        LS_CHECK_MSG((s_reg[MAP[i].reg] & MAP[i].mask) != 0,
                     "%s (pin %d) did not reach reg 0x%02X mask 0x%02X",
                     MAP[i].what, MAP[i].pin, MAP[i].reg, MAP[i].mask);
        uint8_t other = (MAP[i].reg == R_OUTPUT0) ? R_OUTPUT1 : R_OUTPUT0;
        uint8_t other_was = (other == R_OUTPUT0) ? before.out0 : before.out1;
        LS_CHECK_MSG(s_reg[other] == other_was,
                     "%s reached the wrong register too", MAP[i].what);
    }
}

LS_CASE(the_lora_reset_and_the_phy_reset_are_not_the_same_bit)
{
    /* Stated on its own because it is the confusion with real consequences:
       aiming at the radio and hitting the PHY drops the network, and aiming
       at the PHY and hitting the radio makes LoRa look broken. */
    ensure_up();
    LS_CHECK_MSG(expect_out_reg(LS_BOARD_XL_RADIO_RST) !=
                 expect_out_reg(LS_BOARD_XL_ETH_PHY_RST) ||
                 expect_mask(LS_BOARD_XL_RADIO_RST) !=
                 expect_mask(LS_BOARD_XL_ETH_PHY_RST),
                 "the LoRa reset and the PHY reset are the same bit");

    /* And prove it by driving one and watching the other stay put. */
    ls_xl9535_set(LS_BOARD_XL_ETH_PHY_RST, false);
    ls_xl9535_set(LS_BOARD_XL_RADIO_RST, false);
    ls_xl9535_set(LS_BOARD_XL_RADIO_RST, true);
    uint8_t phy_reg = expect_out_reg(LS_BOARD_XL_ETH_PHY_RST);
    LS_CHECK_MSG((s_reg[phy_reg] & expect_mask(LS_BOARD_XL_ETH_PHY_RST)) == 0,
                 "resetting the radio also asserted the PHY reset");
}

LS_CASE(reading_a_pin_takes_it_from_the_right_input_register)
{
    ensure_up();
    s_reg[R_INPUT0] = 0x10;      /* pin 4 high  */
    s_reg[R_INPUT1] = 0x08;      /* pin 11 high */

    bool v = false;
    LS_EQ_INT(ESP_OK, ls_xl9535_get(4, &v));
    LS_CHECK_MSG(v, "pin 4 read low from an input register holding 0x10");

    v = false;
    LS_EQ_INT(ESP_OK, ls_xl9535_get(11, &v));
    LS_CHECK_MSG(v, "pin 11 read low from an input register holding 0x08");

    v = true;
    LS_EQ_INT(ESP_OK, ls_xl9535_get(5, &v));
    LS_CHECK_MSG(!v, "pin 5 read high from a register with that bit clear");
}

LS_CASE(a_pin_outside_the_part_is_refused)
{
    ensure_up();
    bool v;
    LS_CHECK(ls_xl9535_set(-1, true) != ESP_OK);
    LS_CHECK(ls_xl9535_set(LS_XL9535_PIN_COUNT, true) != ESP_OK);
    LS_CHECK(ls_xl9535_set(999, true) != ESP_OK);
    LS_CHECK(ls_xl9535_set_dir(-1, true) != ESP_OK);
    LS_CHECK(ls_xl9535_get(LS_XL9535_PIN_COUNT, &v) != ESP_OK);
    LS_CHECK(ls_xl9535_get(0, NULL) != ESP_OK);
    /* Nothing reached the bus. */
    LS_EQ_INT(0, s_writes);
}

LS_CASE(a_bus_failure_leaves_the_cache_unchanged)
{
    /* If a write fails the driver must not record the value as written, or
       the next attempt at the same value is skipped as redundant and the pin
       stays where it was - a rail that reports enabled and is not. */
    ensure_up();
    ls_xl9535_set(LS_XL9535_IO4, false);
    s_bus_ok = false;
    LS_CHECK(ls_xl9535_set(LS_XL9535_IO4, true) != ESP_OK);

    s_bus_ok = true;
    LS_EQ_INT(ESP_OK, ls_xl9535_set(LS_XL9535_IO4, true));
    LS_CHECK_MSG((s_reg[R_OUTPUT0] & 0x10) != 0,
                 "the retry after a failed write was skipped as redundant");
}
