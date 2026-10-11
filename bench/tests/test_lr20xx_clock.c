/* LR2021 init and receive diagnostics against the shared SPI fake. */
#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"
#include "ls_test.h"
#include "ls_lora_priv.h"

LS_CASE(fast_capture_busy_wait_has_a_bounded_deadline_without_clocking_the_part)
{
    fk_install(FK_LR); LS_EQ_INT(ls_lora_start(),ESP_OK);
    ls_lora_hw_lock();
    LS_EQ_INT(ls_lora_hw_capture_fast(true),ESP_OK);
    int before=fk.nframes;
    fk_busy_for=1000000; fk_busy_reads=0;
    int64_t start=esp_timer_get_time();
    LS_CHECK(!ls_lora_hw_wait_not_busy(2));
    LS_CHECK(esp_timer_get_time()-start<3000);
    LS_EQ_INT(fk.nframes,before);
    fk_busy_for=2;
    LS_CHECK(ls_lora_hw_wait_not_busy(20));
    LS_EQ_INT(ls_lora_hw_capture_fast(false),ESP_OK);
    ls_lora_hw_unlock();
}

static int first_op(uint16_t op)
{
    for (int i = 0; i < fk.nframes; i++)
        if (!fk.frames[i].read2 && fk.frames[i].b[0] == (uint8_t)(op >> 8) &&
            fk.frames[i].b[1] == (uint8_t)op) return i;
    return -1;
}

LS_CASE(memory_probe_bounds_and_receive_preservation)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg;
    ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    const char *invalid[][2] = {
        {"0xF40201", "1"}, {"0x1000000", "1"}, {"-4", "1"},
        {"0xFFFFFC", "2"}, {"0xF40200", "0"}, {"0xF40200", "17"},
        {"0xF40200", "-1"}, {"0xF40200", "1junk"},
        {"0xFFFFFFFFFFFFFFFFFFFFFFFF", "1"}, {"", "1"},
        {"+4", "1"}, {" 4", "1"}
    };
    const int before = fk.nframes, resets = fk.resets;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char *args[] = {"lora", "lrmem", (char *)invalid[i][0], (char *)invalid[i][1]};
        LS_EQ_INT(lr20xx_clock_command(4, args), 1);
        LS_EQ_INT(fk.nframes, before);
    }
    char *args[] = {"lora", "lrmem", "0xF40200"};
    LS_EQ_INT(lr20xx_clock_command(3, args), 0);
    LS_EQ_INT(fk.nframes, before + 2);
    FK_FRAME(before, 0x01, 0x06, 0xF4, 0x02, 0x00, 0x01);
    LS_CHECK(fk.frames[before + 1].read2);
    LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.resets, resets);
    LS_EQ_INT(fk.violations, 0);
    fk_busy_for = 10000;
    LS_EQ_INT(lr20xx_clock_command(3, args), 1);
    LS_EQ_INT(fk.nframes, before + 2);
    LS_EQ_INT(fk.resets, resets);
    fk_busy_for = 0;
    fk.bad_read_stat = true;
    char *many[] = {"lora", "lrmem", "0xF40200", "16"};
    LS_EQ_INT(lr20xx_clock_command(4, many), 1);
    LS_EQ_INT(fk.nframes, before + 4); /* Stop at the first invalid response. */
    fk.bad_read_stat = false;
    const int n = fk.nframes;
    LS_EQ_INT(lr20xx_clock_command(4, many), 0);
    LS_EQ_INT(fk.nframes, n + 32);
    FK_FRAME(n + 30, 0x01, 0x06, 0xF4, 0x02, 0x3C, 0x01);
    LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.resets, resets);
}

LS_CASE(pram_proof_restores_ram_and_rx_on_success_and_failure)
{
    for (int failure = 0; failure < 2; failure++) {
        fk_install(FK_LR);
        fk.model_reset = true;
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        ls_lora_cfg_t cfg;
        ls_lora_cfg_default(&cfg);
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        uint32_t saved[25];
        for (unsigned i = 0; i < 25; i++) saved[i] = fk.proof_ram[i] = 0xABCDE000u + i;
        fk.proof_fail_result = failure;
        const int resets = fk.resets;
        char *args[] = {"lora", "lrproof"};
        LS_EQ_INT(lr20xx_clock_command(2, args), failure);
        LS_CHECK(memcmp(saved, fk.proof_ram, sizeof(saved)) == 0);
        LS_EQ_INT(fk.resets, resets + 1);
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
        LS_EQ_INT(fk.tx_starts, 0);
        LS_EQ_INT(fk.bad_args, 0);
        LS_EQ_INT(fk.bad_mode, 0);
        LS_EQ_INT(fk.bad_reg, 0);
        LS_EQ_INT(fk.violations, 0);
        FK_FRAME(first_op(0x012D), 0x01, 0x2D, 0x00);
        fk.proof_magic = 0x600DB002u;
        const int n = fk.nframes;
        LS_EQ_INT(lr20xx_clock_command(2, args), 1);
        LS_EQ_INT(fk.nframes, n + 2); /* Only the preflight marker read. */
        LS_EQ_INT(fk.resets, resets + 1);
    }
}

