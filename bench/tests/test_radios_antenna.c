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

LS_CASE(external_route_requires_acknowledgment_for_lora_and_fsk)
{
    for (int kind = FK_LR; kind <= FK_SX_STATUS; ++kind) {
        radio_up((fk_kind_t)kind);
        LS_EQ_INT(ls_board_hw_antenna_external(true), ESP_OK);
        LS_CHECK(!ls_board_hw_antenna_tx_allowed());
        uint8_t data = 1;
        int frames = fk.nframes;
        LS_EQ_INT(ls_lora_send(&data, 1), ESP_ERR_INVALID_STATE);
        LS_EQ_INT(ls_lora_fsk_send(&data, 1), ESP_ERR_INVALID_STATE);
        LS_EQ_INT(fk.nframes, frames);
        LS_EQ_INT(ls_board_hw_antenna_confirm_external(), ESP_OK);
        LS_CHECK(ls_board_hw_antenna_tx_allowed());
        LS_EQ_INT(ls_board_hw_antenna_external(false), ESP_OK);
        LS_EQ_INT(ls_board_hw_antenna_external(true), ESP_OK);
        LS_CHECK(!ls_board_hw_antenna_tx_allowed());
    }
}

LS_CASE(transmitting_socket_refuses_route_changes)
{
    radio_up(FK_LR);
    LS_EQ_INT(ls_board_hw_antenna_confirm_external(), ESP_OK);
    uint8_t data = 1;
    LS_EQ_INT(ls_lora_send(&data, 1), ESP_OK);
    int writes = fk_xl_log_n;
    LS_EQ_INT(ls_board_hw_antenna_external(false), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk_xl_log_n, writes);
    LS_CHECK(ls_board_hw_antenna_is_external());
    fk.irq |= LR20XX_IRQ_TX_DONE;
    LS_EQ_INT(ls_board_hw_antenna_external(false), ESP_OK);
    LS_CHECK(!ls_board_hw_antenna_is_external());
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
