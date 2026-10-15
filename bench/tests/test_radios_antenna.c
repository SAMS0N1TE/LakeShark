#define LS_TEST_REAL_BOARD_HW 1
#include "lora_fakes.h"
#include "ls_board_hw.h"
#include "ls_lora_lr20xx.h"

esp_err_t ls_xl9535_set(int pin, bool level) { return ls_xl9535_out(pin, level); }
esp_err_t ls_xl9535_init(ls_i2c_bus_id_t bus, uint8_t addr) { return ESP_OK; }

static void radio_up(fk_kind_t kind)
{
    fk_xl_error = ESP_OK;
    ls_lora_stop();
    fk_install(kind);
    LS_EQ_INT(ls_board_hw_antenna_external(false), ESP_OK);
    ls_lora_cfg_t cfg;
    ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
}

/* A route chosen at boot or by an app (compass calibration) transmits the
   same as one chosen in RADIOS: no hidden acknowledgment to lock TX out. */
LS_CASE(external_route_never_blocks_transmit)
{
    for (int kind = FK_LR; kind <= FK_SX_STATUS; ++kind) {
        radio_up((fk_kind_t)kind);
        LS_EQ_INT(ls_board_hw_antenna_external(true), ESP_OK);
        LS_CHECK(ls_board_hw_antenna_is_external());
        LS_CHECK(ls_board_hw_antenna_tx_allowed());
        uint8_t data = 1;
        LS_EQ_INT(ls_lora_send(&data, 1), ESP_OK);
        LS_CHECK(ls_lora_is_transmitting());
    }
}

/* The last level written to the antenna switch since `from`, or -1. */
static int vctl_since(int from)
{
    int level = -1;
    for (int i = from; i < fk_xl_log_n; i++)
        if (fk_xl_log[i].pin == LS_BOARD_XL_RF_SW_VCTL) level = fk_xl_log[i].level;
    return level;
}

/* MMCX1 receive-only (the default): the transmit goes out the internal
   antenna (VCTL high) and MMCX1 (low) is back once the part reports it
   done. The operator's choice reads MMCX1 throughout. */
