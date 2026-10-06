/* LS_TEST_SOURCES: ls_lora.c, ls_lora_sx126x.c, ls_lora_lr20xx.c and ls_spi.c against an LR2021 SPI fake */

/* Exact frame bytes for the LR20xx command layer, against a fake that follows
   the datasheet's framing. This proves the host side of the protocol; it says
   nothing about RF or about the silicon. */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "lora_fakes.h"
#include "ls_board.h"
#include "ls_xl9535.h"
#include "ls_lora_lr20xx.h"
#include "ls_lora_priv.h"
#include "ls_test.h"
#include "p25_state.h"

/* The one place the LR20xx opcodes the driver is allowed to send are listed. A
   frame outside this set means something sent an SX126x opcode, or a command
   nobody reviewed, to an LR20xx. */
static bool known_lr_opcode(uint16_t op)
{
    static const uint16_t ok[] = {
        0x0100, 0x0101, 0x0110, 0x0111, 0x0116, 0x0117, 0x0123, 0x0128,
        0x0200, 0x0201, 0x020B, 0x020C,
        /* the Mode S receive session */
        0x0001, 0x0105, 0x0106, 0x0112, 0x0113, 0x0115, 0x011C, 0x011E, 0x0122, 0x0206, 0x0207,
        0x021A, 0x0281, 0x0282, 0x0284, 0x0287, 0x0288,
        /* LoRa and GFSK packets, and the DC-DC workaround */
        0x0104, 0x0212, 0x0220, 0x0221, 0x0223, 0x022A, 0x0240, 0x0241, 0x0244, 0x0246, 0x0247,
        /* Receive-only LoRa sides, Wi-SUN and Z-Wave. */
        0x0224, 0x0225, 0x0270, 0x0271, 0x0272, 0x0273,
        0x0297, 0x0298, 0x0299, 0x029A, 0x029C, 0x029D,
        /* the LF transmitter: Tx FIFO, SelPa, SetPaConfig, SetTxParams, SetTx */
        0x0002, 0x011F, 0x020F, 0x0202, 0x0203, 0x020D,
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) if (ok[i] == op) return true;
    return false;
}

static int unknown_frames(void)
{
    int bad = 0;
    for (int i = 0; i < fk.nframes; i++) {
        if (fk.frames[i].read2) continue;
        const uint16_t op = (uint16_t)((fk.frames[i].b[0] << 8) | fk.frames[i].b[1]);
        if (!known_lr_opcode(op)) bad++;
    }
    return bad;
}

static void lr_up(void)
{
    /* Bring a first start through so the SPI device exists, then start clean. */
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk_install(FK_LR);
}

/* ------------------------------------------------------------------ cases -- */

LS_CASE(an_lr2021_is_identified_with_exactly_these_frames)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_CHECK(ls_lora_present());    /* a LoRa radio now, mesh and all */
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    LS_EQ_STR(ls_lora_chip_name(), "LR2021");

    FK_FRAME(0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);          /* GetStatus      */
    FK_FRAME(1, 0x01, 0x01);                                  /* GetVersion, 1  */
    FK_FRAME(2, 0x00, 0x00, 0x00, 0x00);                      /* GetVersion, 2  */
    LS_CHECK(fk.frames[2].read2);
    FK_FRAME(3, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);          /* ClearIrq, all  */
    LS_EQ_INT(fk.nframes, 4);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(the_probe_reads_the_stat_and_the_version_without_changing_the_chip)
{
    lr_up();
    lr20xx_info_t info;
    LS_EQ_INT(lr20xx_probe(&info), ESP_OK);
    LS_EQ_UINT(info.stat, 0x0421);        /* cmd OK, reset by NRESET, STDBY_RC */
    LS_EQ_UINT(info.fw_major, 1);
    LS_EQ_UINT(info.fw_minor, 0x18);
    LS_CHECK(info.lr2021);
    LS_EQ_INT(fk.nframes, 3);
    LS_EQ_INT(fk.violations, 0);

    /* GetStatus cleared the reset source, so a second read says so. */
    lr20xx_status_t st;
    LS_EQ_INT(lr20xx_get_status(&st), ESP_OK);
    LS_EQ_UINT(st.stat & 0xF0, 0x00);
    LS_EQ_UINT(st.stat & 7, LR20XX_MODE_STBY_RC);
}

LS_CASE(an_lr2012_or_lr2022_binds_without_claiming_the_hf_input)
{
    fk_install(FK_LR);
    fk.fw_major = 2; fk.fw_minor = 0;
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    LS_EQ_STR(ls_lora_chip_name(), "LR2012/LR2022");
    LS_CHECK(!(ls_lora_caps() & LS_LORA_CAP_BAND_1G5_2G5));
    LS_CHECK(ls_lora_caps() & LS_LORA_CAP_RSSI_INST);
    LS_CHECK(ls_lora_caps() & LS_LORA_CAP_MODES_RX);   /* 1090 MHz is on the LF input, which all of them have */
}

LS_CASE(capabilities_say_what_is_implemented_not_what_the_silicon_could_do)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    const uint32_t c = ls_lora_caps();
    LS_CHECK(c & LS_LORA_CAP_RSSI_INST);
    LS_CHECK(c & LS_LORA_CAP_BAND_1G5_2G5);
    LS_CHECK(c & LS_LORA_CAP_LORA);
    LS_CHECK(c & LS_LORA_CAP_FSK);
    LS_CHECK(!(c & LS_LORA_CAP_OOK_RX));
    LS_CHECK(c & LS_LORA_CAP_MODES_RX);      /* the receive-only Mode S session exists */
}

LS_CASE(set_rf_frequency_is_a_32_bit_value_in_hertz)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_set_rf_frequency(1090000000u), ESP_OK);
    FK_FRAME(0, 0x02, 0x00, 0x40, 0xF8, 0x14, 0x80);
    LS_EQ_UINT(fk.last_freq_hz, 1090000000u);
    LS_EQ_INT(lr20xx_set_rf_frequency(868100000u), ESP_OK);
    FK_FRAME(1, 0x02, 0x00, 0x33, 0xBE, 0x27, 0xA0);

    /* Outside the 150-2500 MHz synthesiser nothing is sent. */
    LS_EQ_INT(lr20xx_set_rf_frequency(149999999u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_set_rf_frequency(2500000001u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 2);
}

LS_CASE(set_rx_path_sends_the_path_then_the_boost)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_set_rx_path(LR20XX_PATH_LF, 7), ESP_OK);
    FK_FRAME(0, 0x02, 0x01, 0x00, 0x07);
    LS_EQ_INT(lr20xx_set_rx_path(LR20XX_PATH_HF, 4), ESP_OK);
    FK_FRAME(1, 0x02, 0x01, 0x01, 0x04);
    LS_EQ_INT(lr20xx_set_rx_path(LR20XX_PATH_LF, 8), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_set_rx_path((lr20xx_rx_path_t)2, 0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 2);
    LS_EQ_UINT(fk.last_path, 1);
    LS_EQ_UINT(fk.last_boost, 4);
}

LS_CASE(calib_fe_takes_four_mhz_steps_with_the_path_in_bit_15)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_calib_fe_hz(1090000000u, LR20XX_PATH_LF), ESP_OK);
    FK_FRAME(0, 0x01, 0x23, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00);
    /* The datasheet's own examples: 900 MHz is 0x00E1 on LF, 0x80E1 on HF. */
    LS_EQ_INT(lr20xx_calib_fe_hz(900000000u, LR20XX_PATH_LF), ESP_OK);
    FK_FRAME(1, 0x01, 0x23, 0x00, 0xE1, 0x00, 0x00, 0x00, 0x00);
    LS_EQ_INT(lr20xx_calib_fe_hz(900000000u, LR20XX_PATH_HF), ESP_OK);
    FK_FRAME(2, 0x01, 0x23, 0x80, 0xE1, 0x00, 0x00, 0x00, 0x00);
    const uint16_t three[3] = { 0x00E1, 0x8110, 0 };
    LS_EQ_INT(lr20xx_calib_fe(three), ESP_OK);
    FK_FRAME(3, 0x01, 0x23, 0x00, 0xE1, 0x81, 0x10, 0x00, 0x00);
    LS_EQ_INT(lr20xx_calib_fe_hz(1000000u, LR20XX_PATH_LF), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 4);
}

LS_CASE(standby_selects_rc_or_xosc)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    FK_FRAME(0, 0x01, 0x28, 0x00);
    LS_EQ_INT(lr20xx_set_standby(true), ESP_OK);
    FK_FRAME(1, 0x01, 0x28, 0x01);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_XOSC);
}

LS_CASE(tuning_for_receive_calibrates_then_tunes_then_picks_the_path_and_checks_each_step)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    /* 1090 MHz is below the 1.5 GHz hand-over, so it goes on the LF input. */
    LS_EQ_INT(lr20xx_tune_rx(1090000000u, -1), ESP_OK);
    FK_FRAME(0, 0x01, 0x23, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00);
    FK_FRAME(1, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    FK_FRAME(2, 0x02, 0x00, 0x40, 0xF8, 0x14, 0x80);
    FK_FRAME(3, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    FK_FRAME(4, 0x02, 0x01, 0x00, 0x00);
    FK_FRAME(5, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    LS_EQ_INT(fk.nframes, 6);

    fk.nframes = 0;
    LS_EQ_INT(lr20xx_tune_rx(2440000000u, -1), ESP_OK);
    FK_FRAME(0, 0x01, 0x23, 0x82, 0x62, 0x00, 0x00, 0x00, 0x00);
    FK_FRAME(4, 0x02, 0x01, 0x01, 0x04);                    /* HF, default boost 4 */

    fk.nframes = 0;
    LS_EQ_INT(lr20xx_tune_rx(433920000u, 7), ESP_OK);
    FK_FRAME(4, 0x02, 0x01, 0x00, 0x07);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(a_command_the_chip_rejects_is_reported_by_the_next_status)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    /* CalibFE is refused in Rx (datasheet 6.4.2) and says so on the next frame. */
    LS_EQ_INT(lr20xx_set_rx(0xFFFFFFu), ESP_OK);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_tune_rx(868000000u, -1), ESP_ERR_INVALID_RESPONSE);
    LS_EQ_INT(fk.nframes, 2);                   /* stopped at the first failure */
}

LS_CASE(a_read_waits_for_busy_between_its_two_frames)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    uint8_t major = 0, minor = 0;
    LS_EQ_INT(lr20xx_get_version(&major, &minor), ESP_OK);
    LS_EQ_UINT(major, 1); LS_EQ_UINT(minor, 0x18);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(fk.nframes, 2);
    LS_CHECK(fk.frames[1].read2);
}

LS_CASE(the_fake_catches_a_second_frame_clocked_while_busy_is_high)
{
    /* The harness checking itself: if a driver skipped the wait, this is what
       would be seen, and the data it read would be nothing. */
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.read_pending = true; fk.read_op = 0x0101;
    fk_busy_for = 5;
    uint8_t buf[4] = { 0, 0, 0, 0 };
    spi_transaction_t t = { .length = 32, .tx_buffer = buf, .rx_buffer = buf };
    LS_EQ_INT(spi_device_transmit(ls_lora_hw_dev(), &t), ESP_OK);
    LS_EQ_INT(fk.violations, 1);
    LS_EQ_UINT(buf[2], 0);
    fk_busy_for = 0;
    fk.read_pending = false;
}

LS_CASE(busy_stuck_after_the_first_frame_of_a_read_is_a_timeout_with_no_second_frame)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    fk.stuck_after_frame1 = 0x0101;
    uint8_t major = 0, minor = 0;
    LS_EQ_INT(lr20xx_get_version(&major, &minor), ESP_ERR_TIMEOUT);
    LS_EQ_INT(fk.nframes, 1);
    LS_EQ_INT(fk.violations, 0);
    fk_busy_for = 0;
    fk.read_pending = false;
    fk.stuck_after_frame1 = 0;
}

LS_CASE(instantaneous_rssi_is_minus_half_of_the_nine_bit_value)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    float dbm = 42;

    /* Not receiving: the chip is asked, and says standby. */
    LS_EQ_INT(ls_lora_rssi_inst(&dbm), ESP_ERR_INVALID_STATE);
    LS_NEAR(dbm, 42, 0.01);

    LS_EQ_INT(lr20xx_set_rx(0xFFFFFFu), ESP_OK);
    FK_FRAME(fk.nframes - 1, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    fk.nframes = 0;
    fk.rssi_hi = 100; fk.rssi_lo = 0x80;        /* 201 -> -100.5 dBm */
    LS_EQ_INT(ls_lora_rssi_inst(&dbm), ESP_OK);
    LS_NEAR(dbm, -100.5, 0.001);
    /* One read, no GetStatus first: its own Stat says the part was in Rx. */
    FK_FRAME(0, 0x02, 0x0B);
    FK_FRAME(1, 0x00, 0x00, 0x00, 0x00);
    LS_EQ_INT(fk.nframes, 2);
    fk.rssi_hi = 100; fk.rssi_lo = 0x00;
    LS_EQ_INT(ls_lora_rssi_inst(&dbm), ESP_OK);
    LS_NEAR(dbm, -100.0, 0.001);

    /* A read that comes back as nothing is not a measurement. */
    dbm = 42; fk.bad_read_stat = true;
    LS_EQ_INT(ls_lora_rssi_inst(&dbm), ESP_ERR_INVALID_RESPONSE);
    LS_NEAR(dbm, 42, 0.001);
    fk.bad_read_stat = false;
    LS_NEAR(lr20xx_rssi_dbm(0, 0), 0.0, 0.001);
    LS_NEAR(lr20xx_rssi_dbm(0xFF, 0x80), -255.5, 0.001);
}

LS_CASE(the_interrupt_is_polled_from_the_status_word_without_the_expander)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    const int gets = fk_xl_gets;
    bool pending = true; uint32_t irq = 0xDEAD;
    LS_EQ_INT(lr20xx_irq_pending(&pending, &irq), ESP_OK);
    LS_CHECK(!pending); LS_EQ_UINT(irq, 0);

    fk.irq = LR20XX_IRQ_RX_DONE | LR20XX_IRQ_PREAMBLE_DETECTED;
    LS_EQ_INT(lr20xx_irq_pending(&pending, &irq), ESP_OK);
    LS_CHECK(pending);
    LS_EQ_UINT(irq, LR20XX_IRQ_RX_DONE | LR20XX_IRQ_PREAMBLE_DETECTED);

    uint32_t got = 0;
    LS_EQ_INT(lr20xx_get_and_clear_irq(&got), ESP_OK);
    LS_EQ_UINT(got, LR20XX_IRQ_RX_DONE | LR20XX_IRQ_PREAMBLE_DETECTED);
    LS_EQ_INT(lr20xx_irq_pending(&pending, &irq), ESP_OK);
    LS_CHECK(!pending);
    LS_EQ_INT(fk_xl_gets, gets);                /* DIO1 over I2C was never read */

    fk.irq = LR20XX_IRQ_RX_DONE | LR20XX_IRQ_CRC_ERROR;
    LS_EQ_INT(lr20xx_clear_irq(LR20XX_IRQ_RX_DONE), ESP_OK);
    LS_EQ_UINT(fk.irq, LR20XX_IRQ_CRC_ERROR);
}

LS_CASE(before_a_configure_nothing_is_programmed_and_nothing_goes_on_the_bus)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;

    /* The SX126x semantics: no configuration, no receive, no send, no sweep. */
    LS_CHECK(ls_lora_cfg() == NULL);
    LS_EQ_INT(ls_lora_receive(), ESP_ERR_INVALID_STATE);
    LS_CHECK(!ls_lora_is_receiving());
    const uint8_t msg[4] = { 1, 2, 3, 4 };
    LS_EQ_INT(ls_lora_send(msg, sizeof(msg)), ESP_ERR_INVALID_STATE);
    LS_CHECK(ls_lora_send_done());
    uint8_t rx[8]; float r, s;
    LS_EQ_INT(ls_lora_poll(rx, sizeof(rx), &r, &s), 0);
    LS_EQ_UINT(ls_lora_airtime_ms(10), 0);
    /* Registers are an SX126x thing and stay so. */
    uint8_t reg[2];
    LS_EQ_INT(ls_lora_read_reg(0x0740, reg, sizeof(reg)), ESP_ERR_NOT_SUPPORTED);

    /* No FSK session: everything in it is refused. */
    LS_EQ_INT(ls_lora_fsk_send(msg, sizeof(msg)), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_fsk_receive(), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_fsk_poll(rx, sizeof(rx), &r), -1);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_CHECK(!ls_lora_fsk_active());

    float bins[8]; bool done = true;
    LS_EQ_INT(ls_lora_scan_begin(868000000u, 870000000u), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_scan_sweep(bins, 8), 0);
    LS_EQ_INT(ls_lora_scan_pass(bins, 8, &done), 0);
    LS_CHECK(!done);
    LS_CHECK(!ls_lora_scanning());
    LS_EQ_INT(ls_lora_scan_end(), ESP_OK);

    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(stopping_drops_the_binding_so_a_swapped_module_is_found_afresh)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    ls_lora_stop();
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_NONE);
    LS_CHECK(!ls_lora_present());
    LS_EQ_UINT(ls_lora_caps(), 0);
    LS_EQ_STR(ls_lora_chip_name(), "none");
}

LS_CASE(no_frame_in_a_full_session_is_an_sx126x_opcode_or_unlisted)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    (void)lr20xx_tune_rx(1090000000u, 7);
    (void)lr20xx_set_rx(0xFFFFFF);
    float dbm; (void)ls_lora_rssi_inst(&dbm);
    (void)lr20xx_set_standby(false);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    (void)ls_lora_configure(&cfg);
    (void)ls_lora_receive();
    (void)ls_lora_send((const uint8_t *)"x", 1);
    fk_tx_complete();
    (void)ls_lora_send_done();
    (void)ls_lora_receive();
    (void)ls_lora_scan_begin(868000000u, 870000000u);
    (void)ls_lora_scan_end();
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(fk.hf_pa, 0);
    LS_EQ_INT(fk.tx_hf, 0);
}

LS_CASE(mesh_startup_on_an_lr2021_configures_and_listens)
{
    /* The sequence ls_mesh_start follows: default config, configure, and on
       success receive. */
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;

    LS_CHECK(ls_lora_present());
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_CHECK(ls_lora_cfg() != NULL);              /* radio_ready */
    LS_EQ_UINT(ls_lora_cfg()->bw_hz, 62500);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_UINT(fk.packet_type, 0x00);
    uint8_t buf[16]; float r, s;
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.bad_reg, 0);
    LS_EQ_INT(unknown_frames(), 0);

    /* A caller that starts nothing first is brought up by configure, and
       every frame is an LR20xx one. */
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    LS_EQ_INT(unknown_frames(), 0);
}

/* ================================================================== */
/* The Mode S receive session and the RF switch DIOs                  */
/* ================================================================== */

