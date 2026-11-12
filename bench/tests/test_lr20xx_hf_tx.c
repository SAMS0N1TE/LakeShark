#include <string.h>

#include "lora_fakes.h"
#include "ls_board.h"
#include "ls_lora_lr20xx.h"
#include "ls_lora_priv.h"
#include "ls_test.h"

static void up(void)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk_install(FK_LR);
    fk_lr_tcxo = true;
    fk_lr_dcdc = false;
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    fk.nframes = 0;
}

LS_CASE(unlocked_the_rf_switch_routes_hf_transmit_on_dio7_and_dio10)
{
    up();
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    LS_EQ_INT(lr20xx_configure_dios(), ESP_OK);
    LS_EQ_UINT(fk.dio_rfsw[7], LR20XX_RFSW_TX_HF);
    LS_EQ_UINT(fk.dio_rfsw[10], LR20XX_RFSW_RX_HF | LR20XX_RFSW_TX_HF);
    LS_EQ_UINT(fk.dio_rfsw[6], LR20XX_RFSW_RX_HF);
    LS_EQ_UINT(fk.dio_rfsw[8], LR20XX_RFSW_RX_LF | LR20XX_RFSW_TX_LF);
}

LS_CASE(unlocked_2440_mhz_selects_the_hf_pa_and_holds_it_at_plus_12)
{
    up();
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    LS_EQ_INT(lr20xx_set_rf_frequency(2440000000u), ESP_OK);
    LS_CHECK(lr20xx_tx_allowed());
    LS_EQ_INT(lr20xx_set_pa_lf(22), ESP_OK);
    LS_EQ_UINT(fk.pa_sel, 1);
    LS_EQ_UINT(fk.pa_cfg[0], 0x80);
    LS_EQ_UINT(fk.pa_cfg[1], 0x67);
    LS_EQ_UINT(fk.pa_cfg[2], 28);
    LS_EQ_UINT(fk.tx_params[0], 24);
    LS_EQ_UINT(fk.tx_params[1], LR20XX_TX_RAMP_48US);
    LS_EQ_INT(lr20xx_set_pa_lf(-20), ESP_OK);
    LS_EQ_INT((int8_t)fk.tx_params[0], -39);
    LS_EQ_INT(lr20xx_set_pa_lf(5), ESP_OK);
    LS_EQ_INT((int8_t)fk.tx_params[0], 10);
    const int starts = fk.tx_starts;
    LS_EQ_INT(lr20xx_set_tx(), ESP_OK);
    LS_EQ_INT(fk.tx_starts, starts + 1);
    LS_EQ_UINT(fk.tx_freq_hz, 2440000000u);
}

LS_CASE(unlocked_crossing_the_band_edge_needs_the_pa_programmed_again)
{
    up();
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    LS_EQ_INT(lr20xx_set_rf_frequency(2440000000u), ESP_OK);
    LS_EQ_INT(lr20xx_set_pa_lf(10), ESP_OK);
    LS_EQ_INT(lr20xx_set_rf_frequency(915000000u), ESP_OK);
    const int starts = fk.tx_starts;
    LS_EQ_INT(lr20xx_set_tx(), ESP_ERR_NOT_SUPPORTED);
    LS_EQ_INT(fk.tx_starts, starts);
    LS_EQ_INT(lr20xx_set_pa_lf(22), ESP_OK);
    LS_EQ_UINT(fk.pa_sel, 0);
    LS_CHECK(!(fk.pa_cfg[0] & 0x80));
    LS_EQ_INT(lr20xx_set_tx(), ESP_OK);
    LS_EQ_INT(fk.tx_starts, starts + 1);
}

LS_CASE(unlocked_transmit_covers_the_whole_synthesiser_range)
{
    up();
    LS_EQ_INT(lr20xx_set_standby(false), ESP_OK);
    LS_EQ_INT(lr20xx_set_rf_frequency(1090000000u), ESP_OK);
    LS_CHECK(lr20xx_tx_allowed());
    LS_EQ_INT(lr20xx_set_rf_frequency(LR20XX_FREQ_MIN_HZ), ESP_OK);
    LS_CHECK(lr20xx_tx_allowed());
    LS_EQ_INT(lr20xx_set_rf_frequency(LR20XX_FREQ_MAX_HZ), ESP_OK);
    LS_CHECK(lr20xx_tx_allowed());
}
