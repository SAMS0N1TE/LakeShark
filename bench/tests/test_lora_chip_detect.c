/* LS_TEST_SOURCES: ls_lora.c, ls_lora_sx126x.c, ls_lora_lr20xx.c and ls_spi.c: which part answered, and which backend that binds */

#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"

static void up(fk_kind_t kind)
{
    /* Start something once so the SPI device exists, then install the part
       under test. A first start against nothing may fail; that is fine. */
    fk_install(FK_LR);
    (void)ls_lora_start();
    fk_install(kind);
}

LS_CASE(an_lr2021_answers_and_binds_the_lr20xx_backend)
{
    up(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_CHECK(ls_lora_present());    /* bound, and a LoRa radio */
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    LS_EQ_STR(ls_lora_chip_name(), "LR2021");
    LS_CHECK(ls_lora_caps() & LS_LORA_CAP_RSSI_INST);
    LS_EQ_UINT(ls_lora_caps() & (LS_LORA_CAP_LORA | LS_LORA_CAP_FSK),
               LS_LORA_CAP_LORA | LS_LORA_CAP_FSK);
}

LS_CASE(an_sx1262_that_ignores_unknown_opcodes_binds_the_sx126x_backend)
{
    up(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_CHECK(ls_lora_present());
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_SX126X);
    LS_EQ_STR(ls_lora_chip_name(), "SX126x");
    LS_EQ_UINT(ls_lora_caps(), LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_RSSI_INST);

    /* The LR20xx probe went out first: GetStatus, then both frames of
       GetVersion, then the SX126x standby and status. */
    FK_FRAME(0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    FK_FRAME(1, 0x01, 0x01);
    FK_FRAME(2, 0x00, 0x00, 0x00, 0x00);
    FK_FRAME(3, 0x80, 0x00);
    FK_FRAME(4, 0xC0, 0x00);

    uint8_t st = 0;
    LS_EQ_INT(ls_lora_status(&st), ESP_OK);
    LS_EQ_UINT(st, 0x22);
    uint8_t reg[2];
    LS_EQ_INT(ls_lora_read_reg(0x0740, reg, sizeof(reg)), ESP_OK);
    LS_EQ_UINT(reg[0], 0x14);

    /* The Mode S session is an LR20xx thing: not advertised, not callable, and
       asking sends the SX126x nothing. */
    const int frames = fk.nframes;
    LS_CHECK(!(ls_lora_caps() & LS_LORA_CAP_MODES_RX));
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_ERR_NOT_SUPPORTED);
    uint8_t buf[28];
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), NULL), -1);
    LS_EQ_INT(ls_lora_modes_set_gain(5), ESP_ERR_NOT_SUPPORTED);
    LS_CHECK(!ls_lora_modes_active());
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    LS_EQ_INT(fk.nframes, frames);
}

LS_CASE(an_sx1262_that_puts_its_status_in_every_byte_is_not_mistaken_for_an_lr20xx)
{
    up(FK_SX_STATUS);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_SX126X);
}

LS_CASE(a_status_word_of_all_zeros_is_not_a_part)
{
    up(FK_ALL_00);
    LS_EQ_INT(ls_lora_start(), ESP_ERR_NOT_FOUND);
    LS_CHECK(!ls_lora_present());
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_NONE);
    LS_EQ_UINT(ls_lora_caps(), 0);
}

LS_CASE(a_status_word_of_all_ones_is_not_a_part)
{
    up(FK_ALL_FF);
    LS_EQ_INT(ls_lora_start(), ESP_ERR_NOT_FOUND);
    LS_CHECK(!ls_lora_present());
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_NONE);
}

LS_CASE(an_lr_like_part_that_fails_get_version_is_never_reported_as_an_sx1262)
{
    /* The misidentification this exists to prevent: opcodes 0x80 and 0xC0 are
       unknown to an LR20xx, which still clocks out its 16-bit Stat, and the
       old probe took the low byte of that (0x21) as an SX1262 status. */
    up(FK_LR);
    fk.fw_major = 0x05;               /* answers like an LR20xx but is not one */
    LS_EQ_INT(ls_lora_start(), ESP_ERR_NOT_FOUND);
    LS_CHECK(!ls_lora_present());
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_NONE);
    LS_EQ_UINT(ls_lora_caps(), 0);
}

LS_CASE(an_lr2021_whose_reset_failed_still_identifies_as_an_lr2021)
{
    /* The reset pulse went nowhere and the part is still receiving from last
       time. Structure and version identify it; the mode does not matter. */
    up(FK_LR);
    fk.mode = LR20XX_MODE_RX;
    fk.reset_src = 0;
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
}

LS_CASE(an_sx126x_status_with_its_reserved_bits_set_is_not_accepted)
{
    up(FK_SX_STATUS);
    fk.sx_status = 0x23;              /* bit 0 is reserved and reads zero */
    LS_EQ_INT(ls_lora_start(), ESP_ERR_NOT_FOUND);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_NONE);
    fk_install(FK_SX_STATUS);
    fk.sx_status = 0xA2;              /* so is bit 7 */
    LS_EQ_INT(ls_lora_start(), ESP_ERR_NOT_FOUND);
    fk_install(FK_SX_STATUS);
    fk.sx_status = 0x5A;              /* the statuses the part really reports */
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_SX126X);
}

LS_CASE(a_module_swapped_between_starts_is_found_afresh_in_both_directions)
{
    up(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);

    fk_install(FK_SX_ECHO);               /* includes ls_lora_stop() */
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_SX126X);
    uint8_t st = 0;
    LS_EQ_INT(ls_lora_status(&st), ESP_OK);
    LS_EQ_UINT(st, 0x22);

    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    uint8_t reg[2];
    LS_EQ_INT(ls_lora_read_reg(0x0740, reg, sizeof(reg)), ESP_ERR_NOT_SUPPORTED);
}

LS_CASE(an_lr20xx_never_gets_an_sx126x_configure_even_if_the_caller_starts_nothing)
{
    /* configure() brings the part up itself when nobody did. It must bind the
       right backend before sending anything. */
    up(FK_LR);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    for (int i = 0; i < fk.nframes; i++) {
        if (fk.frames[i].read2) continue;
        const uint8_t hi = fk.frames[i].b[0];
        LS_CHECK_MSG(hi == 0x00 || hi == 0x01 || hi == 0x02,
                     "frame %d starts with 0x%02X, not an LR20xx opcode", i, hi);
    }
}

LS_CASE(an_sx1262_still_configures_and_receives_after_the_probe_change)
{
    up(FK_SX_ECHO);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_CHECK(ls_lora_cfg() != NULL);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_CHECK(ls_lora_is_receiving());
    LS_CHECK(ls_lora_airtime_ms(10) > 0);
}