/* Command frames only: not the GetStatus polls the driver interleaves, and
   not the second frame of a read. */
static int cmds(int *ix, int max)
{
    int n = 0;
    for (int i = 0; i < fk.nframes && n < max; i++) {
        if (fk.frames[i].read2) continue;
        if (fk.frames[i].full == 6 && fk.frames[i].b[0] == 0x01 && fk.frames[i].b[1] == 0x00) continue;
        ix[n++] = i;
    }
    return n;
}

#define CMD(k, ...)                                                           \
    do {                                                                      \
        static const uint8_t e_[] = { __VA_ARGS__ };                          \
        LS_CHECK_MSG(fk_frame_is(ix[(k)], e_, sizeof(e_)),                    \
                     "command %d is not the expected %u bytes", (k),          \
                     (unsigned)sizeof(e_));                                   \
    } while (0)

static void session_up(void)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
    fk_xl_log_n = 0;
}

static void frame_k(int k, uint8_t out[28])
{
    for (int i = 0; i < 28; i++) out[i] = (uint8_t)(k * 16 + i);
}

static bool tx_opcode_seen(void)
{
    for (int i = 0; i < fk.nframes; i++) {
        if (fk.frames[i].read2) continue;
        const uint16_t op = (uint16_t)((fk.frames[i].b[0] << 8) | fk.frames[i].b[1]);
        if (op == 0x020D || op == 0x0002 || op == 0x0203 || op == 0x0202 || op == 0x020F)
            return true;
    }
    return false;
}

LS_CASE(the_rf_switch_and_irq_dios_are_configured_in_standby_and_only_lf_transmit_is_routed)
{
    session_up();
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_configure_dios(), ESP_OK);

    int ix[40];
    const int n = cmds(ix, 40);
    LS_EQ_INT(n, 10);
    /* SetDioFunction (6.8.1): Func in the high nibble, pull in the low one.
       2 = RF switch, 3 = pull auto: 0x23. SetDioRfSwitchConfig (6.8.2): bit 0
       standby, 1 Rx LF, 2 Tx LF, 3 Rx HF, 4 Tx HF. */
    CMD(0, 0x01, 0x12, 0x06, 0x23);
    CMD(1, 0x01, 0x13, 0x06, 0x08);          /* DIO6: high in Rx HF        */
    CMD(2, 0x01, 0x12, 0x07, 0x23);
    CMD(3, 0x01, 0x13, 0x07, 0x00);          /* DIO7: never high           */
    CMD(4, 0x01, 0x12, 0x08, 0x23);
    CMD(5, 0x01, 0x13, 0x08, 0x06);          /* DIO8: high in Rx LF, Tx LF */
    CMD(6, 0x01, 0x12, 0x0A, 0x23);
    CMD(7, 0x01, 0x13, 0x0A, 0x08);          /* DIO10: high in Rx HF       */
    /* The interrupt: function 1 (IRQ), pull none: 0x10, then the mask
       RxDone | Timeout | CrcError | LenError | CmdError | Error = 0x00E70000. */
    CMD(8, 0x01, 0x12, 0x0B, 0x10);
    CMD(9, 0x01, 0x15, 0x0B, 0x00, 0xE7, 0x00, 0x00);

    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_UINT(fk.dio_func[8], 0x23);
    LS_EQ_UINT(fk.dio_func[11], 0x10);
    LS_EQ_INT(fk.irq_dio, 11);
    LS_EQ_UINT(fk.irq_mask, 0x00E70000u);

    /* STBY is all low (bit 0 clear on every DIO), LF Rx and LF Tx are DIO8
       only, HF Rx is DIO6 + DIO10, and no DIO is high in Tx HF: that state
       routes nothing to an antenna. */
    for (int d = 5; d <= 11; d++) {
        if (!fk.dio_rfsw_set[d]) continue;
        LS_CHECK(!(fk.dio_rfsw[d] & LR20XX_RFSW_STANDBY));
        LS_CHECK(!(fk.dio_rfsw[d] & LR20XX_RFSW_TX_HF));
        if (d != 8) LS_CHECK(!(fk.dio_rfsw[d] & LR20XX_RFSW_TX_LF));
    }
    LS_CHECK(fk.dio_rfsw_set[6] && fk.dio_rfsw_set[7] && fk.dio_rfsw_set[8] && fk.dio_rfsw_set[10]);
    LS_CHECK(!fk.dio_rfsw_set[5] && !fk.dio_rfsw_set[9] && !fk.dio_rfsw_set[11]);
    LS_EQ_UINT(fk.dio_rfsw[8], LR20XX_RFSW_RX_LF | LR20XX_RFSW_TX_LF);
    LS_EQ_UINT(fk.dio_rfsw[6], LR20XX_RFSW_RX_HF);
    LS_EQ_UINT(fk.dio_rfsw[10], LR20XX_RFSW_RX_HF);
    LS_EQ_UINT(fk.dio_rfsw[7], 0);
}

LS_CASE(the_dio_helpers_refuse_what_the_datasheet_does_not_allow)
{
    session_up();
    LS_EQ_INT(lr20xx_set_dio_function(4, LR20XX_DIO_FUNC_IRQ, 0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_set_dio_function(12, LR20XX_DIO_FUNC_IRQ, 0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_set_dio_rf_switch(8, 0xE0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_set_dio_irq(3, 1), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_set_agc_gain(14), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_calibrate(0x80), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_ook_set_detector(0x0285, 0, 0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_ook_set_detector(0x0285, 17, 0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_ook_set_detector(0x0285, 16, 32), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_ook_set_packet_fixed(16, 28, 5), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_ook_set_modulation(0, 0, 0, 0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);               /* not one of them reached the bus */

    /* The DIO functions are Standby RC only (5.1.1): the part says no in Rx. */
    LS_EQ_INT(lr20xx_set_packet_type_ook(), ESP_OK);
    LS_EQ_INT(lr20xx_set_rx(0xFFFFFFu), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_configure_dios(), ESP_ERR_INVALID_RESPONSE);
    LS_CHECK(fk.bad_mode > 0);
}

LS_CASE(the_wide_receive_bandwidth_ladder_is_table_11_4_at_two_megahertz_and_up)
{
    lr20xx_rx_bw_t b = lr20xx_wide_rx_bw(LR20XX_MODES_RX_BW_MIN_HZ);
    LS_EQ_UINT(b.hz, 2222222); LS_EQ_UINT(b.index, 192);
    b = lr20xx_wide_rx_bw(2300000);  LS_EQ_UINT(b.hz, 2666667); LS_EQ_UINT(b.index, 128);
    b = lr20xx_wide_rx_bw(2800000);  LS_EQ_UINT(b.hz, 2857143); LS_EQ_UINT(b.index, 64);
    b = lr20xx_wide_rx_bw(3000000);  LS_EQ_UINT(b.hz, 3076923); LS_EQ_UINT(b.index, 0);
    b = lr20xx_wide_rx_bw(9000000);  LS_EQ_UINT(b.hz, 3076923); LS_EQ_UINT(b.index, 0);
}

LS_CASE(a_mode_s_session_programs_exactly_these_frames_and_none_of_them_transmits)
{
    session_up();
    LS_CHECK(ls_lora_caps() & LS_LORA_CAP_MODES_RX);
    LS_CHECK(!ls_lora_modes_active());
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_CHECK(ls_lora_modes_active());

    int ix[80];
    const int n = cmds(ix, 80);
    LS_EQ_INT(n, 26);
    CMD(0,  0x01, 0x28, 0x00);                               /* SetStandby, RC            */
    CMD(1,  0x01, 0x12, 0x06, 0x23);                         /* RF switch and IRQ DIOs ... */
    CMD(2,  0x01, 0x13, 0x06, 0x08);
    CMD(3,  0x01, 0x12, 0x07, 0x23);
    CMD(4,  0x01, 0x13, 0x07, 0x00);
    CMD(5,  0x01, 0x12, 0x08, 0x23);
    CMD(6,  0x01, 0x13, 0x08, 0x06);
    CMD(7,  0x01, 0x12, 0x0A, 0x23);
    CMD(8,  0x01, 0x13, 0x0A, 0x08);
    CMD(9,  0x01, 0x12, 0x0B, 0x10);
    CMD(10, 0x01, 0x15, 0x0B, 0x00, 0xE7, 0x00, 0x00);
    CMD(11, 0x02, 0x06, 0x01);                               /* fallback: Standby RC      */
    CMD(12, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);             /* ClearIrq, all             */
    CMD(13, 0x01, 0x22, 0x2F);                               /* Calibrate: LF_RC HF_RC PLL AAF MU */
    CMD(14, 0x02, 0x07, 0x0A);                               /* SetPacketType OOK         */
    CMD(15, 0x01, 0x23, 0x01, 0x10, 0x00, 0x00, 0x00, 0x00); /* CalibFE 1088 MHz cell, LF */
    CMD(16, 0x02, 0x00, 0x40, 0xF8, 0x14, 0x80);             /* SetRfFrequency 1090 MHz   */
    CMD(17, 0x02, 0x01, 0x00, 0x07);                         /* SetRxPath LF, boost 7     */
    CMD(18, 0x02, 0x81, 0x00, 0x1E, 0x84, 0x80, 0x00, 0x00, 0x00);  /* 2 Mbps, no shaping, 3077 kHz, full depth */
    CMD(19, 0x02, 0x82, 0x00, 0x10, 0x00, 0x00, 0x1C, 0x00); /* fixed, 28 bytes, CRC off, Manchester off */
    CMD(20, 0x02, 0x84, 0x00, 0x00, 0x00, 0x00, 0x80);       /* no sync word              */
    CMD(21, 0x02, 0x88, 0x02, 0x85, 0x0F, 0x00, 0x00);       /* detector 0x0285, 16 bits  */
    CMD(22, 0x02, 0x1A, 0x0D);                               /* gain step 13              */
    CMD(23, 0x01, 0x1E);                                     /* ClearRxFifo               */
    CMD(24, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(25, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);                   /* SetRx, continuous         */

    /* The part ends up holding what the datasheet says it should. */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_UINT(fk.packet_type, 0x0A);
    LS_EQ_UINT(fk.agc, 13);
    LS_EQ_UINT(fk.last_freq_hz, 1090000000u);
    LS_EQ_UINT(fk.last_path, 0);
    LS_EQ_UINT(fk.last_boost, 7);
    LS_EQ_UINT(fk.calibrate_blocks, 0x2F);
    LS_EQ_UINT(fk.fallback, 1);
    LS_CHECK(fk.ook_mod_set && fk.ook_pkt_set && fk.ook_sync_set && fk.ook_det_set);
    LS_EQ_INT(fk.rx_arms, 1);

    /* Nothing refused, nothing early, nothing that transmits, no opcode
       outside the receive list. */
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(fk.tx_frames, 0);
    LS_CHECK(!tx_opcode_seen());
    LS_EQ_INT(unknown_frames(), 0);

    /* The antenna switch was driven once, to RF1: expander IO1 HIGH. */
    LS_EQ_INT(fk_xl_log_n, 1);
    LS_EQ_INT(fk_xl_log[0].pin, LS_BOARD_XL_RF_SW_VCTL);
    LS_CHECK(fk_xl_log[0].level);
}

LS_CASE(the_session_follows_the_requested_gain_step_and_agc_is_step_zero)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 0), ESP_OK);
    int ix[80];
    const int n = cmds(ix, 80);
    LS_EQ_INT(n, 26);
    CMD(22, 0x02, 0x1A, 0x00);

    /* A new step is written in standby and Rx armed again. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_gain(7), ESP_OK);
    const int m = cmds(ix, 80);
    LS_EQ_INT(m, 5);
    CMD(0, 0x01, 0x28, 0x00);
    CMD(1, 0x02, 0x1A, 0x07);
    CMD(2, 0x01, 0x1E);
    CMD(3, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(4, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    LS_EQ_UINT(fk.agc, 7);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* The same step again sends nothing. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_gain(7), ESP_OK);
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(ls_lora_modes_set_gain(14), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_modes_set_gain(-1), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.bad_args, 0);
}

LS_CASE(the_session_refuses_a_frequency_or_gain_outside_what_mode_s_uses)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1079999999u, 13), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_modes_begin(1100000001u, 13), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_modes_begin(868000000u, 13), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 14), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, -1), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    LS_CHECK(!ls_lora_modes_active());

    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_ERR_INVALID_STATE);   /* one session */
}

LS_CASE(the_antenna_switch_goes_to_rf1_unless_the_user_chose_mmcx1)
{
    session_up();
    fk_ant_external = true;
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(fk_xl_log_n, 0);              /* the user's choice stands */
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);

    fk_ant_external = false;
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(fk_xl_log_n, 1);
    LS_EQ_INT(fk_xl_log[0].pin, LS_BOARD_XL_RF_SW_VCTL);
    LS_CHECK(fk_xl_log[0].level);
}

LS_CASE(polling_with_nothing_waiting_is_one_status_frame)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.nframes = 0;
    uint8_t buf[28]; float rssi = 7;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_INT(fk.nframes, 1);
    LS_CHECK(fk_frame_is(0, (const uint8_t[]){ 0x01, 0, 0, 0, 0, 0 }, 6));
    LS_NEAR(rssi, 7, 0.001);

    /* Too small a buffer, or no session, is an error and sends nothing. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_poll(buf, 27, &rssi), -1);
    LS_EQ_INT(ls_lora_modes_poll(NULL, 28, &rssi), -1);
    LS_EQ_INT(fk.nframes, 0);
}

LS_CASE(a_received_frame_is_read_from_the_fifo_with_its_level)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t want[28]; frame_k(1, want);
    fk_push_frame(want);
    fk.ook_rssi_high = 62; fk.ook_rssi_avg = 80;
    fk.ook_rssi_flags = 0x01 | 0x04;          /* RssiHigh(0) and rssi_avg(0) */
    fk.nframes = 0;

    uint8_t buf[28]; float rssi = 0;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, want, 28) == 0);
    LS_NEAR(rssi, -62.5, 0.001);              /* RssiHigh: -125/2, not the average */

    int ix[16];
    const int n = cmds(ix, 16);
    LS_EQ_INT(n, 4);
    CMD(0, 0x01, 0x16, 0x00, 0x04, 0x00, 0x00);   /* ClearIrq, RxDone only */
    CMD(1, 0x01, 0x1C);                            /* GetRxFifoLevel        */
    LS_EQ_UINT(fk.frames[ix[2]].full, 30);         /* the direct read: Stat + 28 bytes */
    LS_EQ_UINT(fk.frames[ix[2]].b[0], 0x00);
    LS_EQ_UINT(fk.frames[ix[2]].b[1], 0x01);
    CMD(3, 0x02, 0x87);                            /* GetOokPacketStatus    */
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(a_failed_setup_leaves_the_part_in_standby_and_the_session_off)
{
    static const uint16_t fail_ops[] = { 0x0112, 0x0122, 0x0288, 0x020C };
    for (size_t i = 0; i < sizeof(fail_ops) / sizeof(fail_ops[0]); i++) {
        session_up();
        fk.fail_op = fail_ops[i];
        LS_CHECK_MSG(ls_lora_modes_begin(1090000000u, 13) != ESP_OK,
                     "setup survived a refused 0x%04X", (unsigned)fail_ops[i]);
        LS_CHECK(!ls_lora_modes_active());
        LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);
        LS_EQ_INT(fk.tx_frames, 0);
        /* And a later start works once the part behaves. */
        fk.fail_op = 0;
        LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
        LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    }
}

LS_CASE(several_frames_are_read_in_one_batch_and_handed_out_one_at_a_time)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[3][28];
    for (int k = 0; k < 3; k++) { frame_k(k + 1, f[k]); fk_push_frame(f[k]); }
    fk.ook_rssi_high = 50; fk.ook_rssi_flags = 0;       /* RssiHigh(8:1) = 50: -50 dBm */
    fk.nframes = 0;

    uint8_t buf[28]; float rssi = 0;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, f[0], 28) == 0);
    LS_CHECK(isnan(rssi));                    /* only the last frame has a level */
    const int after_first = fk.nframes;
    LS_CHECK(after_first > 0);

    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, f[1], 28) == 0);
    LS_CHECK(isnan(rssi));
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, f[2], 28) == 0);
    LS_NEAR(rssi, -50.0, 0.001);
    LS_EQ_INT(fk.nframes, after_first);        /* the rest came from the batch, no bus traffic */

    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.frames, 3);
    LS_EQ_UINT(st.overflows, 0);
    LS_EQ_UINT(st.bus_errors, 0);
}

LS_CASE(a_full_fifo_is_read_to_the_last_whole_frame_and_then_cleared)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[28];
    for (int k = 0; k < 10; k++) { frame_k(k, f); fk_push_frame(f); }   /* 280 bytes offered, 256 fit */
    LS_EQ_INT(fk.fifo_n, 256);
    fk.nframes = 0;

    uint8_t buf[28]; float rssi;
    int got = 0;
    while (ls_lora_modes_poll(buf, sizeof(buf), &rssi) == 28) got++;
    LS_EQ_INT(got, 9);                         /* 252 bytes: nine whole frames */
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.overflows, 1);
    LS_EQ_INT(fk.fifo_n, 0);                   /* the 4 bytes of a torn tenth frame are gone */

    /* The next frame arrives aligned. */
    frame_k(11, f);
    fk_push_frame(f);
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, f, 28) == 0);
}

LS_CASE(half_a_packet_that_never_completes_is_cleared_so_the_frames_after_it_stay_aligned)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[28]; frame_k(2, f);
    fk_push_frame(f);
    for (int i = 0; i < 10; i++) fk.fifo[fk.fifo_n++] = 0xEE;      /* ten stray bytes behind it */

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, f, 28) == 0);
    LS_EQ_INT(fk.fifo_n, 10);

    /* Nothing more arrives. Two looks leave it alone (a packet may be in
       flight, 112 us at 2 Mchip/s), the third clears it. */
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_INT(fk.fifo_n, 10);
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_INT(fk.fifo_n, 0);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.overflows, 1);

    frame_k(5, f);
    fk_push_frame(f);
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, f, 28) == 0);
}

LS_CASE(a_packet_still_arriving_is_not_mistaken_for_a_stale_remainder)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[28]; frame_k(3, f);
    fk_push_frame(f);
    for (int i = 0; i < 10; i++) fk.fifo[fk.fifo_n++] = 0xEE;

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);     /* stale count 1 */
    /* The rest of the packet arrives: 18 more bytes make 28. */
    for (int i = 0; i < 18; i++) fk.fifo[fk.fifo_n++] = (uint8_t)(0x40 + i);
    fk.irq |= LR20XX_IRQ_RX_DONE;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_EQ_UINT(buf[0], 0xEE);
    LS_EQ_UINT(buf[27], 0x40 + 17);
    LS_EQ_INT(fk.fifo_n, 0);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.overflows, 0);
}