LS_CASE(finite_capture_restores_rx_and_ram_after_completion_or_timeout)
{
    for (int timeout = 0; timeout < 2; timeout++) {
        fk_install(FK_LR);
        fk.model_reset = true;
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        ls_lora_cfg_t cfg;
        ls_lora_cfg_default(&cfg);
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        for (unsigned i = 0; i < 256; i++) fk.capture_ram[i] = 0xAABB0000u + i;
        fk.capture_timeout = timeout;
        const int resets = fk.resets;
        char *args[] = {"lora", "lrcap"};
        LS_EQ_INT(lr20xx_clock_command(2, args), timeout);
        for (unsigned i = 0; i < 256; i++) LS_EQ_INT(fk.capture_ram[i], 0xAABB0000u + i);
        LS_EQ_INT(fk.resets, resets + 1);
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
        LS_EQ_INT(fk.tx_starts, 0);
        LS_EQ_INT(fk.bad_args, 0);
        LS_EQ_INT(fk.bad_reg, 0);
        LS_EQ_INT(fk.bad_mode, 0);
        LS_EQ_INT(fk.violations, 0);
        FK_FRAME(first_op(0x0290), 0x02, 0x90, 0xFF, 0, 0, 0, 0, 0x92, 0, 0, 0, 0xFF, 0);
    }
}

LS_CASE(capture_c_api_rejects_bad_buffers_and_active_patch)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    uint32_t words[512]; lr20xx_capture_result_t result;
    int n = fk.nframes;
    LS_EQ_INT(lr20xx_capture_source2(NULL, words, 256, &result), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_capture_source2(words, words+255, 256, &result), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_capture_source2(words, words+256, 255, &result), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_capture_source2(words, words+256, 256, NULL), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, n);
    fk.proof_magic = 0x600DB002u;
    LS_EQ_INT(lr20xx_capture_source2(words, words+256, 256, &result), ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk.nframes, n+2);
    LS_EQ_INT(result.words, 0);
    LS_EQ_INT(result.capture_error, ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk.tx_starts, 0);
}

static bool stream_test_sink(void *context, const uint8_t *bytes, size_t n, uint64_t first)
{
    unsigned *count = context;
    LS_EQ_INT(first, *count / 4);
    LS_EQ_INT(n, 128);
    LS_EQ_INT(bytes[127], 127);
    *count += n;
    return *count < 256;
}

LS_CASE(stream_c_api_cancel_overflow_and_timeout_restore_radio)
{
    for (int failure = 0; failure < 5; failure++) {
        fk_install(FK_LR); fk.model_reset = true;
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg); cfg.bw_hz = 31250;
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        fk.capture_fifo_full = failure == 1;
        fk.capture_fifo_error_flags = failure == 2 ? 0x10 : 0;
        fk.capture_timeout = failure == 3;
        fk.capture_tail_overflow = failure == 4;
        unsigned count = 0; lr20xx_stream_result_t r;
        int resets = fk.resets;
        esp_err_t err = lr20xx_stream_source2(10, stream_test_sink, &count, &r);
        LS_CHECK(failure && failure != 4 ? err != ESP_OK : err == ESP_OK);
        LS_EQ_INT(r.bytes, failure == 1 || failure == 3 ? 0 : 256);
        LS_EQ_INT(r.stopped_by_sink, failure == 0 || failure == 2 || failure == 4);
        if (failure == 4) {
            LS_EQ_INT(r.fifo_flags, 0);
            LS_CHECK(fk.capture_fifo_error_flags & 0x10);
        }
        LS_EQ_INT(r.recovery_error, ESP_OK);
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
        LS_EQ_INT(fk.resets, resets+1);
        LS_EQ_INT(fk.tx_starts, 0);
        LS_EQ_INT(fk.bad_args, 0);
        LS_EQ_INT(fk.violations, 0);
    }
}

