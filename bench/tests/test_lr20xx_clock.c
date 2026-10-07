/* LR2021 init and receive diagnostics against the shared SPI fake. */
#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"
#include "ls_test.h"

static int first_op(uint16_t op)
{
    for (int i = 0; i < fk.nframes; i++)
        if (!fk.frames[i].read2 && fk.frames[i].b[0] == (uint8_t)(op >> 8) &&
            fk.frames[i].b[1] == (uint8_t)op) return i;
    return -1;
}

LS_CASE(clock_modes_reinit_fallback_and_receive_status)
{
    {
        fk_lr_tcxo = true;
        fk_lr_dcdc = false;
        fk_install(FK_LR);
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        const int tcxo = first_op(0x0120), reg = first_op(0x0121), cal = first_op(0x0122);
        LS_CHECK(tcxo >= 0 && reg > tcxo && cal > reg);
        FK_FRAME(tcxo, 0x01, 0x20, 0x07, 0x00, 0x00, 0x01, 0x48);
        FK_FRAME(reg, 0x01, 0x21, 0x00);
        LS_EQ_INT(fk.violations, 0);
        LS_EQ_INT(fk.bad_args, 0);
        fk_lr_tcxo = false;
        fk_lr_dcdc = true;
        fk_install(FK_LR);
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        LS_EQ_INT(first_op(0x0120), -1);
        LS_CHECK(first_op(0x0121) < first_op(0x0122));
        FK_FRAME(first_op(0x0121), 0x01, 0x21, 0x01);
        LS_EQ_INT(fk.violations, 0);
    }
    {
        fk_lr_tcxo = true;
        fk_lr_dcdc = false;
        fk_install(FK_LR);
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        ls_lora_cfg_t cfg;
        ls_lora_cfg_default(&cfg);
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        LS_EQ_INT(first_op(0x0104), -1);
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        const int n = fk.nframes;
        char *argv[] = { "lora", "lrstat" };
        LS_EQ_INT(lr20xx_clock_command(2, argv), 0);
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
        LS_EQ_INT(fk.nframes, n + 2);
        FK_FRAME(n, 0x01, 0x10);
        LS_CHECK(fk.frames[n + 1].read2);
    }
    {
        fk_install(FK_LR);
        fk_lr_tcxo = true;
        fk_lr_dcdc = false;
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        ls_lora_cfg_t cfg;
        ls_lora_cfg_default(&cfg);
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        fk.model_reset = true;
        fk.nframes = 0;
        char *clock[] = { "lora", "lrclk", "xtal" };
        LS_EQ_INT(lr20xx_clock_command(3, clock), 0);
        LS_CHECK(!fk_lr_tcxo);
        LS_EQ_INT(first_op(0x0120), -1);
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
        fk.nframes = 0;
        char *reg[] = { "lora", "lrreg", "dcdc" };
        LS_EQ_INT(lr20xx_clock_command(3, reg), 0);
        LS_CHECK(fk_lr_dcdc);
        FK_FRAME(first_op(0x0121), 0x01, 0x21, 0x01);
        LS_CHECK(first_op(0x0104) >= 0);
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
        char *tcxo[] = { "lora", "lrclk", "tcxo" };
        fk_tcxo_errors = (1u << 0) | (1u << 2);
        fk.nframes = 0;
        const int resets = fk.resets;
        LS_EQ_INT(lr20xx_clock_command(3, tcxo), 0);
        LS_CHECK(fk_lr_tcxo);
        LS_CHECK(fk.resets >= resets + 2);
        int tcxo_count = 0;
        for (int i = 0; i < fk.nframes; i++)
            if (!fk.frames[i].read2 && fk.frames[i].b[0] == 1 && fk.frames[i].b[1] == 0x20) tcxo_count++;
        LS_EQ_INT(tcxo_count, 1);
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
        fk_tcxo_errors = 0;
        fk.nframes = 0;
        LS_EQ_INT(lr20xx_initialize(), ESP_OK);
        LS_EQ_INT(first_op(0x0120), -1);
        LS_CHECK(fk_lr_tcxo);
    }
}