LS_CASE(a_receiver_that_left_rx_is_armed_again_from_an_empty_fifo)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    /* A Timeout (bit 21) and the part back in Standby RC, as after an Rx
       timeout under the fallback mode. */
    fk.irq = LR20XX_IRQ_TIMEOUT;
    fk.mode = LR20XX_MODE_STBY_RC;
    fk.rx_arms = 0;
    fk.nframes = 0;

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    int ix[16];
    const int n = cmds(ix, 16);
    LS_EQ_INT(n, 4);
    CMD(0, 0x01, 0x16, 0x00, 0x20, 0x00, 0x00);          /* ClearIrq: Timeout */
    CMD(1, 0x01, 0x1E);                                   /* ClearRxFifo       */
    CMD(2, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(3, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);                 /* SetRx, continuous */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.rx_arms, 1);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.rearms, 1);
    LS_EQ_INT(fk.bad_mode, 0);
}

LS_CASE(a_frame_that_arrived_as_the_part_left_rx_is_delivered_before_rx_is_armed_again)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[28]; frame_k(4, f);
    fk_push_frame(f);
    fk.mode = LR20XX_MODE_STBY_RC;                         /* it fell out of Rx afterwards */

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);
    LS_CHECK(memcmp(buf, f, 28) == 0);
    /* The frame was read first; the next poll finds the part idle and re-arms. */
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
}

LS_CASE(a_chip_error_is_read_and_cleared_without_stopping_the_session)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.irq = LR20XX_IRQ_ERROR;
    fk.errors = 0x0200;                                    /* RxFreqNoCalErr, bit 9 */
    fk.nframes = 0;

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    int ix[16];
    const int n = cmds(ix, 16);
    LS_EQ_INT(n, 3);
    CMD(0, 0x01, 0x10);                                    /* GetErrors         */
    CMD(1, 0x01, 0x11);                                    /* ClearErrors       */
    CMD(2, 0x01, 0x16, 0x00, 0x01, 0x00, 0x00);            /* ClearIrq: Error   */
    LS_EQ_UINT(fk.errors, 0);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.chip_errors, 1);
    LS_CHECK(ls_lora_modes_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);                   /* still receiving, nothing to re-arm */
}

LS_CASE(a_chip_reset_in_the_middle_of_a_session_reprograms_the_whole_session)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 9), ESP_OK);
    /* Brown-out: Standby RC, reset source 1, everything it knew gone. */
    fk.mode = LR20XX_MODE_STBY_RC;
    fk.reset_src = 1;
    fk.packet_type = 0xFF; fk.agc = 0; fk.ook_det_set = false; memset(fk.dio_rfsw_set, 0, sizeof(fk.dio_rfsw_set));
    fk.nframes = 0;
    fk.rx_arms = 0;

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    int ix[80];
    const int n = cmds(ix, 80);
    LS_EQ_INT(n, 26);
    CMD(0, 0x01, 0x28, 0x00);
    CMD(22, 0x02, 0x1A, 0x09);                             /* the gain it had */
    CMD(25, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(fk.ook_det_set);
    LS_CHECK(fk.dio_rfsw_set[8]);
    LS_EQ_UINT(fk.agc, 9);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.resets, 1);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.tx_frames, 0);
}

LS_CASE(a_bus_failure_reading_the_fifo_does_not_lose_the_frame_that_caused_it)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[28]; frame_k(6, f);
    fk_push_frame(f);
    fk.stuck_after_frame1 = 0x011C;                        /* BUSY never falls after GetRxFifoLevel */

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), -2);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.bus_errors, 1);

    fk_busy_for = 0; fk.read_pending = false; fk.stuck_after_frame1 = 0;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 28);   /* RxDone was cleared; it still comes out */
    LS_CHECK(memcmp(buf, f, 28) == 0);
}

LS_CASE(a_status_that_reads_as_nothing_is_a_bus_error_not_an_empty_poll)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.kind = FK_ALL_FF;                                   /* MISO floats */
    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), -2);
    fk.kind = FK_LR;
}

LS_CASE(ending_the_session_puts_the_part_in_standby_and_clears_it)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[28]; frame_k(7, f);
    fk_push_frame(f);
    fk.nframes = 0;

    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    int ix[16];
    const int n = cmds(ix, 16);
    LS_EQ_INT(n, 3);
    CMD(0, 0x01, 0x28, 0x00);                              /* SetStandby, RC     */
    CMD(1, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(2, 0x01, 0x1E);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);
    LS_EQ_UINT(fk.irq, 0);
    LS_EQ_INT(fk.fifo_n, 0);
    LS_CHECK(!ls_lora_modes_active());

    /* Ending again, or polling after it, puts nothing on the bus. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), -1);
    LS_EQ_INT(fk.nframes, 0);

    /* And the session can start again. */
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.bad_mode, 0);
}

LS_CASE(stopping_the_dispatcher_drops_the_session_without_touching_the_bus)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.nframes = 0;
    ls_lora_stop();
    LS_EQ_INT(fk.nframes, 0);
    LS_CHECK(!ls_lora_modes_active());
    LS_EQ_UINT(ls_lora_caps() & LS_LORA_CAP_MODES_RX, 0);
    uint8_t buf[28];
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), NULL), -1);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_ERR_NOT_SUPPORTED);
}

LS_CASE(the_packet_status_reads_both_levels_with_their_half_decibel_bits)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.ook_rssi_avg = 80;  fk.ook_rssi_high = 62;
    fk.ook_rssi_flags = 0x04 | 0x01;           /* rssi_avg(0) = bit 2, RssiHigh(0) = bit 0 */
    fk.ook_lqi = 9;
    lr20xx_ook_pkt_status_t ps;
    LS_EQ_INT(lr20xx_get_ook_packet_status(&ps), ESP_OK);
    LS_EQ_UINT(ps.pkt_len, 28);
    LS_NEAR(ps.rssi_avg_dbm, -80.5, 0.001);
    LS_NEAR(ps.rssi_high_dbm, -62.5, 0.001);
    LS_EQ_UINT(ps.lqi, 9);
    fk.ook_rssi_flags = 0;
    LS_EQ_INT(lr20xx_get_ook_packet_status(&ps), ESP_OK);
    LS_NEAR(ps.rssi_avg_dbm, -80.0, 0.001);
    LS_NEAR(ps.rssi_high_dbm, -62.0, 0.001);
    fk.bad_read_stat = true;
    LS_EQ_INT(lr20xx_get_ook_packet_status(&ps), ESP_ERR_INVALID_RESPONSE);
    fk.bad_read_stat = false;
}

LS_CASE(across_a_whole_session_not_one_frame_is_a_transmit_or_unlisted)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    uint8_t f[28], buf[28]; float rssi;
    for (int k = 0; k < 5; k++) { frame_k(k, f); fk_push_frame(f); (void)ls_lora_modes_poll(buf, sizeof(buf), &rssi); }
    (void)ls_lora_modes_set_gain(5);
    fk.irq = LR20XX_IRQ_TIMEOUT; fk.mode = LR20XX_MODE_STBY_RC;
    (void)ls_lora_modes_poll(buf, sizeof(buf), &rssi);
    fk.irq = LR20XX_IRQ_ERROR; fk.errors = 4;
    (void)ls_lora_modes_poll(buf, sizeof(buf), &rssi);
    fk.reset_src = 2; fk.mode = LR20XX_MODE_STBY_RC;
    (void)ls_lora_modes_poll(buf, sizeof(buf), &rssi);
    (void)ls_lora_modes_end();
    LS_CHECK(!tx_opcode_seen());
    LS_EQ_INT(fk.tx_frames, 0);
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.violations, 0);
}

/* ================================================================== */
/* Live tuning of the Mode S session                                  */
/* ================================================================== */

static int count_frames_with(uint8_t b0, uint8_t b1)
{
    int n = 0;
    for (int i = 0; i < fk.nframes; i++)
        if (!fk.frames[i].read2 && fk.frames[i].b[0] == b0 && fk.frames[i].b[1] == b1) n++;
    return n;
}

LS_CASE(the_ook_filter_table_is_table_11_4_and_the_nearest_rung_wins)
{
    LS_EQ_INT(lr20xx_ook_rx_bw_table_n, 16);
    LS_EQ_UINT(lr20xx_ook_rx_bw_table[0].hz, 3076923);  LS_EQ_UINT(lr20xx_ook_rx_bw_table[0].index, 0);
    LS_EQ_UINT(lr20xx_ook_rx_bw_table[3].hz, 2222222);  LS_EQ_UINT(lr20xx_ook_rx_bw_table[3].index, 192);
    LS_EQ_UINT(lr20xx_ook_rx_bw_table[4].hz, 1333333);  LS_EQ_UINT(lr20xx_ook_rx_bw_table[4].index, 136);
    LS_EQ_UINT(lr20xx_ook_rx_bw_table[15].hz, 512821);  LS_EQ_UINT(lr20xx_ook_rx_bw_table[15].index, 17);
    for (int i = 1; i < lr20xx_ook_rx_bw_table_n; i++)
        LS_CHECK(lr20xx_ook_rx_bw_table[i].hz < lr20xx_ook_rx_bw_table[i - 1].hz);
    /* The four widest are the ones lr20xx_wide_rx_bw already knew. */
    for (int i = 0; i < 4; i++) {
        const lr20xx_rx_bw_t w = lr20xx_wide_rx_bw(lr20xx_ook_rx_bw_table[i].hz);
        LS_EQ_UINT(w.hz, lr20xx_ook_rx_bw_table[i].hz);
        LS_EQ_UINT(w.index, lr20xx_ook_rx_bw_table[i].index);
    }

    lr20xx_rx_bw_t b = lr20xx_ook_rx_bw_nearest(3077000);   LS_EQ_UINT(b.index, 0);
    b = lr20xx_ook_rx_bw_nearest(2222000);                  LS_EQ_UINT(b.index, 192);
    b = lr20xx_ook_rx_bw_nearest(2400000);                  LS_EQ_UINT(b.index, 192);   /* 2222 is nearer than 2667 */
    b = lr20xx_ook_rx_bw_nearest(2500000);                  LS_EQ_UINT(b.index, 128);
    b = lr20xx_ook_rx_bw_nearest(1000000);                  LS_EQ_UINT(b.index, 200);   /* 1111 */
    b = lr20xx_ook_rx_bw_nearest(1);                        LS_EQ_UINT(b.index, 17);
    b = lr20xx_ook_rx_bw_nearest(100000000u);               LS_EQ_UINT(b.index, 0);
    b = lr20xx_ook_rx_bw_nearest(0);                        LS_EQ_UINT(b.index, 17);
}

LS_CASE(a_new_session_starts_from_the_defaults_and_reports_them)
{
    session_up();
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_ERR_INVALID_STATE);      /* no session yet */
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_EQ_INT(t.gain_step, 13);
    LS_EQ_INT(t.boost, 7);
    LS_EQ_UINT(t.rx_bw_hz, 3076923);
    LS_CHECK(!t.thresh_override);
    LS_EQ_INT(fk.thr_writes, 0);                                      /* the chip's own threshold stands */
}

LS_CASE(gain_auto_is_step_zero_the_chips_agc_and_is_applied_live)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_gain(0), ESP_OK);
    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 5);
    CMD(0, 0x01, 0x28, 0x00);                              /* SetStandby, RC             */
    CMD(1, 0x02, 0x1A, 0x00);                              /* SetAgcGainManual 0 = AGC   */
    CMD(2, 0x01, 0x1E);                                    /* ClearRxFifo                */
    CMD(3, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);            /* ClearIrq                   */
    CMD(4, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);                  /* SetRx, continuous          */
    LS_EQ_UINT(fk.agc, 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_EQ_INT(t.gain_step, 0);
    LS_EQ_INT(fk.bad_mode, 0); LS_EQ_INT(fk.violations, 0);
}

LS_CASE(boost_is_set_in_standby_with_the_receive_path_and_rx_is_armed_again)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_boost(3), ESP_OK);
    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 5);
    CMD(0, 0x01, 0x28, 0x00);
    CMD(1, 0x02, 0x01, 0x00, 0x03);                        /* SetRxPath: LF path, boost 3 */
    CMD(2, 0x01, 0x1E);
    CMD(3, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(4, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    LS_EQ_UINT(fk.last_path, 0);
    LS_EQ_UINT(fk.last_boost, 3);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_EQ_INT(t.boost, 3);
    LS_EQ_UINT(fk.calibrate_blocks, 0x2F);                 /* no recalibration for a boost change */

    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_boost(3), ESP_OK);         /* already in force */
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(ls_lora_modes_set_boost(8), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_modes_set_boost(-1), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(fk.bad_mode, 0); LS_EQ_INT(fk.bad_args, 0); LS_EQ_INT(fk.violations, 0);
}

LS_CASE(the_receive_bandwidth_goes_to_the_nearest_rung_through_set_ook_modulation_params)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.nframes = 0;
    uint32_t chosen = 0;
    LS_EQ_INT(ls_lora_modes_set_bw(2300000, &chosen), ESP_OK);   /* nearer 2222 than 2667 */
    LS_EQ_UINT(chosen, 2222222);
    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 5);
    CMD(0, 0x01, 0x28, 0x00);
    CMD(1, 0x02, 0x81, 0x00, 0x1E, 0x84, 0x80, 0x00, 0xC0, 0x00);   /* 2 Mbps, no shaping, rx_bw 192, full depth */
    CMD(2, 0x01, 0x1E);
    CMD(3, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(4, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    LS_EQ_UINT(fk.ook_mod[5], 0xC0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_EQ_UINT(t.rx_bw_hz, 2222222);

    fk.nframes = 0;
    chosen = 0;
    LS_EQ_INT(ls_lora_modes_set_bw(2222222, &chosen), ESP_OK);   /* the rung in use: nothing to send */
    LS_EQ_UINT(chosen, 2222222);
    LS_EQ_INT(fk.nframes, 0);

    LS_EQ_INT(ls_lora_modes_set_bw(100000000u, &chosen), ESP_OK);   /* widest */
    LS_EQ_UINT(chosen, 3076923);
    LS_EQ_UINT(fk.ook_mod[5], 0x00);
    LS_EQ_INT(fk.bad_mode, 0); LS_EQ_INT(fk.bad_args, 0); LS_EQ_INT(fk.violations, 0);
}

LS_CASE(the_detection_threshold_is_one_masked_register_write_and_a_read_back)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    int raw = -1;
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_read_threshold(&raw), ESP_OK);
    LS_EQ_INT(raw, 34);                                    /* the chip's own, the fake's 34 */
    LS_EQ_INT(fk.nframes, 2);
    FK_FRAME(0, 0x01, 0x06, 0xF3, 0x0E, 0x14, 0x01);       /* ReadRegMem32, 1 word      */
    LS_CHECK(fk.frames[1].read2 && fk.frames[1].full == 6);
    LS_EQ_INT(fk.mode, LR20XX_MODE_RX);                    /* a read leaves the chip as it was */

    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_threshold(-10, false), ESP_OK);
    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 5);
    CMD(0, 0x01, 0x28, 0x00);
    /* WriteRegMemMask32: address 0xF30E14, mask bits 26:20, data 64 + (-10) = 54 in them. */
    CMD(1, 0x01, 0x05, 0xF3, 0x0E, 0x14, 0x07, 0xF0, 0x00, 0x00, 0x03, 0x60, 0x00, 0x00);
    CMD(2, 0x01, 0x1E);
    CMD(3, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(4, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    LS_EQ_UINT(fk.thr_reg, 0x03600123u);                   /* the other bits of the word are untouched */
    LS_EQ_INT(fk.bad_reg, 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    LS_EQ_INT(ls_lora_modes_read_threshold(&raw), ESP_OK);
    LS_EQ_INT(raw, 54);
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_CHECK(t.thresh_override);
    LS_EQ_INT(t.thresh_level, -10);

    /* The same level again sends nothing; the ends of the range are accepted, one beyond is not. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_threshold(-10, false), ESP_OK);
    LS_EQ_INT(ls_lora_modes_set_threshold(64, false), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_modes_set_threshold(-65, false), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(ls_lora_modes_set_threshold(63, false), ESP_OK);
    LS_EQ_UINT(fk.thr_reg, (127u << 20) | 0x123u);
    LS_EQ_INT(ls_lora_modes_set_threshold(-64, false), ESP_OK);
    LS_EQ_UINT(fk.thr_reg, 0x123u);
}

LS_CASE(a_new_bandwidth_puts_the_threshold_override_back_and_automatic_runs_the_modulation_again)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_modes_set_threshold(-10, false), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_bw(2222222, NULL), ESP_OK);
    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 6);
    CMD(0, 0x01, 0x28, 0x00);
    CMD(1, 0x02, 0x81, 0x00, 0x1E, 0x84, 0x80, 0x00, 0xC0, 0x00);
    CMD(2, 0x01, 0x05, 0xF3, 0x0E, 0x14, 0x07, 0xF0, 0x00, 0x00, 0x03, 0x60, 0x00, 0x00);   /* ours again, after it */
    CMD(3, 0x01, 0x1E);
    CMD(4, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(5, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);

    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_threshold(0, true), ESP_OK);     /* automatic */
    LS_EQ_INT(cmds(ix, 16), 5);
    CMD(0, 0x01, 0x28, 0x00);
    CMD(1, 0x02, 0x81, 0x00, 0x1E, 0x84, 0x80, 0x00, 0xC0, 0x00);   /* the chip derives its own */
    CMD(2, 0x01, 0x1E);
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_CHECK(!t.thresh_override);

    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_set_threshold(0, true), ESP_OK);     /* nothing to hand back */
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(fk.bad_mode, 0); LS_EQ_INT(fk.bad_args, 0); LS_EQ_INT(fk.bad_reg, 0);
}

LS_CASE(the_tuning_survives_a_chip_reset_and_is_gone_with_the_session)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_modes_set_gain(5), ESP_OK);
    LS_EQ_INT(ls_lora_modes_set_boost(2), ESP_OK);
    LS_EQ_INT(ls_lora_modes_set_bw(2222222, NULL), ESP_OK);
    LS_EQ_INT(ls_lora_modes_set_threshold(-10, false), ESP_OK);

    /* The part restarts under the session: everything is programmed again, with the tuning. */
    fk.irq = 0; fk.reset_src = 2; fk.mode = LR20XX_MODE_STBY_RC;
    fk.agc = 0; fk.last_boost = 0; memset(fk.ook_mod, 0, sizeof(fk.ook_mod)); fk.thr_reg = 0x123u;
    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.resets, 1);
    LS_EQ_UINT(fk.agc, 5);
    LS_EQ_UINT(fk.last_boost, 2);
    LS_EQ_UINT(fk.ook_mod[5], 0xC0);
    LS_EQ_UINT(fk.thr_reg, 0x03600123u);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* A new session is the defaults again, not what the last one was left at. */
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_EQ_INT(t.gain_step, 13);
    LS_EQ_INT(t.boost, 7);
    LS_EQ_UINT(t.rx_bw_hz, 3076923);
    LS_CHECK(!t.thresh_override);
    LS_EQ_UINT(fk.last_boost, 7);
    LS_EQ_UINT(fk.ook_mod[5], 0x00);
}