LS_CASE(receive_only_mmcx1_moves_transmit_to_internal_and_back)
{
    radio_up(FK_LR);
    ls_board_hw_antenna_set_mmcx_tx(false);
    LS_EQ_INT(ls_board_hw_antenna_external(true), ESP_OK);
    int mark = fk_xl_log_n;
    uint8_t data = 1;
    LS_EQ_INT(ls_lora_send(&data, 1), ESP_OK);
    LS_EQ_INT(vctl_since(mark), 1);
    LS_CHECK(ls_board_hw_antenna_is_external());
    mark = fk_xl_log_n;
    LS_CHECK(!ls_lora_send_done());
    LS_EQ_INT(vctl_since(mark), -1);
    fk.irq |= LR20XX_IRQ_TX_DONE;
    LS_CHECK(ls_lora_send_done());
    LS_EQ_INT(vctl_since(mark), 0);
    LS_CHECK(ls_board_hw_antenna_is_external());

    /* A refused start puts the route straight back. */
    mark = fk_xl_log_n;
    LS_EQ_INT(ls_lora_send(&data, 0), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(vctl_since(mark), 0);

    /* A failed switch write means no transmit at all. */
    const int frames = fk.nframes;
    fk_xl_error = ESP_FAIL;
    LS_CHECK(ls_lora_send(&data, 1) != ESP_OK);
    fk_xl_error = ESP_OK;
    LS_EQ_INT(fk.nframes, frames);
    LS_CHECK(!ls_lora_is_transmitting());
}

LS_CASE(mmcx1_rx_tx_and_internal_route_leave_the_switch_alone)
{
    radio_up(FK_LR);
    uint8_t data = 1;
    ls_board_hw_antenna_set_mmcx_tx(true);
    LS_EQ_INT(ls_board_hw_antenna_external(true), ESP_OK);
    int mark = fk_xl_log_n;
    LS_EQ_INT(ls_lora_send(&data, 1), ESP_OK);
    LS_EQ_INT(vctl_since(mark), -1);
    fk.irq |= LR20XX_IRQ_TX_DONE;
    LS_CHECK(ls_lora_send_done());
    ls_board_hw_antenna_set_mmcx_tx(false);
    LS_EQ_INT(ls_board_hw_antenna_external(false), ESP_OK);
    mark = fk_xl_log_n;
    LS_EQ_INT(ls_lora_send(&data, 1), ESP_OK);
    LS_EQ_INT(vctl_since(mark), -1);
    fk.irq |= LR20XX_IRQ_TX_DONE;
    LS_CHECK(ls_lora_send_done());
}

LS_CASE(transmitting_socket_refuses_route_changes)
{
    radio_up(FK_LR);
    LS_EQ_INT(ls_board_hw_antenna_confirm_external(), ESP_OK);
    uint8_t data = 1;
    LS_EQ_INT(ls_lora_send(&data, 1), ESP_OK);
    int frames = fk.nframes;
    LS_CHECK(ls_lora_is_transmitting());
    LS_EQ_INT(fk.nframes, frames);
    int writes = fk_xl_log_n;
    LS_EQ_INT(ls_board_hw_antenna_external(false), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk_xl_log_n, writes);
    LS_CHECK(ls_board_hw_antenna_is_external());
    fk.irq |= LR20XX_IRQ_TX_DONE;
    LS_EQ_INT(ls_board_hw_antenna_external(false), ESP_OK);
    LS_CHECK(!ls_board_hw_antenna_is_external());
    LS_CHECK(!ls_lora_is_transmitting());
}

LS_CASE(parking_stops_receiver_and_preserves_state_on_reset_failure)
{
    for (int kind = FK_LR; kind <= FK_SX_STATUS; ++kind) {
        radio_up((fk_kind_t)kind);
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        LS_CHECK(ls_lora_is_receiving());
        fk_xl_error = ESP_FAIL;
        LS_EQ_INT(ls_lora_park(), ESP_FAIL);
        LS_CHECK(ls_lora_present());
        LS_CHECK(ls_lora_is_receiving());
        fk_xl_error = ESP_OK;
        LS_EQ_INT(ls_lora_park(), ESP_OK);
        LS_CHECK(!ls_lora_present());
        LS_CHECK(!ls_lora_is_receiving());
        LS_EQ_INT(fk_xl_log[fk_xl_log_n - 1].pin, LS_BOARD_XL_RADIO_RST);
        LS_CHECK(!fk_xl_log[fk_xl_log_n - 1].level);
    }
}

LS_CASE(reinit_keeps_external_route_and_never_drives_internal)
{
    for (int kind = FK_LR; kind <= FK_SX_STATUS; ++kind) {
        radio_up((fk_kind_t)kind);
        LS_EQ_INT(ls_board_hw_antenna_external(true), ESP_OK);
        for (int round = 0; round < 3; ++round) {
            ls_lora_stop();
            fk_install((fk_kind_t)kind);
            ls_lora_cfg_t cfg;
            ls_lora_cfg_default(&cfg);
            int from = fk_xl_log_n;
            LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
            LS_CHECK(ls_board_hw_antenna_is_external());
            for (int i = from; i < fk_xl_log_n; ++i)
                if (fk_xl_log[i].pin == LS_BOARD_XL_RF_SW_VCTL)
                    LS_CHECK(!fk_xl_log[i].level);
        }
    }
}

LS_CASE(cached_transmit_status_distinguishes_rx_and_tx_on_both_chips)
{
    for (int kind = FK_LR; kind <= FK_SX_STATUS; kind++) {
        radio_up((fk_kind_t)kind);
        LS_CHECK(!ls_lora_is_transmitting());
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        LS_CHECK(ls_lora_is_receiving() && !ls_lora_is_transmitting());
        uint8_t data = 1;
        LS_EQ_INT(ls_lora_send(&data, 1), ESP_OK);
        int frames = fk.nframes;
        LS_CHECK(ls_lora_is_transmitting() && !ls_lora_is_receiving());
        LS_EQ_INT(fk.nframes, frames);
        ls_shim_time_advance(9000000);
        LS_CHECK(ls_lora_send_done());
        LS_CHECK(!ls_lora_is_transmitting());
        LS_EQ_INT(ls_lora_park(), ESP_OK);
        LS_CHECK(!ls_lora_is_transmitting());
    }
}