typedef struct { unsigned bytes, calls; int resets; bool refuse; } checked_context;
static bool checked_test_sink(void *context, const uint8_t *bytes, size_t n, uint64_t first)
{
    checked_context *c=context; c->calls++;
    LS_EQ_UINT(first,c->bytes/4);
    LS_EQ_INT(fk.resets,c->resets);
    LS_EQ_INT(fk.mode,LR20XX_MODE_RX);
    LS_CHECK(fk.capture_flag_reads && !(fk.capture_fifo_error_flags&0x30));
    LS_CHECK(n && n<=1280 && n%4==0);
    LS_EQ_UINT(bytes[0],0);
    if(c->refuse)return false;
    c->bytes+=(unsigned)n; return true;
}

LS_CASE(checked_stream_crosses_old_window_boundary_without_reset_and_rejects_bad_tail)
{
    for(unsigned scenario=0;scenario<4;scenario++) {
        fk_install(FK_LR); fk.model_reset=true;
        LS_EQ_INT(ls_lora_start(),ESP_OK);
        ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);cfg.bw_hz=31250;
        LS_EQ_INT(ls_lora_configure(&cfg),ESP_OK);
        LS_EQ_INT(ls_lora_receive(),ESP_OK);
        fk.capture_flag_reads=0;
        fk.capture_overflow_at=scenario==1?3:0;
        fk.capture_fifo_full=scenario==3;
        checked_context c={0,0,fk.resets,scenario==2};
        lr20xx_stream_result_t r;
        esp_err_t e=lr20xx_stream_source2_checked(1000,checked_test_sink,&c,&r);
        LS_EQ_UINT(r.validated_bytes,c.bytes);
        LS_EQ_INT(fk.resets,c.resets+1);
        LS_EQ_INT(r.recovery_error,ESP_OK);
        LS_EQ_INT(fk.mode,LR20XX_MODE_RX);
        LS_EQ_INT(fk.tx_starts,0);
        LS_EQ_INT(fk.violations,0);
        if(scenario==0) {
            LS_EQ_INT(e,ESP_OK); LS_CHECK(!r.stopped_by_sink);
            LS_CHECK(r.elapsed_us>=1000000 && c.calls>3);
            LS_EQ_UINT(r.bytes,r.validated_bytes);
        } else if(scenario==1) {
            LS_EQ_INT(e,ESP_ERR_INVALID_RESPONSE);
            LS_EQ_UINT(r.validated_bytes,2048); LS_EQ_UINT(r.bytes,3072);
        } else if(scenario==2) {
            LS_EQ_INT(e,ESP_OK); LS_CHECK(r.stopped_by_sink);
            LS_EQ_UINT(c.calls,1); LS_EQ_UINT(r.validated_bytes,0);
        } else { LS_CHECK(e!=ESP_OK); LS_EQ_UINT(c.calls,0); }
    }
}

LS_CASE(fifo_trigger_wait_is_bounded_and_does_not_transmit)
{
    fk_install(FK_LR);
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg); cfg.bw_hz=31250;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    fk.rssi_hi=125; fk.rssi_lo=0;
    int resets=fk.resets;
    char *quiet[]={"lora","lrfifo","10","hex","-100","10"};
    LS_EQ_INT(lr20xx_clock_command(6,quiet),1);
    LS_EQ_INT(fk.resets,resets);
    LS_EQ_INT(fk.mode,LR20XX_MODE_RX);
    LS_EQ_INT(fk.tx_starts,0);
    char *bad[]={"lora","lrfifo","250","hex","-100","60001"};
    int frames=fk.nframes;
    LS_EQ_INT(lr20xx_clock_command(6,bad),1);
    LS_EQ_INT(fk.nframes,frames);
}

LS_CASE(checked_fifo_console_has_bounds_before_any_radio_io)
{
    fk_install(FK_LR); LS_EQ_INT(ls_lora_start(),ESP_OK);
    int before=fk.nframes;
    char *bad[]={"lora","lrfifo-check","1001"};
    LS_EQ_INT(lr20xx_clock_command(3,bad),1);LS_EQ_INT(fk.nframes,before);
    char *zero[]={"lora","lrfifo-check","0"};
    LS_EQ_INT(lr20xx_clock_command(3,zero),1);LS_EQ_INT(fk.nframes,before);
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
        FK_FRAME(tcxo, 0x01, 0x20, 0x07, 0x00, 0x00, 0x80, 0x00);
        FK_FRAME(reg, 0x01, 0x21, 0x00);
        LS_EQ_INT(fk.violations, 0);
        LS_EQ_INT(fk.bad_args, 0);
        fk_lr_tcxo = false;
        fk_lr_dcdc = true;
        fk_install(FK_LR);
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        LS_EQ_INT(first_op(0x0120), -1);
        LS_CHECK(first_op(0x0121) < first_op(0x0122));
        FK_FRAME(first_op(0x0121), 0x01, 0x21, 0x02);
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
        FK_FRAME(first_op(0x0121), 0x01, 0x21, 0x02);
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