LS_CASE(tuning_needs_a_session_and_an_lr20xx_and_none_of_it_transmits)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_set_boost(3), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_modes_set_bw(2222222, NULL), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_modes_set_threshold(-10, false), ESP_ERR_INVALID_STATE);
    int raw;
    LS_EQ_INT(ls_lora_modes_read_threshold(&raw), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk.nframes, 0);

    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    (void)ls_lora_modes_set_gain(0);
    (void)ls_lora_modes_set_boost(0);
    (void)ls_lora_modes_set_bw(1111111, NULL);
    (void)ls_lora_modes_set_threshold(5, false);
    (void)ls_lora_modes_read_threshold(&raw);
    (void)ls_lora_modes_set_threshold(0, true);
    LS_CHECK(!tx_opcode_seen());
    LS_EQ_INT(fk.tx_frames, 0);
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.bad_args, 0); LS_EQ_INT(fk.bad_mode, 0); LS_EQ_INT(fk.bad_reg, 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_UINT(ls_lora_hw_unguarded(), 0);

    /* An SX126x has none of it. */
    fk_install(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_modes_set_boost(3), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(ls_lora_modes_set_bw(2222222, NULL), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(ls_lora_modes_set_threshold(0, true), ESP_ERR_NOT_SUPPORTED);
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(ls_lora_modes_read_threshold(&raw), ESP_ERR_NOT_SUPPORTED);
}

LS_CASE(a_failed_step_leaves_the_part_in_standby_and_the_next_poll_arms_rx_again)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.fail_op = 0x0201;                                     /* SetRxPath refused */
    LS_CHECK(ls_lora_modes_set_boost(4) != ESP_OK);
    fk.fail_op = 0;
    ls_lora_modes_tuning_t t;
    LS_EQ_INT(ls_lora_modes_tuning(&t), ESP_OK);
    LS_EQ_INT(t.boost, 7);                                   /* what the part holds is what is reported */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);

    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);   /* the pump's own recovery */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    lr20xx_modes_stats_t st; lr20xx_modes_stats(&st);
    LS_EQ_UINT(st.rearms, 1);
    LS_EQ_INT(count_frames_with(0x02, 0x01), 2);             /* SetRxPath: begin's, and the refused one */
}

/* ================================================================== */
/* LoRa packets, FSK sessions, the sweep and the LF transmitter       */
/* ================================================================== */

/* Configured (the defaults unless `cfg`) and receiving, with the log empty. */
static void lora_up(const ls_lora_cfg_t *cfg)
{
    session_up();
    ls_lora_cfg_t c;
    if (!cfg) { ls_lora_cfg_default(&c); cfg = &c; }
    LS_EQ_INT(ls_lora_configure(cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    fk.nframes = 0;
}

/* Nothing selected, configured or powered the HF PA, and no transmit started
   at 1 GHz or above. Checked against the part's state and every frame logged. */
static void no_hf_transmit(void)
{
    LS_EQ_INT(fk.hf_pa, 0);
    LS_EQ_INT(fk.tx_hf, 0);
    for (int i = 0; i < fk.nframes; i++) {
        if (fk.frames[i].read2) continue;
        const uint16_t op = (uint16_t)((fk.frames[i].b[0] << 8) | fk.frames[i].b[1]);
        if (op == 0x020F) LS_EQ_UINT(fk.frames[i].b[2], 0x00);       /* SelPa LF   */
        if (op == 0x0202) LS_EQ_UINT(fk.frames[i].b[2] & 0x80, 0);   /* PaSel LF   */
    }
}

static const ls_fsk_cfg_t FSK_868 = {
    .freq_hz = 868000000, .bitrate = 2400, .deviation_hz = 4800, .bandwidth_hz = 19500,
    .sync_word = 0x7cd215d8, .payload_bytes = 8, .preamble_bits = 64, .power_dbm = 10,
    .sync_bits = 0,
};

LS_CASE(a_lora_configure_at_22_dbm_programs_exactly_these_frames)
{
    session_up();
    /* 910.525 MHz, SF7, 62.5 kHz, CR 4/5, 22 dBm, 32 symbols, 0x12, CRC on. */
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);

    int ix[80];
    const int n = cmds(ix, 80);
    LS_EQ_INT(n, 32);
    CMD(0,  0x01, 0x28, 0x00);                               /* SetStandby, RC            */
    CMD(1,  0x01, 0x12, 0x06, 0x23);
    CMD(2,  0x01, 0x13, 0x06, 0x08);
    CMD(3,  0x01, 0x12, 0x07, 0x23);
    CMD(4,  0x01, 0x13, 0x07, 0x00);                         /* DIO7: nothing in Tx HF    */
    CMD(5,  0x01, 0x12, 0x08, 0x23);
    CMD(6,  0x01, 0x13, 0x08, 0x06);                         /* DIO8: Rx LF and Tx LF     */
    CMD(7,  0x01, 0x12, 0x0A, 0x23);
    CMD(8,  0x01, 0x13, 0x0A, 0x08);
    CMD(9,  0x01, 0x12, 0x0B, 0x10);
    CMD(10, 0x01, 0x15, 0x0B, 0x00, 0xEF, 0x02, 0x00);       /* + TxDone, LoRa header error */
    CMD(11, 0x02, 0x06, 0x01);                               /* fallback: Standby RC      */
    CMD(12, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(13, 0x01, 0x22, 0x2F);                               /* Calibrate, no PA_OFF      */
    CMD(14, 0x02, 0x07, 0x00);                               /* SetPacketType LoRa        */
    CMD(15, 0x01, 0x05, 0xF2, 0x00, 0x24, 0x00, 0xF0, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00);
    CMD(16, 0x01, 0x05, 0xF2, 0x00, 0x24, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00);
    CMD(17, 0x01, 0x04, 0x80, 0x00, 0x4C, 0x00, 0x2C, 0xCC, 0xCC);   /* DC-DC reset: 2.8 MHz */
    CMD(18, 0x01, 0x23, 0x00, 0xE3, 0x00, 0x00, 0x00, 0x00); /* CalibFE 908 MHz cell, LF  */
    CMD(19, 0x02, 0x00, 0x36, 0x45, 0x82, 0x48);             /* SetRfFrequency 910.525    */
    CMD(20, 0x02, 0x01, 0x00, 0x00);                         /* SetRxPath LF, boost 0     */
    CMD(21, 0x02, 0x20, 0x73, 0x10);                         /* SF7 | BW 0x03; CR 1, LDRO off */
    CMD(22, 0x01, 0x06, 0xF4, 0x02, 0x00, 0x01);             /* DC-DC: read ADC control   */
    CMD(23, 0x01, 0x05, 0xF2, 0x00, 0x24, 0x00, 0xF0, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00);
    CMD(24, 0x01, 0x05, 0xF2, 0x00, 0x24, 0x00, 0x0F, 0x00, 0x00, 0x00, 0x0F, 0x00, 0x00);
    CMD(25, 0x01, 0x04, 0x80, 0x00, 0x4C, 0x00, 0x2C, 0xCC, 0xCC);
    CMD(26, 0x02, 0x00, 0x36, 0x45, 0x82, 0x48);             /* and the frequency again   */
    CMD(27, 0x02, 0x21, 0x00, 0x20, 0xFF, 0x02);             /* 32 symbols, 255, explicit, CRC, IQ standard */
    CMD(28, 0x02, 0x23, 0x12);                               /* the one-byte sync word    */
    CMD(29, 0x02, 0x0F, 0x00);                               /* SelPa: the LF PA          */
    CMD(30, 0x02, 0x02, 0x00, 0x67, 0x10);                   /* PaSel LF; duty 6, slices 7; HF unused */
    CMD(31, 0x02, 0x03, 0x23, 0x05);                         /* power 35, 48 us ramp      */

    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_EQ_INT(fk.tx_starts, 0);                              /* configuring sends nothing on air */
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.bad_reg, 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(unknown_frames(), 0);
    no_hf_transmit();
    const ls_lora_cfg_t *got = ls_lora_cfg();
    LS_CHECK(got != NULL);
    LS_EQ_UINT(got->bw_hz, 62500);
    LS_EQ_UINT(got->freq_hz, 910525000u);
}

LS_CASE(the_lf_pa_is_radiolibs_table_for_the_power_and_is_clamped_to_minus_9_to_22)
{
    lr20xx_pa_lf_t p = lr20xx_pa_lf_for_dbm(22);
    LS_EQ_UINT(p.duty, 6); LS_EQ_UINT(p.slices, 7); LS_EQ_INT(p.power, 35);
    p = lr20xx_pa_lf_for_dbm(-9);
    LS_EQ_UINT(p.duty, 1); LS_EQ_UINT(p.slices, 1); LS_EQ_INT(p.power, 8);
    p = lr20xx_pa_lf_for_dbm(0);
    LS_EQ_UINT(p.duty, 1); LS_EQ_UINT(p.slices, 1); LS_EQ_INT(p.power, 18);
    p = lr20xx_pa_lf_for_dbm(14);
    LS_EQ_UINT(p.duty, 3); LS_EQ_UINT(p.slices, 2); LS_EQ_INT(p.power, 39);
    p = lr20xx_pa_lf_for_dbm(40);                  /* clamped to 22 */
    LS_EQ_INT(p.power, 35); LS_EQ_UINT(p.slices, 7);
    p = lr20xx_pa_lf_for_dbm(-40);                 /* clamped to -9 */
    LS_EQ_INT(p.power, 8);

    session_up();
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    cfg.power_dbm = -9;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_UINT(fk.pa_cfg[0], 0x00); LS_EQ_UINT(fk.pa_cfg[1], 0x11); LS_EQ_UINT(fk.pa_cfg[2], 0x10);
    LS_EQ_UINT(fk.tx_params[0], 0x08); LS_EQ_UINT(fk.tx_params[1], 0x05);
    cfg.power_dbm = 14;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_UINT(fk.pa_cfg[1], 0x32);
    LS_EQ_UINT(fk.tx_params[0], 0x27);

    /* Outside -9..22 is refused, as on the SX126x, before anything is sent. */
    fk.nframes = 0;
    cfg.power_dbm = 23;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_INVALID_ARG);
    cfg.power_dbm = -10;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    no_hf_transmit();
}

LS_CASE(the_lora_bandwidth_is_the_lr2021_table_snapped_to_the_nearest_rung)
{
    LS_EQ_INT(lr20xx_lora_bw_table_n, 12);
    for (int i = 1; i < lr20xx_lora_bw_table_n; i++)
        LS_CHECK(lr20xx_lora_bw_table[i].hz > lr20xx_lora_bw_table[i - 1].hz);
    lr20xx_lora_bw_t b = lr20xx_lora_bw_nearest(62500);   LS_EQ_UINT(b.hz, 62500);  LS_EQ_UINT(b.code, 0x03);
    b = lr20xx_lora_bw_nearest(125000);                   LS_EQ_UINT(b.hz, 125000); LS_EQ_UINT(b.code, 0x04);
    b = lr20xx_lora_bw_nearest(250000);                   LS_EQ_UINT(b.hz, 250000); LS_EQ_UINT(b.code, 0x05);
    b = lr20xx_lora_bw_nearest(500000);                   LS_EQ_UINT(b.hz, 500000); LS_EQ_UINT(b.code, 0x06);
    b = lr20xx_lora_bw_nearest(7810);                     LS_EQ_UINT(b.hz, 31250);  LS_EQ_UINT(b.code, 0x02);
    b = lr20xx_lora_bw_nearest(41670);                    LS_EQ_UINT(b.hz, 41667);  LS_EQ_UINT(b.code, 0x0A);
    b = lr20xx_lora_bw_nearest(100000);                   LS_EQ_UINT(b.hz, 101563); LS_EQ_UINT(b.code, 0x0C);
    b = lr20xx_lora_bw_nearest(160000);                   LS_EQ_UINT(b.hz, 125000);
    b = lr20xx_lora_bw_nearest(200000);                   LS_EQ_UINT(b.hz, 203125); LS_EQ_UINT(b.code, 0x0D);
    b = lr20xx_lora_bw_nearest(5000000);                  LS_EQ_UINT(b.hz, 1000000); LS_EQ_UINT(b.code, 0x07);

    session_up();
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    cfg.bw_hz = 125000;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_UINT(fk.lora_mod[0], 0x74);
    LS_EQ_UINT(ls_lora_cfg()->bw_hz, 125000);
    cfg.bw_hz = 10420;                                    /* an SX126x rung this part does not have */
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_UINT(fk.lora_mod[0], 0x72);
    LS_EQ_UINT(ls_lora_cfg()->bw_hz, 31250);              /* what was set is what is reported */
}

LS_CASE(ldro_is_on_from_a_16_ms_symbol)
{
    LS_CHECK(lr20xx_lora_ldro(12, 125000));               /* 32.8 ms */
    LS_CHECK(lr20xx_lora_ldro(11, 125000));               /* 16.4 ms */
    LS_CHECK(!lr20xx_lora_ldro(10, 125000));              /*  8.2 ms */
    LS_CHECK(lr20xx_lora_ldro(10, 62500));
    LS_CHECK(lr20xx_lora_ldro(9, 31250));
    LS_CHECK(!lr20xx_lora_ldro(8, 31250));
    LS_CHECK(!lr20xx_lora_ldro(7, 62500));
    LS_CHECK(!lr20xx_lora_ldro(7, 0));                    /* no bandwidth: off, not a division */

    session_up();
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    cfg.sf = 12; cfg.bw_hz = 125000; cfg.cr = 8;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_UINT(fk.lora_mod[0], 0xC4);
    LS_EQ_UINT(fk.lora_mod[1], 0x41);                     /* CR 4/8 is 4, LDRO on */
}

LS_CASE(airtime_is_the_semtech_formula_and_agrees_with_the_sx126x_backend)
{
    static const struct { uint8_t sf; uint32_t bw; uint8_t cr; uint16_t pre; bool crc; int len; } c[] = {
        { 7, 125000, 5,  8, true,  10 },
        { 12, 125000, 8, 8, true,  51 },
        { 11, 125000, 5, 16, true, 20 },
        { 9,  62500, 6, 32, false, 100 },
        { 10, 31250, 5,  8, true, 255 },
        { 5, 500000, 5,  8, true,   0 },
        { 8, 250000, 7, 12, false,  1 },
    };
    const int nc = (int)(sizeof(c) / sizeof(c[0]));
    uint32_t lr[8] = { 0 };

    session_up();
    for (int i = 0; i < nc; i++) {
        ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
        cfg.sf = c[i].sf; cfg.bw_hz = c[i].bw; cfg.cr = c[i].cr;
        cfg.preamble = c[i].pre; cfg.crc_on = c[i].crc;
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        lr[i] = ls_lora_airtime_ms(c[i].len);
    }
    /* SF7, 125 kHz, CR 4/5, 10 bytes, 8 symbols, explicit header, CRC: 41.216 ms. */
    LS_EQ_UINT(lr[0], 42);

    fk_install(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_SX126X);
    for (int i = 0; i < nc; i++) {
        ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
        cfg.sf = c[i].sf; cfg.bw_hz = c[i].bw; cfg.cr = c[i].cr;
        cfg.preamble = c[i].pre; cfg.crc_on = c[i].crc;
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        LS_CHECK_MSG(ls_lora_airtime_ms(c[i].len) == lr[i],
                     "case %d: SX126x %lu ms, LR20xx %lu ms", i,
                     (unsigned long)ls_lora_airtime_ms(c[i].len), (unsigned long)lr[i]);
    }
}

LS_CASE(a_received_lora_packet_is_read_with_its_level_and_the_fifo_left_empty)
{
    lora_up(NULL);
    LS_CHECK(ls_lora_is_receiving());
    const int arms = fk.rx_arms;
    uint8_t want[10];
    for (int i = 0; i < 10; i++) want[i] = (uint8_t)(0xA0 + i);
    fk_push_packet(want, 10);
    fk.lora_rssi_hi = 90; fk.lora_rssi_flags = 0x02;      /* 181 half-dB: -90.5 dBm */
    fk.lora_snr_q = -10;                                  /* -2.5 dB */

    uint8_t buf[32]; float rssi = 0, snr = 0;
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &rssi, &snr), 10);
    LS_CHECK(memcmp(buf, want, 10) == 0);
    LS_NEAR(rssi, -90.5, 0.001);
    LS_NEAR(snr, -2.5, 0.001);

    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 5);
    CMD(0, 0x01, 0x16, 0x00, 0x04, 0x00, 0x00);           /* ClearIrq, RxDone only     */
    CMD(1, 0x02, 0x12);                                    /* GetRxPktLength            */
    LS_EQ_UINT(fk.frames[ix[2]].full, 12);                 /* the direct read: Stat + 10 */
    LS_EQ_UINT(fk.frames[ix[2]].b[0], 0x00);
    LS_EQ_UINT(fk.frames[ix[2]].b[1], 0x01);
    CMD(3, 0x02, 0x2A);                                    /* GetLoraPacketStatus       */
    CMD(4, 0x01, 0x1E);                                    /* ClearRxFifo               */
    LS_EQ_INT(fk.fifo_n, 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);                   /* continuous Rx: nothing to re-arm */
    LS_EQ_INT(fk.rx_arms, arms);

    /* Nothing waiting is one GetStatus and nothing else. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &rssi, &snr), 0);
    LS_EQ_INT(fk.nframes, 1);
    LS_EQ_INT(fk_xl_gets, 0);                              /* no DIO read over the expander */
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(a_crc_or_header_error_is_negative_and_the_receiver_keeps_listening)
{
    lora_up(NULL);
    uint8_t junk[12] = { 0 };
    fk_push_packet(junk, 12);
    fk.irq |= LR20XX_IRQ_CRC_ERROR;
    uint8_t buf[32]; float rssi = 7, snr = 7;
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &rssi, &snr), -1);
    LS_EQ_INT(fk.fifo_n, 0);                               /* the bad payload is gone */
    LS_NEAR(rssi, 7, 0.001);                               /* and nothing was reported for it */
    LS_EQ_UINT(fk.irq, 0);

    fk.irq = LR20XX_IRQ_LORA_HDR_ERROR;
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &rssi, &snr), -1);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* A packet longer than the caller's buffer: what fits, and the rest dropped. */
    uint8_t p[20];
    for (int i = 0; i < 20; i++) p[i] = (uint8_t)i;
    fk_push_packet(p, 20);
    LS_EQ_INT(ls_lora_poll(buf, 8, &rssi, &snr), 8);
    LS_CHECK(memcmp(buf, p, 8) == 0);
    LS_EQ_INT(fk.fifo_n, 0);
    LS_EQ_INT(fk.bad_mode, 0);
}

LS_CASE(a_receiver_that_fell_out_of_rx_or_was_reset_is_armed_again_by_the_poll)
{
    lora_up(NULL);
    uint8_t buf[16]; float r, s;
    fk.mode = LR20XX_MODE_STBY_RC;                         /* dropped out, nothing said why */
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* A reset wipes the part: the next poll programs it again in full. */
    fk.mode = LR20XX_MODE_STBY_RC; fk.reset_src = 1;
    fk.packet_type = 0xFF; fk.lora_mod_set = false;
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_CHECK(fk.lora_mod_set);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_INT(fk.bad_mode, 0);
}

LS_CASE(receive_is_idempotent_and_does_not_disturb_a_listening_part)
{
    lora_up(NULL);
    const int arms = fk.rx_arms;
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_EQ_INT(fk.rx_arms, arms);
    LS_EQ_INT(fk.nframes, 2);                              /* a GetStatus each, no standby */
    LS_EQ_INT(fk.bad_mode, 0);
}

LS_CASE(send_fills_the_tx_fifo_then_sets_tx_and_send_done_waits_for_tx_done)
{
    lora_up(NULL);
    const uint8_t msg[5] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5 };
    LS_EQ_INT(ls_lora_send(msg, sizeof(msg)), ESP_OK);

    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 6);
    CMD(0, 0x01, 0x28, 0x00);                              /* SetStandby               */
    CMD(1, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);            /* ClearIrq                 */
    CMD(2, 0x02, 0x21, 0x00, 0x20, 0x05, 0x02);            /* the payload's own length */
    CMD(3, 0x01, 0x1F);                                    /* ClearTxFifo              */
    CMD(4, 0x00, 0x02, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5);      /* WriteRadioTxFifo, one frame */
    CMD(5, 0x02, 0x0D, 0x00, 0x00, 0x00);                  /* SetTx, no timeout        */
    LS_EQ_INT(fk.txfifo_n, 5);
    LS_CHECK(memcmp(fk.txfifo, msg, 5) == 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_TX);
    LS_EQ_INT(fk.tx_starts, 1);
    LS_EQ_UINT(fk.tx_freq_hz, 910525000u);
    LS_CHECK(!ls_lora_is_receiving());

    /* One at a time, and never more than 255 bytes. */
    LS_EQ_INT(ls_lora_send(msg, sizeof(msg)), ESP_ERR_INVALID_STATE);
    static uint8_t big[256];
    LS_EQ_INT(ls_lora_send(big, sizeof(big)), ESP_ERR_INVALID_SIZE);
    LS_EQ_INT(ls_lora_send(NULL, 4), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.tx_starts, 1);

    LS_CHECK(!ls_lora_send_done());
    fk_tx_complete();
    fk.nframes = 0;
    LS_CHECK(ls_lora_send_done());
    LS_EQ_INT(cmds(ix, 16), 2);
    CMD(0, 0x01, 0x28, 0x00);                              /* back to standby          */
    CMD(1, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    fk.nframes = 0;
    LS_CHECK(ls_lora_send_done());                         /* nothing in flight: no bus */
    LS_EQ_INT(fk.nframes, 0);

    /* What the mesh does next: listen again, at the full receive length. */
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_UINT(fk.lora_pkt[2], 0xFF);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.violations, 0);
    no_hf_transmit();
}

LS_CASE(a_transmit_that_never_ends_is_given_up_at_its_deadline_and_stood_down)
{
    lora_up(NULL);
    const uint8_t msg[3] = { 1, 2, 3 };
    LS_EQ_INT(ls_lora_send(msg, sizeof(msg)), ESP_OK);
    LS_CHECK(!ls_lora_send_done());
    ls_shim_time_advance(9000000);
    LS_CHECK(ls_lora_send_done());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);              /* the transmitter is off */
    LS_EQ_INT(ls_lora_send(msg, sizeof(msg)), ESP_OK);     /* and the next one may go */
}

LS_CASE(nothing_transmits_at_or_above_1_ghz_and_the_hf_pa_is_never_touched)
{
    session_up();
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    cfg.freq_hz = 2440000000u;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_NOT_SUPPORTED);
    cfg.freq_hz = 1000000000u;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_NOT_SUPPORTED);
    cfg.freq_hz = 1090000000u;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(fk.nframes, 0);                              /* refused before a frame */
    LS_CHECK(ls_lora_cfg() == NULL);
    const uint8_t msg[2] = { 1, 2 };
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_ERR_INVALID_STATE);

    /* Below the limit is the LF path, and transmits. */
    cfg.freq_hz = 433920000u;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_OK);
    LS_EQ_UINT(fk.tx_freq_hz, 433920000u);
    fk_tx_complete();
    LS_CHECK(ls_lora_send_done());

    /* The command layer holds the line by itself, whatever is asked of it. */
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    LS_EQ_INT(lr20xx_set_rf_frequency(2440000000u), ESP_OK);
    LS_CHECK(!lr20xx_tx_allowed());
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_set_pa_lf(10), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(lr20xx_set_tx(), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(fk.nframes, 0);
    /* Back below 1 GHz, the LF PA has to be programmed again before SetTx goes. */
    LS_EQ_INT(lr20xx_set_rf_frequency(915000000u), ESP_OK);
    LS_CHECK(lr20xx_tx_allowed());
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_set_tx(), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(lr20xx_set_pa_lf(10), ESP_OK);
    LS_EQ_INT(lr20xx_set_tx(), ESP_OK);
    LS_EQ_UINT(fk.tx_freq_hz, 915000000u);

    /* The Mode S session's 1090 MHz is receive only, at every level. */
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(lr20xx_set_tx(), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);

    LS_EQ_INT(fk.tx_starts, 2);
    no_hf_transmit();
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(the_dcdc_workaround_takes_the_slower_settings_for_the_narrow_decimations)
{
    session_up();
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    LS_EQ_INT(lr20xx_set_rf_frequency(915000000u), ESP_OK);

    fk.dcdc_adc = 1u << 8;                                 /* ana_dec 1 */
    LS_EQ_INT(lr20xx_dcdc_workaround_set(), ESP_OK);
    LS_EQ_UINT(fk.dcdc_sw & 0x00FF0000u, (11u << 20) | (13u << 16));
    LS_EQ_UINT(fk.dcdc_freq_lf, 4508876u);
    LS_EQ_UINT(fk.last_freq_hz, 915000000u);               /* the frequency set again after it */

    fk.dcdc_adc = 2u << 8;
    LS_EQ_INT(lr20xx_dcdc_workaround_set(), ESP_OK);
    LS_EQ_UINT(fk.dcdc_sw & 0x00FF0000u, (11u << 20) | (13u << 16));
    LS_EQ_UINT(fk.dcdc_freq_lf, 2936012u);

    fk.dcdc_adc = 3u << 8;
    LS_EQ_INT(lr20xx_dcdc_workaround_set(), ESP_OK);
    LS_EQ_UINT(fk.dcdc_sw & 0x00FF0000u, (15u << 20) | (15u << 16));

    fk.dcdc_adc = 1u << 8;
    LS_EQ_INT(lr20xx_dcdc_workaround_set(), ESP_OK);
    LS_EQ_INT(lr20xx_dcdc_workaround_reset(), ESP_OK);
    LS_EQ_UINT(fk.dcdc_sw & 0x00FF0000u, (15u << 20) | (15u << 16));
    LS_EQ_UINT(fk.dcdc_freq_lf, 2936012u);
    LS_EQ_INT(fk.bad_reg, 0);
    LS_EQ_INT(fk.bad_args, 0);
}

LS_CASE(fsk_bandwidths_come_from_the_lr2021_table_only_while_an_lr20xx_is_bound)
{
    /* The table: ascending, and every code is 40 MHz / (D * M * 2^E). */
    static const uint32_t D[4] = { 13, 14, 15, 18 };
    LS_EQ_INT(lr20xx_fsk_rx_bw_table_n, 97);
    LS_EQ_UINT(lr20xx_fsk_rx_bw_table[0].hz, 3472);   LS_EQ_UINT(lr20xx_fsk_rx_bw_table[0].index, 231);
    for (int i = 0; i < lr20xx_fsk_rx_bw_table_n; i++) {
        const uint8_t c = lr20xx_fsk_rx_bw_table[i].index;
        const double hz = 40e6 / ((double)D[c >> 6] * (((c >> 3) & 7) + 1) * (double)(1u << (c & 7)));
        LS_CHECK_MSG(fabs(hz - lr20xx_fsk_rx_bw_table[i].hz) <= 1.0,
                     "rung %d: code %u is %.1f Hz, table says %lu", i, (unsigned)c, hz,
                     (unsigned long)lr20xx_fsk_rx_bw_table[i].hz);
        if (i) LS_CHECK(lr20xx_fsk_rx_bw_table[i].hz > lr20xx_fsk_rx_bw_table[i - 1].hz);
    }
    /* RadioLib's own rungs are in it with RadioLib's codes. */
    LS_EQ_UINT(lr20xx_fsk_rx_bw(4808).index, 39);
    LS_EQ_UINT(lr20xx_fsk_rx_bw(12019).index, 30);
    LS_EQ_UINT(lr20xx_fsk_rx_bw(23148).index, 213);
    LS_EQ_UINT(lr20xx_fsk_rx_bw(1111111).index, 200);
    /* The POCSAG filter: Carson for 1200 baud at 4.5 kHz is 11.7 kHz. */
    LS_EQ_UINT(lr20xx_fsk_rx_bw(11700).hz, 12019);
    LS_EQ_UINT(lr20xx_fsk_rx_bw(153846).index, 34);
    LS_EQ_UINT(lr20xx_fsk_rx_bw(3076923).index, 0);

    session_up();
    LS_EQ_UINT(ls_lora_fsk_bw_snap(19500), 20833);         /* at or above, never narrower */
    LS_EQ_UINT(ls_lora_fsk_bw_snap(19231), 19231);
    LS_EQ_UINT(ls_lora_fsk_bw_snap(1), 3472);
    LS_EQ_UINT(ls_lora_fsk_bw_snap(467000), 476190);
    LS_EQ_UINT(ls_lora_fsk_bw_snap(9000000), 3076923);

    /* With nothing bound, or an SX126x, the SX126x ladder as before. */
    ls_lora_stop();
    LS_EQ_UINT(ls_lora_fsk_bw_snap(19500), 19500);
    LS_EQ_UINT(ls_lora_fsk_bw_snap(4800), 4800);
    LS_EQ_UINT(ls_lora_fsk_bw_snap(9000000), 467000);
    fk_install(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_UINT(ls_lora_fsk_bw_snap(19000), 19500);
}

LS_CASE(an_fsk_session_programs_gfsk_from_every_field_of_its_config)
{
    lora_up(NULL);                                         /* the mesh was listening */
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    LS_CHECK(ls_lora_fsk_active());
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_UINT(fk.packet_type, 0x02);
    LS_EQ_UINT(fk.last_freq_hz, 868000000u);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* 2400 bps, no shaping, the 20.8 kHz rung (19.5 kHz asked), 4800 Hz. */
    static const uint8_t mod[9] = { 0x00, 0x00, 0x09, 0x60, 0x00, 0x9D, 0x00, 0x12, 0xC0 };
    LS_CHECK(memcmp(fk.gfsk_mod, mod, 9) == 0);
    /* 64 preamble bits, detector off, fixed, 8 bytes, no CRC, no whitening. */
    static const uint8_t pkt[7] = { 0x00, 0x40, 0x00, 0x00, 0x00, 0x08, 0x00 };
    LS_CHECK(memcmp(fk.gfsk_pkt, pkt, 7) == 0);
    /* All 32 bits of the sync word, right-aligned, MSB first. */
    static const uint8_t sync[9] = { 0, 0, 0, 0, 0x7C, 0xD2, 0x15, 0xD8, 0xA0 };
    LS_CHECK(memcmp(fk.gfsk_sync, sync, 9) == 0);
    /* 10 dBm on the LF PA. */
    LS_EQ_UINT(fk.pa_cfg[0], 0x00); LS_EQ_UINT(fk.pa_cfg[1], 0x21);
    LS_EQ_UINT(fk.tx_params[0], 38);

    /* Ending it gives LoRa back, listening, as it was. */
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_CHECK(!ls_lora_fsk_active());
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_EQ_UINT(fk.lora_mod[0], 0x73);
    LS_EQ_UINT(fk.last_freq_hz, 910525000u);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(ls_lora_is_receiving());

    /* The defaults: 32 preamble bits; and eight sync bits are the top byte. */
    ls_fsk_cfg_t f = FSK_868;
    f.preamble_bits = 0; f.sync_bits = 8;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
    LS_EQ_UINT(fk.gfsk_pkt[0], 0x00); LS_EQ_UINT(fk.gfsk_pkt[1], 0x20);
    static const uint8_t sync8[9] = { 0, 0, 0, 0, 0, 0, 0, 0x7C, 0x88 };
    LS_CHECK(memcmp(fk.gfsk_sync, sync8, 9) == 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);

    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.bad_reg, 0);
    LS_EQ_INT(fk.tx_starts, 0);
    no_hf_transmit();
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(an_fsk_session_takes_pocsag_at_512_baud)
{
    lora_up(NULL);
    ls_fsk_cfg_t f = FSK_868;
    f.bitrate = 512;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
}

LS_CASE(an_fsk_session_hears_uat_at_978_mhz_and_never_transmits_there)
{
    lora_up(NULL);
    ls_fsk_cfg_t f = FSK_868;
    f.freq_hz = 978000000u;
    f.bitrate = 1041667u;
    f.deviation_hz = 312500u;
    f.bandwidth_hz = 2222222u;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
    LS_EQ_UINT(fk.last_freq_hz, 978000000u);
    static const uint8_t frame[4] = { 1, 2, 3, 4 };
    LS_EQ_INT(ls_lora_fsk_send(frame, sizeof(frame)), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(fk.tx_starts, 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    no_hf_transmit();
}

LS_CASE(an_fsk_session_refuses_what_the_sx126x_backend_refuses_and_holds_the_part)
{
    lora_up(NULL);
    ls_fsk_cfg_t f;
    f = FSK_868; f.freq_hz = 1100000001u;     LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.freq_hz = 149999999u;      LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.bitrate = 499;             LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.bandwidth_hz = 9000;       LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);  /* 2400 + 9600 */
    f = FSK_868; f.bandwidth_hz = 4000000;    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.sync_bits = 12;            LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.sync_bits = 40;            LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.preamble_bits = 4;         LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.power_dbm = 23;            LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_868; f.payload_bytes = 0;         LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    LS_CHECK(!ls_lora_fsk_active());

    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    fk.nframes = 0;
    const uint8_t msg[2] = { 1, 2 };
    uint8_t buf[16]; float r, s;
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_ERR_INVALID_STATE);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_receive(), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk.nframes, 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
}

LS_CASE(an_fsk_session_receives_fixed_length_frames_and_transmits_on_the_lf_pa)
{
    session_up();                                          /* no LoRa underneath */
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);

    uint8_t f[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    fk_push_packet(f, 8);
    fk.gfsk_rssi_avg = 101; fk.gfsk_rssi_flags = 0x04;     /* RssiAvg(0) is bit 2: -101.5 */
    uint8_t buf[16]; float rssi = 0;
    LS_EQ_INT(ls_lora_fsk_poll(buf, sizeof(buf), &rssi), 8);
    LS_CHECK(memcmp(buf, f, 8) == 0);
    LS_NEAR(rssi, -101.5, 0.001);
    LS_EQ_INT(ls_lora_fsk_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_INT(ls_lora_fsk_poll(buf, 7, &rssi), -1);         /* too small for the session */

    /* A length that is not the session's is an error, and the FIFO starts over. */
    fk_push_packet(f, 5);
    LS_EQ_INT(ls_lora_fsk_poll(buf, sizeof(buf), &rssi), -1);
    LS_EQ_INT(fk.fifo_n, 0);

    fk.nframes = 0;
    const uint8_t msg[3] = { 0x55, 0x66, 0x77 };
    LS_EQ_INT(ls_lora_fsk_send(msg, sizeof(msg)), ESP_OK);
    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 6);
    CMD(0, 0x01, 0x28, 0x00);
    CMD(1, 0x01, 0x16, 0xFF, 0xFF, 0xFF, 0xFF);
    CMD(2, 0x02, 0x41, 0x00, 0x40, 0x00, 0x00, 0x00, 0x03, 0x00);   /* this payload's length */
    CMD(3, 0x01, 0x1F);
    CMD(4, 0x00, 0x02, 0x55, 0x66, 0x77);
    CMD(5, 0x02, 0x0D, 0x00, 0x00, 0x00);
    LS_EQ_UINT(fk.tx_freq_hz, 868000000u);
    LS_EQ_INT(ls_lora_fsk_send(msg, sizeof(msg)), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_fsk_receive(), ESP_ERR_INVALID_STATE);

    fk_tx_complete();
    LS_CHECK(ls_lora_send_done());
    LS_EQ_INT(ls_lora_fsk_receive(), ESP_OK);
    LS_EQ_UINT(fk.gfsk_pkt[5], 8);                          /* the session's length again */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(ls_lora_fsk_receive(), ESP_OK);               /* and again, already listening */

    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);               /* nothing to give back */
    LS_CHECK(ls_lora_cfg() == NULL);
    LS_EQ_INT(fk.bad_mode, 0);
    no_hf_transmit();
}

LS_CASE(a_sweep_fills_every_bin_and_gives_lora_back_as_it_found_it)
{
    lora_up(NULL);
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_OK);
    LS_CHECK(ls_lora_scanning());
    LS_CHECK(!ls_lora_is_receiving());
    LS_EQ_UINT(fk.packet_type, 0x02);
    /* 64 bins over 26 MHz are 413 kHz apart: this part's own 444.4 kHz rung
       covers them in one look. */
    LS_EQ_UINT(fk.gfsk_mod[5], 224);
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_OK);     /* the same band: nothing */

    fk.rssi_hi = 75; fk.rssi_lo = 0;                       /* -75 dBm everywhere */
    float bins[LS_LORA_SCAN_BINS];
    for (int i = 0; i < LS_LORA_SCAN_BINS; i++) bins[i] = 0;
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_scan_sweep(bins, LS_LORA_SCAN_BINS), LS_LORA_SCAN_BINS);
    for (int i = 0; i < LS_LORA_SCAN_BINS; i++) LS_NEAR(bins[i], -75.0, 0.001);
    LS_EQ_INT(count_frames_with(0x02, 0x0C), LS_LORA_SCAN_BINS);       /* one SetRx a bin   */
    LS_EQ_INT(count_frames_with(0x01, 0x23), 1);           /* CalibFE again at 922 MHz, once */
    LS_EQ_INT(fk.bad_mode, 0);
    ls_lora_scan_prof_t p;
    ls_lora_scan_profile(&p);
    LS_CHECK(p.standby_us > 0 && p.tune_us > 0 && p.rx_us > 0 && p.rssi_us > 0);

    /* A different bin count is planned again; the waterfall's passes are bounded. */
    float few[8];
    LS_EQ_INT(ls_lora_scan_sweep(few, 8), 8);
    bool done = false;
    int passes = 0;
    while (!done && passes < 200) {
        LS_EQ_INT(ls_lora_scan_pass(bins, LS_LORA_SCAN_BINS, &done), LS_LORA_SCAN_BINS);
        passes++;
    }
    LS_CHECK(done);
    LS_CHECK(passes > 1);

    /* A read that does not come from a receiving part is not a measurement. */
    fk.bad_read_stat = true;
    LS_EQ_INT(ls_lora_scan_pass(bins, LS_LORA_SCAN_BINS, &done), 0);
    fk.bad_read_stat = false;

    LS_EQ_INT(ls_lora_scan_end(), ESP_OK);
    LS_CHECK(!ls_lora_scanning());
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_EQ_UINT(fk.lora_mod[0], 0x73);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.tx_starts, 0);
    LS_EQ_INT(fk.violations, 0);
    no_hf_transmit();
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(a_sweep_refuses_what_the_sx126x_backend_refuses)
{
    session_up();
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_ERR_INVALID_STATE);  /* no LoRa yet */
    lora_up(NULL);
    LS_EQ_INT(ls_lora_scan_begin(928000000u, 902000000u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_scan_begin(100000000u, 200000000u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 1100000001u), ESP_ERR_INVALID_ARG);
    /* One input or the other, never across the gap between them. */
    LS_EQ_INT(ls_lora_scan_begin(1000000000u, 1600000000u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_scan_begin(1499999999u, 1600000000u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_scan_begin(2400000000u, 2500000001u), ESP_ERR_INVALID_ARG);
    const uint8_t msg[2] = { 1, 2 };
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_OK);
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_ERR_INVALID_STATE);  /* mid-transmit */
    fk_tx_complete();
    LS_CHECK(ls_lora_send_done());
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_OK);
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_ERR_INVALID_STATE);                        /* mid-sweep */
    LS_EQ_INT(ls_lora_scan_end(), ESP_OK);
    LS_CHECK(!ls_lora_is_receiving());                     /* it was idle after the send */
}

/* The SetRxPath byte of the last one sent: 0 LF, 1 HF; -1 for none. */
static int last_rx_path(int *boost)
{
    for (int i = fk.nframes - 1; i >= 0; i--)
        if (!fk.frames[i].read2 && fk.frames[i].b[0] == 0x02 && fk.frames[i].b[1] == 0x01) {
            if (boost) *boost = fk.frames[i].b[3];
            return fk.frames[i].b[2];
        }
    return -1;
}

/* CalibFE frames for the HF input (bit 15 of the first word) and the LF one. */
static void calib_fe_paths(int *hf, int *lf)
{
    *hf = *lf = 0;
    for (int i = 0; i < fk.nframes; i++)
        if (!fk.frames[i].read2 && fk.frames[i].b[0] == 0x01 && fk.frames[i].b[1] == 0x23) {
            if (fk.frames[i].b[2] & 0x80) (*hf)++;
            else (*lf)++;
        }
}

LS_CASE(the_sweep_plan_uses_the_lr2021_rungs_and_few_looks_on_a_wide_band)
{
    ls_lora_scan_plan_t p;
    lr20xx_rx_bw_t r;
    lr20xx_scan_plan(902000000u, 928000000u, 64, &p, &r);
    LS_EQ_UINT(p.bw_hz, 444444); LS_EQ_UINT(r.index, 224); LS_EQ_INT(p.looks, 1);
    LS_EQ_UINT(p.settle_us, 338);                         /* 300 us held in filter samples */
    /* The mesh watch: 2 MHz in 64 bins is 31.7 kHz apart, the 32.1 kHz rung. */
    lr20xx_scan_plan(909500000u, 911500000u, 64, &p, &r);
    LS_EQ_UINT(p.bw_hz, 32051); LS_EQ_UINT(r.index, 21); LS_EQ_INT(p.looks, 1);
    /* All of the LF input: the widest rung, five looks a bin, not thirty. */
    lr20xx_scan_plan(150000000u, 1100000000u, 64, &p, &r);
    LS_EQ_UINT(p.bw_hz, 3076923); LS_EQ_UINT(r.index, 0); LS_EQ_INT(p.looks, 5);
    LS_EQ_UINT(p.settle_us, 300);                         /* no shorter for a wider filter */
    lr20xx_scan_plan(1500000000u, 2500000000u, 64, &p, &r);
    LS_EQ_UINT(p.bw_hz, 3076923); LS_EQ_INT(p.looks, 6);
    /* 2.4 GHz ISM, 83.5 MHz: 1.33 MHz apart, the 1.33 MHz rung. */
    lr20xx_scan_plan(2400000000u, 2483500000u, 64, &p, &r);
    LS_EQ_UINT(p.bw_hz, 1333333); LS_EQ_INT(p.looks, 1);
    /* Narrow: never below the SX126x's narrowest, whatever the spacing. */
    lr20xx_scan_plan(915000000u, 915120000u, 64, &p, &r);
    LS_EQ_UINT(p.bw_hz, 8013); LS_EQ_INT(p.looks, 1);
    /* A single bin, or no band: the 500 kHz rung and one look, as before. */
    lr20xx_scan_plan(915000000u, 915000000u, 64, &p, NULL);
    LS_EQ_UINT(p.bw_hz, 512821); LS_EQ_INT(p.looks, 1);
    lr20xx_scan_plan(902000000u, 928000000u, 1, &p, NULL);
    LS_EQ_UINT(p.bw_hz, 512821); LS_EQ_INT(p.looks, 1);
    /* A band too wide even for the widest rung is capped in looks. */
    lr20xx_scan_plan(150000000u, 1100000000u, 4, &p, NULL);
    LS_EQ_INT(p.looks, LS_LORA_SCAN_LOOKS_MAX);
    lr20xx_scan_plan(150000000u, 1100000000u, 64, NULL, NULL);   /* no crash */
}

LS_CASE(a_sweep_reaches_1100_mhz_on_the_lf_input)
{
    lora_up(NULL);
    LS_CHECK(ls_lora_caps() & LS_LORA_CAP_RX_WIDE);
    LS_EQ_INT(ls_lora_scan_begin(960000000u, 1100000000u), ESP_OK);
    int boost = -1;
    LS_EQ_INT(last_rx_path(&boost), 0);
    LS_EQ_INT(boost, 0);
    float bins[LS_LORA_SCAN_BINS];
    fk.rssi_hi = 80; fk.rssi_lo = 0;                       /* -80 dBm */
    LS_EQ_INT(ls_lora_scan_sweep(bins, LS_LORA_SCAN_BINS), LS_LORA_SCAN_BINS);
    LS_NEAR(bins[63], -80.0, 0.001);
    int hf, lf;
    calib_fe_paths(&hf, &lf);
    LS_EQ_INT(hf, 0);
    LS_CHECK(lf > 0);
    LS_EQ_INT(ls_lora_scan_end(), ESP_OK);
    LS_EQ_UINT(fk.last_freq_hz, 910525000u);
    LS_EQ_INT(fk.tx_starts, 0);
    no_hf_transmit();
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(a_sweep_of_the_hf_input_receives_there_and_gives_lora_back_on_lf)
{
    lora_up(NULL);
    LS_EQ_INT(ls_lora_scan_begin(2400000000u, 2483500000u), ESP_OK);
    int boost = -1;
    LS_EQ_INT(last_rx_path(&boost), 1);                    /* SetRxPath HF        */
    LS_EQ_INT(boost, 4);                                   /* its default boost   */
    /* The HF input takes the plain DC-DC switcher settings whatever the
       decimation. */
    LS_EQ_UINT(fk.dcdc_sw & 0x00FF0000u, (15u << 20) | (15u << 16));
    float bins[LS_LORA_SCAN_BINS];
    fk.rssi_hi = 90; fk.rssi_lo = 0;                       /* -90 dBm */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_scan_sweep(bins, LS_LORA_SCAN_BINS), LS_LORA_SCAN_BINS);
    for (int i = 0; i < LS_LORA_SCAN_BINS; i++) LS_NEAR(bins[i], -90.0, 0.001);
    LS_EQ_UINT(fk.last_freq_hz, 2483500000u);
    int hf, lf;
    calib_fe_paths(&hf, &lf);
    LS_CHECK(hf > 0);                                      /* CalibFE with the HF bit */
    LS_EQ_INT(lf, 0);
    LS_EQ_INT(fk.bad_mode, 0);

    LS_EQ_INT(ls_lora_scan_end(), ESP_OK);
    LS_EQ_INT(last_rx_path(NULL), 0);                      /* LoRa is on LF again */
    LS_EQ_UINT(fk.last_freq_hz, 910525000u);
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_INT(fk.tx_starts, 0);
    no_hf_transmit();
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(an_lr2012_or_lr2022_sweeps_the_lf_input_only)
{
    fk_install(FK_LR);
    fk.fw_major = 2; fk.fw_minor = 0;
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t c; ls_lora_cfg_default(&c);
    LS_EQ_INT(ls_lora_configure(&c), ESP_OK);
    LS_EQ_INT(ls_lora_scan_begin(2400000000u, 2483500000u), ESP_ERR_INVALID_ARG);
    ls_fsk_cfg_t f = FSK_868; f.freq_hz = 1626000000u;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_scan_begin(1000000000u, 1100000000u), ESP_OK);
    LS_EQ_INT(ls_lora_scan_end(), ESP_OK);
}

LS_CASE(an_fsk_listener_on_the_hf_input_hops_in_place_and_never_transmits)
{
    lora_up(NULL);
    ls_fsk_cfg_t f = FSK_868;
    f.freq_hz = 1626000000u;
    f.bandwidth_hz = 41667;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
    LS_EQ_INT(last_rx_path(NULL), 1);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    static const uint8_t frame[4] = { 1, 2, 3, 4 };
    LS_EQ_INT(ls_lora_fsk_send(frame, sizeof(frame)), ESP_ERR_NOT_SUPPORTED);

    /* A hop inside the 4 MHz calibration step: standby, the frequency, Rx. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_retune(1626270833u), ESP_OK);
    LS_EQ_UINT(fk.last_freq_hz, 1626270833u);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(count_frames_with(0x01, 0x23), 0);
    LS_EQ_INT(count_frames_with(0x02, 0x40), 0);          /* the modulation stays */
    LS_EQ_INT(count_frames_with(0x02, 0x0C), 1);
    float dbm;
    LS_EQ_INT(ls_lora_rssi_inst(&dbm), ESP_OK);

    /* A long hop calibrates the front end again, with the HF bit. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_retune(1575420000u), ESP_OK);
    int hf, lf;
    calib_fe_paths(&hf, &lf);
    LS_EQ_INT(hf, 1);
    LS_EQ_INT(lf, 0);

    /* Not to the other input, and not past either end. */
    LS_EQ_INT(ls_lora_fsk_retune(915000000u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_fsk_retune(2500000001u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_fsk_retune(1300000000u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_fsk_send(frame, sizeof(frame)), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_retune(1626000000u), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(last_rx_path(NULL), 0);
    LS_EQ_INT(fk.tx_starts, 0);
    no_hf_transmit();
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(an_lf_listener_hops_on_lf_and_still_transmits_only_below_960_mhz)
{
    lora_up(NULL);
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_retune(1090000000u), ESP_OK);   /* listening only up there */
    static const uint8_t frame[4] = { 1, 2, 3, 4 };
    LS_EQ_INT(ls_lora_fsk_send(frame, sizeof(frame)), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(ls_lora_fsk_retune(1626000000u), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(ls_lora_fsk_retune(869000000u), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(fk.tx_hf, 0);
    no_hf_transmit();
}

LS_CASE(mode_s_takes_the_part_from_lora_and_gives_it_back_reprogrammed)
{
    lora_up(NULL);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_CHECK(ls_lora_modes_active());
    LS_EQ_UINT(fk.packet_type, 0x0A);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(!ls_lora_is_receiving());
    LS_CHECK(ls_lora_cfg() != NULL);                       /* kept for afterwards */

    /* LoRa waits while the session runs, and touches nothing. */
    fk.nframes = 0;
    const uint8_t msg[2] = { 1, 2 };
    uint8_t buf[28]; float r, s;
    LS_EQ_INT(ls_lora_receive(), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_scan_begin(902000000u, 928000000u), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk.nframes, 0);

    uint8_t f[28]; frame_k(1, f);
    fk_push_frame(f);
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &r), 28);

    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    LS_CHECK(!ls_lora_modes_active());
    LS_EQ_INT(count_frames_with(0x02, 0x07), 1);           /* SetPacketType, and it was LoRa */
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_EQ_UINT(fk.lora_mod[0], 0x73);
    LS_EQ_UINT(fk.lora_sync, 0x12);
    LS_EQ_UINT(fk.last_freq_hz, 910525000u);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(ls_lora_is_receiving());

    /* And the mesh may transmit again, on the LoRa frequency. */
    LS_EQ_INT(ls_lora_send(msg, 2), ESP_OK);
    LS_EQ_UINT(fk.tx_freq_hz, 910525000u);
    LS_EQ_INT(fk.bad_mode, 0);
    no_hf_transmit();
}

LS_CASE(a_failed_hand_back_from_mode_s_is_finished_by_the_next_poll)
{
    lora_up(NULL);
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.fail_op = 0x0220;                                   /* the LoRa modulation refused */
    LS_CHECK(ls_lora_modes_end() != ESP_OK);
    LS_CHECK(!ls_lora_modes_active());
    LS_CHECK(ls_lora_cfg() != NULL);                       /* the configuration survives */
    fk.fail_op = 0;
    uint8_t buf[16]; float r, s;
    LS_EQ_INT(ls_lora_poll(buf, sizeof(buf), &r, &s), 0);
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_EQ_UINT(fk.lora_mod[0], 0x73);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(ls_lora_is_receiving());
}

LS_CASE(mode_s_over_an_idle_lora_configuration_leaves_it_idle_afterwards)
{
    session_up();
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);            /* configured, never listening */
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);
    LS_CHECK(!ls_lora_is_receiving());
    /* An argument Mode S refuses leaves LoRa exactly as it was. */
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_modes_begin(868000000u, 13), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    LS_CHECK(ls_lora_is_receiving());
}

/* ---- the OOK session with the numbers left open ---------------------------- */

static const ls_ook_cfg_t OOK_315 = {
    .freq_hz = 315000000u, .bitrate = 40000, .bandwidth_hz = 200000, .pattern = 0x000E,
    .pattern_bits = 4, .repeats = 0, .frame_bytes = 252, .gain_step = 0, .boost = -1,
};

LS_CASE(an_ook_detector_pattern_the_part_refuses_is_refused_before_any_frame)
{
    session_up();
    /* Measured on the LR2021: an odd length, or two equal first chips, is a
       parameter error from the part. */
    static const struct { uint16_t p; uint8_t bits; } bad[] = {
        { 0x0F00, 12 }, { 0x0003, 16 }, { 0x0006, 3 }, { 0x7F00, 15 }, { 0x0001, 1 },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ls_ook_cfg_t c = OOK_315;
        c.pattern = bad[i].p;
        c.pattern_bits = bad[i].bits;
        LS_CHECK_MSG(ls_lora_ook_begin(&c) != ESP_OK, "pattern %04X/%u taken", bad[i].p, bad[i].bits);
        LS_CHECK(!ls_lora_ook_active());
    }
    LS_EQ_INT(ls_lora_ook_begin(&OOK_315), ESP_OK);
    LS_EQ_INT(ls_lora_ook_end(), ESP_OK);
}

LS_CASE(an_ook_session_programs_the_rate_filter_pattern_and_frame_it_is_given)
{
    session_up();
    LS_CHECK(!ls_lora_ook_active());
    LS_EQ_INT(ls_lora_ook_begin(&OOK_315), ESP_OK);
    LS_CHECK(ls_lora_ook_active());
    LS_CHECK(!ls_lora_modes_active());       /* it is not Mode S */

    int ix[80];
    const int n = cmds(ix, 80);
    LS_EQ_INT(n, 26);                        /* the Mode S sequence, frame for frame */
    CMD(0,  0x01, 0x28, 0x00);                                /* SetStandby, RC         */
    CMD(14, 0x02, 0x07, 0x0A);                                /* SetPacketType OOK      */
    CMD(16, 0x02, 0x00, 0x12, 0xC6, 0x84, 0xC0);              /* SetRfFrequency 315 MHz */
    CMD(17, 0x02, 0x01, 0x00, 0x00);                          /* LF, the path's default boost */
    {
        const lr20xx_rx_bw_t bw = lr20xx_fsk_rx_bw(200000);
        LS_CHECK(bw.hz >= 200000);
        const uint8_t mod[] = { 0x02, 0x81, 0x00, 0x00, 0x9C, 0x40, 0x00, bw.index, 0x00 };  /* 40000 bps */
        LS_CHECK(fk_frame_is(ix[18], mod, sizeof(mod)));
    }
    CMD(19, 0x02, 0x82, 0x00, 0x10, 0x00, 0x00, 0xFC, 0x00);  /* fixed, 252 bytes      */
    CMD(20, 0x02, 0x84, 0x00, 0x00, 0x00, 0x00, 0x80);        /* no sync word           */
    CMD(21, 0x02, 0x88, 0x00, 0x0E, 0x03, 0x00, 0x00);        /* pattern 0x000E, 4 bits */
    CMD(22, 0x02, 0x1A, 0x00);                                /* AGC                    */
    CMD(25, 0x02, 0x0C, 0xFF, 0xFF, 0xFF);                    /* SetRx, continuous      */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_UINT(fk.last_freq_hz, 315000000u);
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.tx_frames, 0);
    LS_CHECK(!tx_opcode_seen());
    LS_EQ_INT(unknown_frames(), 0);

    /* A frame of the size asked for comes back whole, with its level. */
    uint8_t want[252];
    for (int i = 0; i < 252; i++) want[i] = (uint8_t)(i * 7 + 3);
    fk_push_packet(want, 252);
    fk.ook_rssi_high = 75; fk.ook_rssi_flags = 0x01;         /* RssiHigh 151: -75.5 dBm */
    static uint8_t buf[256];
    float rssi = 0;
    LS_EQ_INT(ls_lora_ook_poll(buf, 251, &rssi), -1);         /* too small a buffer */
    LS_EQ_INT(ls_lora_ook_poll(buf, sizeof(buf), &rssi), 252);
    LS_CHECK(memcmp(buf, want, 252) == 0);
    LS_NEAR(rssi, -75.5, 0.001);
    LS_EQ_INT(ls_lora_ook_poll(buf, sizeof(buf), &rssi), 0);

    /* The Mode S calls leave it alone; its own end puts the part down. */
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), -1);
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    LS_CHECK(ls_lora_ook_active());
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_ook_begin(&OOK_315), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_ook_end(), ESP_OK);
    LS_CHECK(!ls_lora_ook_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_STBY_RC);
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(an_ook_session_refuses_what_the_engine_cannot_do_and_sends_nothing)
{
    session_up();
    ls_ook_cfg_t c;
#define REFUSED(field, value)                                                  \
    do {                                                                        \
        c = OOK_315;                                                            \
        c.field = value;                                                        \
        LS_CHECK_MSG(ls_lora_ook_begin(&c) == ESP_ERR_INVALID_ARG, #field " = " #value " taken"); \
    } while (0)
    REFUSED(freq_hz, 149000000u);
    REFUSED(freq_hz, 1213000001u);
    REFUSED(bitrate, 599);
    REFUSED(bitrate, 2000001);
    REFUSED(bandwidth_hz, 0);
    REFUSED(bandwidth_hz, 4000000);
    REFUSED(pattern_bits, 0);
    REFUSED(pattern_bits, 17);
    REFUSED(repeats, 32);
    REFUSED(frame_bytes, 0);
    REFUSED(frame_bytes, 253);
    REFUSED(gain_step, 14);
    REFUSED(boost, 8);
#undef REFUSED
    LS_EQ_INT(ls_lora_ook_begin(NULL), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    LS_CHECK(!ls_lora_ook_active());

    /* Mode S holds the part: no OOK session beside it. */
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_ook_begin(&OOK_315), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_ook_end(), ESP_OK);                    /* not its session to end */
    LS_CHECK(ls_lora_modes_active());
}

LS_CASE(an_ook_session_over_lora_gives_it_back_listening)
{
    lora_up(NULL);
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_INT(ls_lora_ook_begin(&OOK_315), ESP_OK);
    LS_CHECK(!ls_lora_is_receiving());
    LS_EQ_INT(ls_lora_ook_end(), ESP_OK);
    LS_EQ_UINT(fk.packet_type, 0x00);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(ls_lora_is_receiving());
}
LS_CASE(fsk_sync_rssi_is_latched_status_and_average_remains_the_default)
{
    session_up();
    ls_fsk_cfg_t cfg = FSK_868;
    cfg.rssi_at_sync = true;
    LS_EQ_INT(ls_lora_fsk_begin(&cfg), ESP_OK);
    uint8_t frame[8] = { 0 }, out[8];
    fk_push_packet(frame, 8);
    fk.gfsk_rssi_avg = 76;
    fk.gfsk_rssi_flags = 0x01; /* Sync has the half dB bit, average does not. */
    float rssi = 0;
    LS_EQ_INT(ls_lora_fsk_poll(out, sizeof(out), &rssi), 8);
    LS_NEAR(rssi, -76.5, 0.001);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    cfg.rssi_at_sync = false;
    LS_EQ_INT(ls_lora_fsk_begin(&cfg), ESP_OK);
    fk_push_packet(frame, 8);
    fk.gfsk_rssi_flags = 0x01;
    LS_EQ_INT(ls_lora_fsk_poll(out, sizeof(out), &rssi), 8);
    LS_NEAR(rssi, -76.0, 0.001);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(fk.tx_starts, 0);
    LS_EQ_INT(unknown_frames(), 0);
}
/* Run ls_lora_diagnostics() with stdout going to `out`. */
static void diagnostics_text(char *out, size_t n)
{
    FILE *tmp = ls_test_tmpfile();
    LS_CHECK(tmp != NULL);
    fflush(stdout);
    const int saved = dup(1);
    dup2(fileno(tmp), 1);
    ls_lora_diagnostics();
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    rewind(tmp);
    const size_t got = fread(out, 1, n - 1, tmp);
    out[got] = 0;
    fclose(tmp);
}

LS_CASE(fsk_preamble_detector_is_off_by_default_and_packs_the_length_in_bits)
{
    session_up();
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    static const uint8_t off[7] = { 0x00, 0x40, 0x00, 0x00, 0x00, 0x08, 0x00 };
    LS_CHECK(memcmp(fk.gfsk_pkt, off, 7) == 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);

    /* The third byte after the opcode, behind the 2-byte preamble, is the
       length itself and nothing else in the frame moves. */
    static const uint8_t bits[] = { 8, 16, 24, 32 };
    for (unsigned i = 0; i < sizeof(bits); i++) {
        ls_fsk_cfg_t f = FSK_868;
        f.preamble_detect_bits = bits[i];
        LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
        const uint8_t want[7] = { 0x00, 0x40, bits[i], 0x00, 0x00, 0x08, 0x00 };
        LS_CHECK(memcmp(fk.gfsk_pkt, want, 7) == 0);
        LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
        LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    }
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(fsk_preamble_detector_refuses_other_lengths_before_touching_the_part)
{
    lora_up(NULL);
    static const uint8_t bad[] = { 1, 4, 7, 12, 31, 33, 40, 64, 255 };
    for (unsigned i = 0; i < sizeof(bad); i++) {
        ls_fsk_cfg_t f = FSK_868;
        f.preamble_detect_bits = bad[i];
        LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    }
    LS_EQ_INT(fk.nframes, 0);
    LS_CHECK(!ls_lora_fsk_active());
    LS_EQ_INT(lr20xx_gfsk_set_packet_fixed(64, 9, 8), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_gfsk_set_packet_fixed(64, 40, 8), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
}

LS_CASE(fsk_preamble_detector_is_receive_only_and_survives_a_restart)
{
    session_up();
    ls_fsk_cfg_t f = FSK_868;
    f.preamble_detect_bits = 16;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
    LS_EQ_UINT(fk.gfsk_pkt[2], 16);

    /* A transmit goes out with the detector off, and listening again puts it back. */
    const uint8_t msg[3] = { 0x55, 0x66, 0x77 };
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_send(msg, sizeof(msg)), ESP_OK);
    int ix[16];
    LS_EQ_INT(cmds(ix, 16), 6);
    CMD(2, 0x02, 0x41, 0x00, 0x40, 0x00, 0x00, 0x00, 0x03, 0x00);
    fk_tx_complete();
    LS_CHECK(ls_lora_send_done());
    LS_EQ_INT(ls_lora_fsk_receive(), ESP_OK);
    LS_EQ_UINT(fk.gfsk_pkt[2], 16);
    LS_EQ_UINT(fk.gfsk_pkt[5], 8);

    /* The part restarts under the session: the poll programs it again, with the detector. */
    memset(fk.gfsk_pkt, 0, sizeof(fk.gfsk_pkt));
    fk.reset_src = 2;
    uint8_t buf[16]; float rssi = 0;
    LS_EQ_INT(ls_lora_fsk_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_UINT(fk.gfsk_pkt[2], 16);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* A session after it that does not ask is off again. */
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    LS_EQ_UINT(fk.gfsk_pkt[2], 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    no_hf_transmit();
}

LS_CASE(fsk_rx_stats_are_unpacked_and_diagnostics_only_reads_them)
{
    session_up();
    static const uint8_t raw[14] = { 0x01, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05,
                                     0x00, 0x06, 0x00, 0x07, 0xFF, 0xFE };
    memcpy(fk.gfsk_stats, raw, sizeof(raw));
    lr20xx_gfsk_rx_stats_t st;
    LS_EQ_INT(lr20xx_get_gfsk_rx_stats(&st), ESP_OK);
    LS_EQ_UINT(st.received, 0x0102);   LS_EQ_UINT(st.crc_errors, 3);
    LS_EQ_UINT(st.length_errors, 4);   LS_EQ_UINT(st.preamble_detections, 5);
    LS_EQ_UINT(st.sync_ok, 6);         LS_EQ_UINT(st.sync_fail, 7);
    LS_EQ_UINT(st.timeouts, 0xFFFE);

    /* No session: the stats are not asked for. */
    char text[2048];
    const int reads = fk.gfsk_stats_reads;
    diagnostics_text(text, sizeof(text));
    LS_EQ_INT(fk.gfsk_stats_reads, reads);
    LS_CHECK(strstr(text, "FSK rx stats") == NULL);

    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    const int arms = fk.rx_arms;
    fk.nframes = 0;
    diagnostics_text(text, sizeof(text));
    LS_EQ_INT(fk.gfsk_stats_reads, reads + 1);
    LS_CHECK(strstr(text, "FSK rx stats: received=258 crc=3 length=4 preamble=5 sync-ok=6"
                          " sync-fail=7 timeout=65534") != NULL);
    LS_EQ_INT(fk.rx_arms, arms);                 /* the part was not armed again */
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(count_frames_with(0x02, 0x45), 0); /* nothing like a reset of the counters */
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
}

LS_CASE(fsk_diagnostics_leave_a_restart_for_the_poll_and_do_not_read_stats_through_it)
{
    session_up();
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    char text[2048];
    const int reads = fk.gfsk_stats_reads;

    fk.reset_src = 2;                              /* restarted under the session */
    diagnostics_text(text, sizeof(text));          /* its GetStatus clears the source */
    LS_EQ_INT(fk.reset_src, 0);
    LS_EQ_INT(fk.gfsk_stats_reads, reads);         /* a part with no packet type is not asked */
    LS_CHECK(strstr(text, "FSK rx stats") == NULL);

    memset(fk.gfsk_pkt, 0, sizeof(fk.gfsk_pkt));
    fk.gfsk_pkt_set = false;
    uint8_t buf[16]; float rssi = 0;
    LS_EQ_INT(ls_lora_fsk_poll(buf, sizeof(buf), &rssi), 0);
    LS_CHECK(fk.gfsk_pkt_set);                     /* the poll still saw it, and programmed again */
    LS_EQ_UINT(fk.gfsk_pkt[5], 8);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
}

/* P25 on the LR2021: C4FM sampled at 19200 bps behind an 8-bit trigger
   word, with no packet length of its own. */
static const ls_fsk_cfg_t FSK_STREAM = {
    .freq_hz = 154785000, .bitrate = 19200, .deviation_hz = 1800, .bandwidth_hz = 12019,
    .sync_word = 0xCC000000, .sync_bits = 8, .stream = true,
};

/* `n` bytes arrive in the Rx FIFO, counting up from `first`, with no RxDone. */
static void stream_arrives(uint8_t first, int n)
{
    for (int i = 0; i < n && fk.fifo_n < 256; i++) fk.fifo[fk.fifo_n++] = (uint8_t)(first + i);
}

LS_CASE(fsk_stream_asks_for_the_longest_packet_behind_an_eight_bit_trigger)
{
    session_up();
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    /* 32 preamble bits to send, the detector off, fixed length 0xFFFF. */
    static const uint8_t pkt[7] = { 0x00, 0x20, 0x00, 0x00, 0xFF, 0xFF, 0x00 };
    LS_CHECK(memcmp(fk.gfsk_pkt, pkt, 7) == 0);
    static const uint8_t sync[9] = { 0, 0, 0, 0, 0, 0, 0, 0xCC, 0x88 };
    LS_CHECK(memcmp(fk.gfsk_sync, sync, 9) == 0);
    static const uint8_t rate[4] = { 0x00, 0x00, 0x4B, 0x00 };   /* 19200 bps */
    LS_CHECK(memcmp(fk.gfsk_mod, rate, 4) == 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* A stream only listens, and has no packets to poll. */
    uint8_t buf[300]; float rssi = 0;
    LS_EQ_INT(ls_lora_fsk_poll(buf, sizeof(buf), &rssi), -1);
    const uint8_t msg[2] = { 1, 2 };
    LS_EQ_INT(ls_lora_fsk_send(msg, sizeof(msg)), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);

    /* The filter rule is a packet session's: the same numbers without
       stream are refused, 19200 + 2 x 1800 being wider than 12019. */
    ls_fsk_cfg_t f = FSK_STREAM;
    f.stream = false;
    f.payload_bytes = 8;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f = FSK_STREAM;
    f.rssi_at_sync = true;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    no_hf_transmit();
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(fsk_stream_hands_over_bytes_as_they_arrive_and_marks_each_new_packet)
{
    session_up();
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    uint8_t buf[512];
    bool restarted = true;
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 0);
    LS_CHECK(!restarted);

    stream_arrives(0x10, 40);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 40);
    LS_CHECK(restarted);                           /* the first bytes of a packet */
    LS_EQ_UINT(buf[0], 0x10);
    LS_EQ_UINT(buf[39], 0x10 + 39);
    stream_arrives(0x40, 30);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 30);
    LS_CHECK(!restarted);                          /* these follow them */

    /* The packet ends at 100 bytes: 30 of its own are left, and 5 behind
       them are not its own. Those 30 are read, and Rx is armed again from
       an empty buffer. */
    stream_arrives(0x60, 35);
    fk.rx_pkt_len = 100;
    fk.irq |= (1u << 18);
    const int arms = fk.rx_arms;
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 30);
    LS_CHECK(!restarted);
    LS_EQ_UINT(buf[29], 0x60 + 29);
    LS_EQ_INT(fk.rx_arms, arms + 1);
    LS_EQ_INT(fk.fifo_n, 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    stream_arrives(0x80, 10);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 10);
    LS_CHECK(restarted);                           /* the next packet */
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(fsk_stream_marks_what_follows_a_full_buffer_as_not_continuing)
{
    session_up();
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    uint8_t buf[512];
    bool restarted = false;
    stream_arrives(0, 10);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 10);
    stream_arrives(0, 256);                        /* full: what came after it was lost */
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 256);
    LS_CHECK(!restarted);                          /* these still follow the last */
    stream_arrives(0, 20);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 20);
    LS_CHECK(restarted);                           /* these do not */

    /* The part restarts under the stream: programmed again, and a new packet. */
    fk.reset_src = 2;
    memset(fk.gfsk_pkt, 0, sizeof(fk.gfsk_pkt));
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 0);
    LS_EQ_UINT(fk.gfsk_pkt[4], 0xFF);
    LS_EQ_UINT(fk.gfsk_pkt[5], 0xFF);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    stream_arrives(0, 5);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 5);
    LS_CHECK(restarted);

    /* A buffer smaller than the part's own is refused, and so is a read
       with no stream. */
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, 100, &restarted), -1);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), -1);
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), -1);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(fk.violations, 0);
}

/* The first command frame with this opcode at or after `from`, or -1. */
static int frame_from(uint8_t b0, uint8_t b1, int from)
{
    for (int i = from; i < fk.nframes; i++)
        if (!fk.frames[i].read2 && fk.frames[i].b[0] == b0 && fk.frames[i].b[1] == b1) return i;
    return -1;
}

static bool in_order(int a, int b) { return a >= 0 && b >= 0 && a < b; }

LS_CASE(fsk_stream_gain_step_is_one_frame_after_the_setup_and_zero_sends_none)
{
    session_up();
    /* The chip's AGC, which is what a zeroed config means: no frame for it. */
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 0);

    ls_fsk_cfg_t f = FSK_STREAM;
    f.rx_gain_step = 13;
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 1);
    const int gain = frame_from(0x02, 0x1A, 0);
    LS_EQ_UINT(fk.frames[gain].b[2], 13);
    LS_EQ_UINT(fk.agc, 13);
    /* In standby, after the modulation, packet, sync word and PA, and before
       Rx is armed. */
    LS_CHECK(in_order(frame_from(0x02, 0x40, 0), gain));
    LS_CHECK(in_order(frame_from(0x02, 0x41, 0), gain));
    LS_CHECK(in_order(frame_from(0x02, 0x44, 0), gain));
    LS_CHECK(in_order(frame_from(0x02, 0x03, 0), gain));
    LS_CHECK(in_order(gain, frame_from(0x02, 0x0C, 0)));
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* Ending it hands the AGC back, so the mesh does not inherit the step. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 1);
    LS_EQ_UINT(fk.agc, 0);

    /* And the next session that does not ask sends nothing again. */
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.bad_args, 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(fsk_gain_step_above_13_is_refused_before_touching_the_part)
{
    session_up();
    ls_fsk_cfg_t f = FSK_STREAM;
    f.rx_gain_step = 14;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    f.rx_gain_step = 255;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, 0);
    LS_CHECK(!ls_lora_fsk_active());

    /* The SX126x has no such setting. */
    ls_lora_stop();
    fk_install(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    f = FSK_868;
    f.rx_gain_step = 1;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_ERR_NOT_SUPPORTED);
}

LS_CASE(fsk_gain_step_is_sent_again_after_the_part_restarts_and_the_agc_after_a_forced_step)
{
    session_up();
    ls_fsk_cfg_t f = FSK_STREAM;
    f.rx_gain_step = 13;
    LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);

    /* The part restarts under the stream and forgets the step: programmed
       again, with it, by the setup's one calibration. */
    uint8_t buf[512];
    bool restarted = false;
    fk.reset_src = 2;
    fk.agc = 0;
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 0);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 1);
    LS_EQ_UINT(fk.agc, 13);
    LS_EQ_INT(count_frames_with(0x01, 0x22), 1);
    LS_EQ_UINT(fk.calibrate_blocks, 0x2F);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);

    /* A restart under a session with no step has nothing to put back. */
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    fk.reset_src = 2;
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_stream_read(buf, sizeof(buf), &restarted), 0);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);

    /* A step another session left in the part (Mode S holds one) is taken
       back with the AGC setting, once. */
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    LS_EQ_INT(ls_lora_modes_end(), ESP_OK);
    LS_EQ_UINT(fk.agc, 13);
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 1);
    LS_EQ_UINT(fk.frames[frame_from(0x02, 0x1A, 0)].b[2], 0);
    LS_EQ_UINT(fk.agc, 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    fk.nframes = 0;
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);
    LS_EQ_INT(count_frames_with(0x02, 0x1A), 0);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(p25_receive_trim_reaches_the_frequency_the_fsk_session_tunes_to)
{
    session_up();
    /* A P25 control channel; the trim is added where p25_lora_begin tunes. */
    const uint32_t channel = 851012500u;
    static const int16_t trims[] = { 0, -1500, 1500, -5000, 5000 };
    for (unsigned i = 0; i < sizeof(trims) / sizeof(trims[0]); i++) {
        const p25_lr_cfg_t cfg = { .freq_trim_hz = trims[i] };
        ls_fsk_cfg_t f = FSK_STREAM;
        f.freq_hz = p25_lr_tuned_hz(channel, &cfg);
        LS_EQ_UINT(f.freq_hz, (uint32_t)((int32_t)channel + trims[i]));
        LS_EQ_INT(ls_lora_fsk_begin(&f), ESP_OK);
        LS_EQ_UINT(fk.last_freq_hz, (uint32_t)((int32_t)channel + trims[i]));
        LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    }
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(an_fsk_session_far_from_the_mesh_calibrates_once_in_its_setup)
{
    lora_up(NULL);                                         /* the mesh, at 910.525 MHz */
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_STREAM), ESP_OK);     /* 154.785 MHz */

    /* The setup's own calibration, ahead of the tune, and no second pass
       behind it: P25 on the LR2021 was measured at its best this way. */
    int tune = -1;
    for (int i = 0; i < fk.nframes && tune < 0; i++) {
        static const uint8_t hz[4] = { 0x09, 0x39, 0xD4, 0xE8 };     /* 154785000 */
        if (!fk.frames[i].read2 && fk.frames[i].b[0] == 0x02 && fk.frames[i].b[1] == 0x00 &&
            memcmp(&fk.frames[i].b[2], hz, sizeof(hz)) == 0) tune = i;
    }
    LS_CHECK(tune >= 0);
    LS_EQ_INT(count_frames_with(0x01, 0x22), 1);
    const int first = frame_from(0x01, 0x22, 0);
    LS_EQ_UINT(fk.frames[first].b[2], 0x2F);
    LS_CHECK(in_order(first, tune));
    LS_EQ_UINT(fk.last_freq_hz, 154785000u);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(unknown_frames(), 0);
}

LS_CASE(dme_ook_tuning_uses_only_lf_receive_through_1213_mhz)
{
    session_up();
    const uint32_t freqs[] = {962000000u, 1100000000u, 1163000000u, 1178000000u, 1205000000u, 1213000000u};
    for (unsigned i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        ls_ook_cfg_t cfg = {.freq_hz = freqs[i], .bitrate = 2000000, .bandwidth_hz = 3076923,
            .pattern = 0xFE, .pattern_bits = 8, .frame_bytes = 12, .gain_step = 0, .boost = 7};
        LS_EQ_INT(ls_lora_ook_begin(&cfg), ESP_OK);
        LS_EQ_UINT(fk.last_freq_hz, freqs[i]);
        LS_EQ_UINT(fk.last_path, LR20XX_PATH_LF);
        LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
        LS_CHECK(!tx_opcode_seen());
        LS_EQ_INT(ls_lora_ook_end(), ESP_OK);
    }
    LS_EQ_INT(fk.tx_starts, 0);
    LS_EQ_INT(unknown_frames(), 0);
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(side_detector_bytes_and_receive_constraints)
{
    lr_up(); const lr20xx_side_det_t side[]={{8,0,0},{9,1,1},{10,1,0}};
    LS_EQ_INT(lr20xx_lora_side_config(7,62500,side,3),ESP_OK);
    FK_FRAME(0,0x02,0x24,0x80,0x95,0xa4);
    const uint8_t sync[]={0x12,0x2b,0x12};
    LS_EQ_INT(lr20xx_lora_side_sync(sync,3),ESP_OK); FK_FRAME(1,0x02,0x25,0x12,0x2b,0x12);
    LS_EQ_INT(lr20xx_lora_side_config(7,500000,side,3),ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_lora_side_config(9,250000,side,3),ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_lora_side_config(5,62500,side,3),ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_lora_side_sync(sync,4),ESP_ERR_INVALID_ARG);
    const lr20xx_side_det_t duplicate[]={{8,0,0},{8,0,0}};
    LS_EQ_INT(lr20xx_lora_side_config(7,62500,duplicate,2),ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes,2);
    LS_EQ_INT(lr20xx_lora_side_config(7,62500,NULL,0),ESP_OK); FK_FRAME(2,0x02,0x24);
    LS_EQ_INT(unknown_frames(),0);
}
LS_CASE(zwave_commands_are_exact_semtech_layouts)
{
    lr_up();
    LS_EQ_INT(lr20xx_zwave_params(3,0x12,0,255,0x1234,32,true),ESP_OK);
    FK_FRAME(0,0x02,0x97,3,0x12,0,255,0x12,0x34,32,1);
    LS_EQ_INT(lr20xx_zwave_homeid(0xdeadbeef),ESP_OK); FK_FRAME(1,0x02,0x98,0xde,0xad,0xbe,0xef);
    const lr20xx_zwave_channel_t ch[]={{908420000,1,19},{908400000,2,34},{916000000,3,8}};
    LS_EQ_INT(lr20xx_zwave_scan_config(ch,3,0,true),ESP_OK);
    FK_FRAME(2,0x02,0x9c,0x30,0x39,0,1,0x36,0x25,0x63,0xa0,19,0x36,0x25,0x15,0x80,34,0x36,0x99,0x0d,0,8);
    LS_EQ_INT(lr20xx_zwave_scan(),ESP_OK); FK_FRAME(3,0x02,0x9d);
    LS_EQ_INT(lr20xx_zwave_scan_config(ch,4,0,true),ESP_ERR_INVALID_ARG);
    LS_EQ_INT(unknown_frames(),0); LS_EQ_INT(fk.violations,0);
}
LS_CASE(wisun_commands_match_packet_bitfields)
{
    lr_up(); LS_EQ_INT(lr20xx_wisun_mode(4,0x12),ESP_OK); FK_FRAME(0,0x02,0x70,4,0x12);
    LS_EQ_INT(lr20xx_wisun_packet(true,true,true,3,0x456,8,24),ESP_OK);
    FK_FRAME(1,0x02,0x71,0x3b,4,0x56,8,24);
    LS_EQ_INT(lr20xx_wisun_packet(false,true,true,1,2048,8,24),ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_wisun_mode(8,0),ESP_ERR_INVALID_ARG); LS_EQ_INT(fk.nframes,2);
    LS_EQ_INT(unknown_frames(),0);
}
LS_CASE(engine_reads_use_two_frames_and_unpack_status)
{
    lr_up(); const uint8_t ws[]={0x08,0x64,0,100,90,91,0x84,30}; memcpy(fk.engine_status,ws,8);
    lr20xx_engine_packet_t p;
    LS_EQ_INT(lr20xx_engine_status(true,&p),ESP_OK); FK_FRAME(0,0x02,0x73);
    LS_EQ_INT(fk.frames[1].full,10); LS_CHECK(fk.frames[1].read2);
    LS_EQ_UINT(p.phr,0x0864); LS_EQ_UINT(p.length,100); LS_CHECK(p.rssi_dbm == -90.5f); LS_EQ_UINT(p.sync,1);
    const uint8_t zs[]={0,10,80,81,3,0x14,20}; memcpy(fk.engine_status,zs,7);
    LS_EQ_INT(lr20xx_engine_status(false,&p),ESP_OK); FK_FRAME(2,0x02,0x9a);
    LS_EQ_INT(fk.frames[3].full,9); LS_EQ_UINT(p.rate,3); LS_EQ_UINT(p.channel,2); LS_CHECK(p.rssi_dbm == -80.5f);
    const uint8_t counts[]={0x12,0x34,0,2,0,3}; memcpy(fk.engine_stats,counts,6);
    lr20xx_engine_stats_t stats;
    LS_EQ_INT(lr20xx_engine_stats(true,&stats),ESP_OK); FK_FRAME(4,0x02,0x72);
    LS_EQ_UINT(stats.packets,0x1234); LS_EQ_UINT(stats.crc_errors,2); LS_EQ_UINT(stats.length_errors,3);
    LS_EQ_INT(lr20xx_engine_stats(false,&stats),ESP_OK); FK_FRAME(6,0x02,0x99);
    fk.bad_read_stat=true; LS_EQ_INT(lr20xx_engine_status(false,&p),ESP_ERR_INVALID_RESPONSE);
    LS_EQ_INT(fk.violations,0); LS_EQ_INT(unknown_frames(),0);
}
LS_CASE(lora_status_has_detector_cr_length_and_signed_fei)
{
    lr_up(); fk.rx_pkt_len=42; fk.lora_rssi_flags=0x22; fk.lora_fei=-1250;
    lr20xx_lora_pkt_status_t p;
    LS_EQ_INT(lr20xx_get_lora_packet_status(&p),ESP_OK); LS_EQ_UINT(p.detector,8); LS_EQ_UINT(p.cr,1);
    LS_EQ_UINT(p.length,42); LS_EQ_INT(p.fei_hz,-1250); LS_EQ_INT(fk.frames[1].full,11);
}
LS_CASE(experimental_sessions_never_send_and_restore_lora)
{
    lr_up(); LS_EQ_INT(ls_lora_start(), ESP_OK); fk.nframes=0;
    for (int engine=0; engine<3; engine++) {
        LS_EQ_INT(lr20xx_exp_begin((lr20xx_engine_t)engine,0,0,1),ESP_OK);
        LS_EQ_INT(lr20xx_exp_begin((lr20xx_engine_t)engine,0,0,1),ESP_ERR_INVALID_STATE);
        uint8_t b[255]; lr20xx_engine_packet_t p;
        LS_EQ_INT(lr20xx_exp_poll(b,sizeof(b),&p),0);
        LS_EQ_INT(lr20xx_exp_end(),ESP_OK);
    }
    for(int i=0;i<fk.nframes;i++) if(!fk.frames[i].read2) {
        unsigned op=(fk.frames[i].b[0]<<8)|fk.frames[i].b[1];
        LS_CHECK(op!=0x020d); LS_CHECK(op!=0x0002); LS_CHECK(op!=0x0202); LS_CHECK(op!=0x0203);
    }
    LS_EQ_INT(unknown_frames(),0); LS_EQ_INT(fk.violations,0);
}

LS_CASE(experimental_packet_poll_reads_and_rearms_each_engine)
{
    lr_up(); LS_EQ_INT(ls_lora_start(),ESP_OK);
    for (int engine=0;engine<3;engine++) {
        LS_EQ_INT(lr20xx_exp_begin((lr20xx_engine_t)engine,0,0,1),ESP_OK);
        memset(fk.engine_status,0,8);
        fk.rx_pkt_len=10;
        if(engine==1) { fk.engine_status[1]=10; fk.engine_status[2]=80; fk.engine_status[4]=2; }
        if(engine==2) { fk.engine_status[0]=8; fk.engine_status[1]=10; fk.engine_status[3]=10; fk.engine_status[4]=90; }
        fk.lora_rssi_flags=4; fk.lora_fei=321;
        fk.fifo_n=10; memset(fk.fifo,0x5a,10); fk.irq=LR20XX_IRQ_RX_DONE;
        uint8_t b[255]={0}; lr20xx_engine_packet_t p;
        LS_EQ_INT(lr20xx_exp_poll(b,sizeof(b),&p),10); LS_EQ_UINT(p.length,10); LS_EQ_UINT(b[9],0x5a);
        LS_EQ_INT(fk.fifo_n,0); LS_EQ_INT(fk.mode,LR20XX_MODE_RX);
        if(engine==0) { LS_EQ_UINT(p.detector,1); LS_EQ_INT(p.fei_hz,321); }
        if(engine==1) LS_EQ_UINT(p.rate,2);
        if(engine==2) LS_EQ_UINT(p.phr,0x080a);
        fk.irq=LR20XX_IRQ_RX_DONE|LR20XX_IRQ_CRC_ERROR;
        LS_EQ_INT(lr20xx_exp_poll(b,sizeof(b),&p),-1); LS_EQ_INT(fk.irq,0);
        fk.reset_src=1; LS_EQ_INT(lr20xx_exp_poll(b,sizeof(b),&p),0);
        LS_EQ_INT(lr20xx_exp_end(),ESP_OK);
    }
    LS_EQ_INT(fk.violations,0); LS_EQ_INT(unknown_frames(),0);
}
LS_CASE(wisun_large_psdu_is_counted_without_reading_past_buffer)
{
    lr_up(); LS_EQ_INT(ls_lora_start(),ESP_OK);
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_WISUN,2,41,1),ESP_OK);
    LS_EQ_UINT(fk.last_freq_hz,927200000);
    fk.engine_status[0]=9; fk.engine_status[1]=44; fk.engine_status[2]=1; fk.engine_status[3]=44;
    fk.irq=LR20XX_IRQ_RX_DONE;
    uint8_t b[16]; memset(b,0xcc,sizeof(b)); lr20xx_engine_packet_t p;
    LS_EQ_INT(lr20xx_exp_poll(b,sizeof(b),&p),-2); LS_EQ_UINT(p.length,300); LS_EQ_UINT(b[0],0xcc);
    LS_EQ_INT(lr20xx_exp_end(),ESP_OK);
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_WISUN,2,42,1),ESP_ERR_INVALID_ARG);
}
LS_CASE(experimental_stop_restores_mesh_and_failed_start_gives_it_back)
{
    lr_up(); ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg),ESP_OK); LS_EQ_INT(ls_lora_receive(),ESP_OK);
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_LORAMON,1,0,0),ESP_OK);
    LS_EQ_UINT(fk.last_freq_hz,906875000); LS_EQ_INT(ls_lora_receive(),ESP_ERR_INVALID_STATE);
    LS_EQ_INT(lr20xx_exp_end(),ESP_OK); LS_EQ_UINT(fk.last_freq_hz,cfg.freq_hz); LS_CHECK(ls_lora_is_receiving());
    fk.fail_op=LR20XX_OP_SET_ZWAVE_SCAN_CFG;
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_ZWAVE,0,0,0),ESP_ERR_INVALID_RESPONSE);
    LS_EQ_UINT(fk.last_freq_hz,cfg.freq_hz); LS_CHECK(ls_lora_is_receiving());
    fk.fail_op=0; LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_WISUN,0,0,1),ESP_OK);
    LS_EQ_INT(lr20xx_exp_end(),ESP_OK); LS_CHECK(ls_lora_is_receiving());
}

/* ================================================================== */
/* A part that is slow to start, wedged, or not an LR2021             */
/* ================================================================== */

LS_CASE(a_part_still_starting_after_a_reset_is_found_by_resetting_it_again)
{
    /* The first frames of a part that has not finished starting come back as
       nonsense; that was read as an empty socket for the whole session. */
    lr_up();
    fk.model_reset = true;
    fk.dead_resets = 2;
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_LR20XX);
    LS_EQ_INT(fk.resets, 3);
}

LS_CASE(a_part_that_never_answers_is_not_found_after_a_bounded_number_of_resets)
{
    lr_up();
    fk.model_reset = true;
    fk.dead_resets = 99;
    LS_EQ_INT(ls_lora_start(), ESP_ERR_NOT_FOUND);
    LS_CHECK(!ls_lora_present());
    LS_EQ_INT(fk.resets, 4);
}

LS_CASE(a_wedged_part_is_reset_and_the_mesh_configuration_goes_through)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.model_reset = true;
    fk.wedged = true;                      /* refuses every command until NRESET */
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(fk.resets, 1);
    LS_CHECK(!fk.wedged);
    LS_EQ_UINT(fk.packet_type, LR20XX_PKT_TYPE_LORA);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_CHECK(ls_lora_is_receiving());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
}

LS_CASE(a_command_refused_every_time_fails_after_one_reset_and_not_forever)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.model_reset = true;
    fk.fail_op = 0x0122;                   /* Calibrate */
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_ERR_INVALID_RESPONSE);
    LS_EQ_INT(fk.resets, 1);
    fk.fail_op = 0;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
}

LS_CASE(an_experiment_whose_part_wedges_is_reset_and_programmed_again)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.model_reset = true;
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_ZWAVE, 0, 0, 0), ESP_OK);
    fk.wedged = true;
    fk.mode = LR20XX_MODE_STBY_RC;         /* it dropped out of Rx and will not take commands */
    uint8_t b[255]; lr20xx_engine_packet_t p;
    for (int i = 0; i < 8; i++) (void)lr20xx_exp_poll(b, sizeof(b), &p);
    LS_EQ_INT(fk.resets, 1);
    LS_CHECK(!fk.wedged);
    LS_EQ_UINT(fk.packet_type, LR20XX_PKT_TYPE_ZWAVE);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(lr20xx_exp_end(), ESP_OK);
}

LS_CASE(an_fsk_session_whose_part_wedges_is_reset_and_programmed_again)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.model_reset = true;
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    fk.wedged = true;
    fk.mode = LR20XX_MODE_STBY_RC;
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < 6 && e != ESP_OK; i++) e = ls_lora_fsk_receive();
    LS_EQ_INT(e, ESP_OK);
    LS_EQ_INT(fk.resets, 1);
    LS_EQ_UINT(fk.packet_type, LR20XX_PKT_TYPE_GFSK);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
}

LS_CASE(a_session_start_on_a_wedged_part_resets_it_and_succeeds)
{
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.model_reset = true;
    fk.wedged = true;
    LS_EQ_INT(ls_lora_fsk_begin(&FSK_868), ESP_OK);
    LS_EQ_INT(fk.resets, 1);
    LS_EQ_UINT(fk.packet_type, LR20XX_PKT_TYPE_GFSK);
    LS_EQ_INT(ls_lora_fsk_end(), ESP_OK);
}

LS_CASE(a_restart_seen_by_a_check_is_not_lost_to_the_receive_that_follows)
{
    /* The checks after each command are GetStatus, which clears the reset
       source. A restart one of them saw left the receive path thinking the
       part still held LoRa, and it refused every command from then on. */
    lr_up();
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    fk.packet_type = 0xFF;                 /* it restarted */
    fk.reset_src = 2;
    fk.mode = LR20XX_MODE_STBY_RC;
    (void)lr20xx_check_last_ok();          /* some other command's check reads the source */
    uint8_t buf[16]; float r, s;
    for (int i = 0; i < 4; i++) (void)ls_lora_poll(buf, sizeof(buf), &r, &s);
    LS_EQ_UINT(fk.packet_type, LR20XX_PKT_TYPE_LORA);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(ls_lora_is_receiving());
}

LS_CASE(a_part_back_in_rx_by_itself_is_not_armed_again)
{
    session_up();
    LS_EQ_INT(ls_lora_modes_begin(1090000000u, 13), ESP_OK);
    fk.mode = LR20XX_MODE_STBY_RC;         /* the poll finds it out of Rx ... */
    fk.has_next_mode = true;
    fk.next_mode = LR20XX_MODE_RX;         /* ... and it is back in Rx a moment later */
    fk.rx_arms = 0;
    uint8_t buf[28]; float rssi;
    LS_EQ_INT(ls_lora_modes_poll(buf, sizeof(buf), &rssi), 0);
    LS_EQ_INT(fk.rx_arms, 0);              /* SetRx in Rx is refused, as is ClearRxFifo */
    LS_EQ_INT(fk.bad_mode, 0);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
}

LS_CASE(the_experiment_engines_are_not_an_sx126x_thing)
{
    lr_up();
    fk_install(FK_SX_ECHO);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    LS_EQ_INT(ls_lora_chip(), LS_LORA_CHIP_SX126X);
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_LORAMON, 0, 0, 0), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_ZWAVE, 0, 0, 0), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(lr20xx_exp_begin(LR20XX_ENGINE_WISUN, 0, 0, 1), ESP_ERR_NOT_SUPPORTED);
}
