/* LR2021 init and receive diagnostics against the shared SPI fake. */
#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"
#include "ls_test.h"
#include "ls_lora_priv.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"

static bool s_test_field_owned, s_test_mesh_wanted, s_test_mesh_held;
bool ls_mesh_radio_wanted(void) {return s_test_mesh_wanted;}
bool ls_mesh_radio_held(void) {return s_test_mesh_held;}
bool ls_field_owned(void) { return s_test_field_owned; }

/* Exercise the production CPU transport against register operations on host.
   The protocol fake below separately checks the LR FIFO command boundaries. */
typedef struct {
    uint8_t data[64];
    bool rx_dma, tx_dma, mosi, miso, keep_cs, done;
    size_t tx_bits, rx_bits;
    unsigned starts;
} spi_dev_t;
static spi_dev_t cpu_hw;
#define SPI_LL_GET_HW(host) (&cpu_hw)
#define CPU_SET(name, field, type) static void name(spi_dev_t *h, type v) { h->field = v; }
CPU_SET(spi_ll_dma_rx_enable, rx_dma, bool)
CPU_SET(spi_ll_dma_tx_enable, tx_dma, bool)
CPU_SET(spi_ll_enable_mosi, mosi, bool)
CPU_SET(spi_ll_enable_miso, miso, bool)
CPU_SET(spi_ll_set_mosi_bitlen, tx_bits, size_t)
CPU_SET(spi_ll_set_miso_bitlen, rx_bits, size_t)
CPU_SET(spi_ll_master_keep_cs, keep_cs, bool)
static void spi_ll_clear_int_stat(spi_dev_t *h) { h->done = false; }
static void spi_ll_write_buffer(spi_dev_t *h, const uint8_t *p, size_t bits)
{ LS_CHECK(bits <= 512); memcpy(h->data, p, bits / 8); }
static void spi_ll_apply_config(spi_dev_t *h) { (void)h; }
static void spi_ll_user_start(spi_dev_t *h)
{
    LS_CHECK(!h->rx_dma && !h->tx_dma && h->mosi && h->miso && !h->keep_cs);
    LS_EQ_UINT(h->rx_bits, h->tx_bits);
    for (size_t i = 0; i < h->rx_bits / 8; ++i) h->data[i] ^= 0xA5;
    h->done = true; h->starts++;
}
static bool spi_ll_usr_is_done(spi_dev_t *h) { return h->done; }
static void spi_ll_read_buffer(spi_dev_t *h, uint8_t *p, size_t bits)
{ LS_CHECK(h->done); memcpy(p, h->data, bits / 8); }
#include "ls_lora_spi_cpu.h"

LS_CASE(cpu_transport_disables_dma_for_inline_and_full_fifo_transactions)
{
    for (size_t n = 1; n <= 64; ++n) {
        uint8_t data[68]; memset(data, 0x39, sizeof(data));
        spi_transaction_t t = { .length = n * 8, .tx_buffer = data, .rx_buffer = data };
        if (n <= 4) {
            t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
            memset(t.tx_data, 0x39, sizeof(t.tx_data));
        }
        cpu_hw.rx_dma = cpu_hw.tx_dma = true;
        LS_EQ_INT(capture_cpu_transmit(&t), ESP_OK);
        for (size_t i = 0; i < n; ++i) LS_EQ_INT(n <= 4 ? t.rx_data[i] : data[i], 0x9C);
        LS_EQ_INT(data[n], 0x39);
    }
    unsigned before = cpu_hw.starts;
    spi_transaction_t t = { .length = 65 * 8 };
    LS_EQ_INT(capture_cpu_transmit(&t), ESP_ERR_INVALID_SIZE);
    LS_EQ_UINT(cpu_hw.starts, before);
}

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

static unsigned live_waits;
static bool live_snapshot_guard_checked;
static void live_wait_observe(void)
{
    fk_capture_advance();
    /* The loop sleeps on headroom at every rate (500 kB/s included), so the
       level at a sleep may exceed 256 bytes. What must hold: a sleep is never
       longer than the wake cap, and woken on time it never fills the FIFO
       (500 B/ms here, 2 us a byte). */
    const int64_t sleep_us = ls_shim_timer_wait_remaining();
    LS_CHECK(sleep_us > 0 && sleep_us <= (int64_t)LR20XX_LIVE_WAKE_US);
    LS_CHECK((unsigned)fk.fifo_n < 1024u);
    LS_CHECK(sleep_us <= (int64_t)(1024u - (unsigned)fk.fifo_n) * 2);
    ++live_waits;
}
static bool live_timed_sink(void *context, const uint8_t *bytes, size_t n, uint64_t first)
{
    unsigned *total = context;
    (void)bytes;
    if (!live_snapshot_guard_checked) {
        live_snapshot_guard_checked = true;
        LS_CHECK(lr20xx_stream_source2_active());
        LS_CHECK(!ls_lora_diagnostic_try_begin());
        LS_CHECK(ls_lora_hw_locked_by_me()); /* rejection must preserve stream ownership */
    }
    LS_EQ_UINT(first, *total / 4);
    *total += n;
    return *total < 50000;
}
LS_CASE(live_stream_subtick_wait_drains_continuous_fifo_despite_scheduling_delay)
{
    /* The loop sleeps at most 400 us and at most a quarter of the headroom, so a
       wake 1000 us late survives at 500 kB/s; 2500 us late does not. */
    const unsigned delays[] = {0, 1000, 2500};
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        const unsigned late = delays[scenario];
        fk_install(FK_LR); fk.model_reset = true;
        LS_EQ_INT(ls_lora_start(), ESP_OK);
        ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg); cfg.bw_hz = 31250;
        LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
        LS_EQ_INT(ls_lora_receive(), ESP_OK);
        fk.capture_timed = true; fk_busy_step_us = 1;
        fk.capture_updated = esp_timer_get_time();
        live_waits = 0;
        live_snapshot_guard_checked = false;
        ls_shim_timer_wait_hook(live_wait_observe, late);
        unsigned total = 0; lr20xx_stream_result_t r;
        esp_err_t err = lr20xx_stream_source2_live(live_timed_sink, &total, &r);
        ls_shim_timer_wait_hook(NULL, 0);
        LS_EQ_UINT(ls_shim_timer_count(), 0);
        LS_CHECK(!lr20xx_stream_source2_active());
        LS_CHECK(ls_lora_diagnostic_try_begin());
        ls_lora_diagnostic_try_end();
        LS_CHECK(!ls_lora_hw_locked_by_me());
        if (late > 1100) {
            LS_EQ_INT(err, ESP_ERR_INVALID_SIZE);
            LS_CHECK(r.fifo_flags & 0x30);
            LS_CHECK(r.max_fifo_level >= r.fifo_capacity);
            LS_EQ_UINT(r.validated_bytes, 0);
            LS_EQ_UINT(r.max_wake_latency_us, late);
            continue;
        }
        LS_EQ_INT(err, ESP_OK);
        LS_CHECK(r.stopped_by_sink);
        LS_CHECK(r.max_drain_us > 0 && r.max_drain_us < r.max_service_gap_us);
        LS_CHECK(r.worst_drain_spi_bytes >= 6);
        LS_CHECK(r.max_drain_spi_bytes >= r.worst_drain_spi_bytes);
        LS_CHECK(r.max_drain_spi_bytes < 1050); /* FIFO payload + wire headers/flags. */
        LS_EQ_UINT(r.wakes, live_waits);
        LS_CHECK(r.wakes > 20);
        LS_CHECK(r.max_wake_fifo_level > 0);
        LS_EQ_UINT(r.max_wake_latency_us, late);
        LS_CHECK(fk.capture_peak < 1024);
        LS_EQ_UINT(r.fifo_flags, 0);
        LS_CHECK(fk.capture_high_residual_polls > r.wakes);
        /* Blocking gives lower-priority tasks/idle an opportunity every cycle;
           normal simulated acquisition spends more than half its time asleep. */
        if (!late) LS_CHECK((uint64_t)r.wakes * LR20XX_LIVE_WAKE_US > (uint64_t)r.elapsed_us / 2);
        LS_EQ_UINT(ls_shim_timer_count(), 0);
        LS_CHECK(!lr20xx_stream_source2_active());
        if (late <= 1100) LS_CHECK(live_snapshot_guard_checked);
        ls_shim_timer_wait_hook(NULL, 0);
    }
}

LS_CASE(capture_fifo_chunks_preserve_bytes_and_wire_counters_then_restore_packets)
{
    fk_install(FK_LR); LS_EQ_INT(ls_lora_start(), ESP_OK);
    uint8_t expected[500], actual[500];
    for (unsigned i = 0; i < sizeof(expected); ++i) expected[i] = (uint8_t)(i * 73 + 9);
    ls_lora_hw_lock();
    for (int capture = 1; capture >= 0; --capture) {
        if (capture) LS_EQ_INT(ls_lora_hw_capture_fast(true), ESP_OK);
        memcpy(fk.fifo, expected, sizeof(expected)); fk.fifo_n = sizeof(expected);
        int before = fk.nframes;
        uint32_t bytes = ls_lora_hw_spi_bytes();
        LS_EQ_INT(lr20xx_read_rx_fifo(actual, 256), ESP_OK);
        LS_EQ_INT(lr20xx_read_rx_fifo(actual + 256, 244), ESP_OK);
        LS_CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
        LS_EQ_INT(fk.fifo_n, 0);
        LS_EQ_INT(fk.nframes - before, capture ? 9 : 2);
        LS_EQ_UINT(ls_lora_hw_spi_bytes() - bytes, capture ? 518 : 504);
        for (int i = before; i < fk.nframes; ++i) {
            LS_CHECK(fk.frames[i].full <= (capture ? 64u : 258u));
            LS_EQ_INT(fk.frames[i].b[0], 0);
            LS_EQ_INT(fk.frames[i].b[1], 1);
        }
        if (capture) LS_EQ_INT(ls_lora_hw_capture_fast(false), ESP_OK);
    }
    ls_lora_hw_unlock();
}

/* Keep the upload exercised through the public command dispatcher: this links
   the same include and radio-lock path that ships in the board driver. */
static int tx_upload_call(int argc, char **argv)
{
    return lr20xx_clock_command(argc, argv);
}

static uint32_t tx_test_crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xEDB88320u);
    }
    return ~crc;
}

LS_CASE(tx_upload_is_lazy_rejects_allocation_failure_and_abort_releases_buffer)
{
    extern void ls_shim_heap_reset(void);
    extern void ls_shim_heap_fail_from(unsigned call_index);
    extern unsigned ls_shim_heap_call_count(void);
    extern unsigned ls_shim_heap_outstanding(void);
    extern unsigned ls_shim_heap_call_caps(unsigned call_index);
    extern size_t ls_shim_heap_call_size(unsigned call_index);
    fk_install(FK_LR); LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_shim_heap_reset();
    char *status[] = {"lora", "lrtx-upload", "status"};
    LS_EQ_INT(tx_upload_call(3, status), 0);
    LS_EQ_UINT(ls_shim_heap_call_count(), 0); /* status is allocation-free */

    char *begin[] = {"lora", "lrtx-upload", "begin", "2", "0x41D912FF"};
    ls_shim_heap_fail_from(1);
    LS_EQ_INT(tx_upload_call(5, begin), 1);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
    ls_shim_heap_reset();
    LS_EQ_INT(tx_upload_call(5, begin), 0);
    LS_EQ_UINT(ls_shim_heap_call_count(), 1);
    LS_EQ_UINT(ls_shim_heap_call_caps(1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    LS_EQ_UINT(ls_shim_heap_call_size(1), 2); /* allocate the requested waveform only */
    char *abort[] = {"lora", "lrtx-upload", "abort"};
    LS_EQ_INT(tx_upload_call(3, abort), 0);
    LS_EQ_UINT(ls_shim_heap_outstanding(), 0);
}

LS_CASE(tx_upload_rejects_bad_order_hex_crc_and_amplitude_then_commits_valid_iq)
{
    fk_install(FK_LR); LS_EQ_INT(ls_lora_start(), ESP_OK);
    char *begin[] = {"lora", "lrtx-upload", "begin", "2", "0x41D912FF"}; /* CRC32(00 00) */
    char *wrong_offset[] = {"lora", "lrtx-upload", "chunk", "2", "0000"};
    char *bad_hex[] = {"lora", "lrtx-upload", "chunk", "0", "00xz"};
    char *chunk[] = {"lora", "lrtx-upload", "chunk", "0", "0000"};
    char *commit[] = {"lora", "lrtx-upload", "commit"};
    char *abort[] = {"lora", "lrtx-upload", "abort"};
    LS_EQ_INT(tx_upload_call(5, begin), 0);
    LS_EQ_INT(tx_upload_call(5, wrong_offset), 1);
    LS_EQ_INT(tx_upload_call(5, bad_hex), 1);
    LS_EQ_INT(tx_upload_call(3, commit), 1); /* no bytes accepted */
    LS_EQ_INT(tx_upload_call(5, chunk), 0);
    LS_EQ_INT(tx_upload_call(3, commit), 0);
    LS_EQ_INT(tx_upload_call(5, chunk), 1); /* committed uploads are immutable */

    /* A structurally valid transfer with a wrong CRC cannot be committed. */
    char *bad_crc_begin[] = {"lora", "lrtx-upload", "begin", "2", "0"};
    LS_EQ_INT(tx_upload_call(5, bad_crc_begin), 0);
    LS_EQ_INT(tx_upload_call(5, chunk), 0);
    LS_EQ_INT(tx_upload_call(3, commit), 1);

    /* CRC-valid but over-limit magnitude is rejected at commit. */
    char *large_begin[] = {"lora", "lrtx-upload", "begin", "2", "0x4F5D9F45"}; /* 57,0 */
    char *large_chunk[] = {"lora", "lrtx-upload", "chunk", "0", "3900"};
    LS_EQ_INT(tx_upload_call(5, large_begin), 0);
    LS_EQ_INT(tx_upload_call(5, large_chunk), 0);
    LS_EQ_INT(tx_upload_call(3, commit), 1);
    LS_EQ_INT(tx_upload_call(3, abort), 0);
}

LS_CASE(tx_send_without_a_committed_upload_refuses_before_touching_radio)
{
    fk_install(FK_LR); LS_EQ_INT(ls_lora_start(), ESP_OK);
    const int before = fk.tx_frames;
    char *send[] = {"lora", "lrtx-send", "9600"};
    LS_EQ_INT(lr20xx_clock_command(3, send), 1);
    LS_EQ_INT(fk.tx_frames, before);
    LS_EQ_INT(fk.bad_mode, 0);
}

LS_CASE(tx_send_commits_exact_waveform_and_respects_fast_spi_fifo_frame_bound)
{
    fk_install(FK_LR); fk_busy_step_us = 1; LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    LS_CHECK(!ls_lora_hw_capture_active());
    ls_lora_hw_lock();
    LS_CHECK(ls_lora_hw_locked_by_me());
    LS_EQ_INT(ls_lora_hw_capture_fast(true), ESP_OK);
    LS_EQ_INT(ls_lora_hw_capture_fast(false), ESP_OK);
    ls_lora_hw_unlock();
    LS_CHECK(!ls_lora_hw_capture_active());
    uint8_t wave[70];
    for (unsigned sample = 0; sample < sizeof(wave) / 2; ++sample) {
        wave[2 * sample] = (uint8_t)(int8_t)((int)(sample % 31u) - 15);
        wave[2 * sample + 1] = (uint8_t)(int8_t)((int)((sample * 3u) % 21u) - 10);
    }
    char size_arg[8], crc_arg[16];
    snprintf(size_arg, sizeof(size_arg), "%u", (unsigned)sizeof(wave));
    snprintf(crc_arg, sizeof(crc_arg), "0x%08lX", (unsigned long)tx_test_crc32(wave, sizeof(wave)));
    char *begin[] = {"lora", "lrtx-upload", "begin", size_arg, crc_arg};
    LS_EQ_INT(lr20xx_clock_command(5, begin), 0);
    for (unsigned offset = 0; offset < sizeof(wave);) {
        char offset_arg[16], hex[65];
        const unsigned amount = sizeof(wave) - offset > 32u ? 32u : (unsigned)sizeof(wave) - offset;
        snprintf(offset_arg, sizeof(offset_arg), "%u", offset);
        for (unsigned i = 0; i < amount; ++i)
            snprintf(hex + 2u * i, sizeof(hex) - 2u * i, "%02X", wave[offset + i]);
        char *chunk[] = {"lora", "lrtx-upload", "chunk", offset_arg, hex};
        LS_EQ_INT(lr20xx_clock_command(5, chunk), 0);
        offset += amount;
    }
    char *commit[] = {"lora", "lrtx-upload", "commit"};
    char *send[] = {"lora", "lrtx-send", "4800", "3"};
    LS_EQ_INT(lr20xx_clock_command(3, commit), 0);
    fk.tx_auto_done = true;
    const int frame_start = fk.nframes;
    extern unsigned ls_shim_heap_call_count(void);
    const unsigned heap_calls_before_send = ls_shim_heap_call_count();
    LS_EQ_INT(lr20xx_clock_command(4, send), 0);
    LS_EQ_UINT(ls_shim_heap_call_count(), heap_calls_before_send); /* sending reuses the uploaded waveform */
    LS_EQ_UINT(fk.tx_starts, 1);
    LS_EQ_UINT(fk.tx_hf, 0);
    LS_EQ_UINT(fk.tx_samples, 105);
    LS_EQ_UINT(fk.tx_rate, 4800);
    LS_EQ_UINT(fk.tx_fifo_base, 0x804000u);
    LS_EQ_UINT(fk.tx_fifo_size, 0x3FCu);
    LS_EQ_UINT(fk.txfifo_n, 210);
    for (int i = 0; i < fk.txfifo_n; ++i) LS_EQ_UINT(fk.txfifo[i], wave[i % sizeof(wave)]);
    unsigned fifo_writes = 0;
    for (int i = frame_start; i < fk.nframes; ++i) {
        const fk_frame_t *frame = &fk.frames[i];
        if (frame->len >= 2 && frame->b[0] == 0 && frame->b[1] == 2) {
            fifo_writes++;
            LS_CHECK(frame->full <= 64u);
            LS_CHECK(frame->full > 2u);
        }
    }
    LS_EQ_UINT(fifo_writes, 6); /* periodic boundary plus 62-byte CPU transport limit */
    LS_CHECK(!ls_lora_hw_capture_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX); /* recovered to the caller's prior RX mode */
    LS_EQ_INT(fk.violations, 0);
}

LS_CASE(tx_send_cancellation_uses_recovery_and_does_not_poison_next_send)
{
    fk_install(FK_LR); fk_busy_step_us = 1; LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    uint8_t wave[1024] = {0};
    char size_arg[8], crc_arg[16];
    snprintf(size_arg, sizeof(size_arg), "%u", (unsigned)sizeof(wave));
    snprintf(crc_arg, sizeof(crc_arg), "0x%08lX", (unsigned long)tx_test_crc32(wave, sizeof(wave)));
    char *begin[] = {"lora", "lrtx-upload", "begin", size_arg, crc_arg};
    LS_EQ_INT(lr20xx_clock_command(5, begin), 0);
    for (unsigned offset = 0; offset < sizeof(wave);) {
        char offset_arg[16], hex[65];
        const unsigned amount = sizeof(wave) - offset > 32u ? 32u : (unsigned)sizeof(wave) - offset;
        snprintf(offset_arg, sizeof(offset_arg), "%u", offset);
        for (unsigned i = 0; i < amount; ++i)
            snprintf(hex + 2u * i, sizeof(hex) - 2u * i, "%02X", wave[offset + i]);
        char *chunk[] = {"lora", "lrtx-upload", "chunk", offset_arg, hex};
        LS_EQ_INT(lr20xx_clock_command(5, chunk), 0);
        offset += amount;
    }
    char *commit[] = {"lora", "lrtx-upload", "commit"};
    char *send[] = {"lora", "lrtx-send", "4800"};
    LS_EQ_INT(lr20xx_clock_command(3, commit), 0);
    fk.tx_auto_done = true;

    /* A stop request arriving during the initial FIFO fill must prevent SetTx
       and take the ordinary recovery path. */
    fk.tx_cancel_on_fifo_write = 1;
    LS_EQ_INT(lr20xx_clock_command(3, send), 1);
    LS_EQ_UINT(fk.tx_fifo_write_count, 1);
    LS_EQ_INT(fk.tx_starts, 0);
    LS_CHECK(!ls_lora_hw_capture_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.violations, 0);

    /* A generation observed by a later send is current, not a stale cancel. */
    fk.tx_cancel_on_fifo_write = 0;
    fk.tx_fifo_write_count = fk.tx_fifo_total_written = 0;
    fk.irq = 0;
    LS_EQ_INT(lr20xx_clock_command(3, send), 0);
    LS_EQ_INT(fk.tx_starts, 1);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);

    /* Cancel on the first refill chunk after SetTx. The next FIFO chunk must
       not be queued, and recovery must leave a subsequent send usable. */
    fk.tx_fifo_write_count = fk.tx_fifo_total_written = 0;
    fk.irq = 0;
    fk.tx_cancel_on_fifo_write = 16; /* 15 chunks prime 896 bytes; #16 is refill */
    LS_EQ_INT(lr20xx_clock_command(3, send), 1);
    LS_EQ_UINT(fk.tx_fifo_write_count, 16);
    LS_EQ_INT(fk.tx_starts, 2);
    LS_CHECK(!ls_lora_hw_capture_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.violations, 0);

    fk.tx_cancel_on_fifo_write = 0;
    fk.tx_fifo_write_count = fk.tx_fifo_total_written = 0;
    fk.irq = 0;
    LS_EQ_INT(lr20xx_clock_command(3, send), 0);
    LS_EQ_INT(fk.tx_starts, 3);
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    char *abort[] = {"lora", "lrtx-upload", "abort"};
    LS_EQ_INT(lr20xx_clock_command(3, abort), 0);
}

LS_CASE(tx_send_setup_failure_releases_radio_lock_and_recovers_receive)
{
    fk_install(FK_LR); fk_busy_step_us = 1; LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    char *begin[] = {"lora", "lrtx-upload", "begin", "2", "0x41D912FF"};
    char *chunk[] = {"lora", "lrtx-upload", "chunk", "0", "0000"};
    char *commit[] = {"lora", "lrtx-upload", "commit"};
    char *send[] = {"lora", "lrtx-send", "4800"};
    LS_EQ_INT(lr20xx_clock_command(5, begin), 0);
    LS_EQ_INT(lr20xx_clock_command(5, chunk), 0);
    LS_EQ_INT(lr20xx_clock_command(3, commit), 0);
    fk.fail_op = 0x0293; /* packet-rate programming fails before FIFO/TX setup */
    LS_EQ_INT(lr20xx_clock_command(3, send), 1);
    fk.fail_op = 0;
    LS_CHECK(!ls_lora_hw_capture_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.tx_starts, 0);
    LS_EQ_INT(fk.violations, 0);
    /* A new public receive operation proves the bus lock was released. */
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
}

LS_CASE(tx_send_refuses_when_field_owner_holds_radio_without_touching_tx)
{
    fk_install(FK_LR); fk_busy_step_us = 1; LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    char *begin[] = {"lora", "lrtx-upload", "begin", "2", "0x41D912FF"};
    char *chunk[] = {"lora", "lrtx-upload", "chunk", "0", "0000"};
    char *commit[] = {"lora", "lrtx-upload", "commit"};
    char *send[] = {"lora", "lrtx-send", "4800"};
    LS_EQ_INT(lr20xx_clock_command(5, begin), 0);
    LS_EQ_INT(lr20xx_clock_command(5, chunk), 0);
    LS_EQ_INT(lr20xx_clock_command(3, commit), 0);
    const int frames = fk.nframes;
    const int tx_frames = fk.tx_frames;
    s_test_field_owned = true;
    LS_EQ_INT(lr20xx_clock_command(3, send), 1);
    s_test_field_owned = false;
    LS_EQ_INT(fk.nframes, frames);
    LS_EQ_INT(fk.tx_frames, tx_frames);
    char *abort[] = {"lora", "lrtx-upload", "abort"};
    LS_EQ_INT(lr20xx_clock_command(3, abort), 0);
}

LS_CASE(tx_send_start_failure_after_fifo_load_releases_fast_mode_and_recovers_rx)
{
    fk_install(FK_LR); fk_busy_step_us = 1; LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    char *begin[] = {"lora", "lrtx-upload", "begin", "2", "0x41D912FF"};
    char *chunk[] = {"lora", "lrtx-upload", "chunk", "0", "0000"};
    char *commit[] = {"lora", "lrtx-upload", "commit"};
    char *send[] = {"lora", "lrtx-send", "4800"};
    LS_EQ_INT(lr20xx_clock_command(5, begin), 0);
    LS_EQ_INT(lr20xx_clock_command(5, chunk), 0);
    LS_EQ_INT(lr20xx_clock_command(3, commit), 0);
    fk.fail_op = 0x020D; /* SetTx is refused after the initial FIFO load */
    LS_EQ_INT(lr20xx_clock_command(3, send), 1);
    fk.fail_op = 0;
    LS_EQ_INT(fk.tx_starts, 0);
    LS_EQ_UINT(fk.txfifo_n, 2);
    LS_CHECK(!ls_lora_hw_capture_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    char *abort[] = {"lora", "lrtx-upload", "abort"};
    LS_EQ_INT(lr20xx_clock_command(3, abort), 0);
}

LS_CASE(tx_send_refill_status_failure_restores_radio_and_releases_fast_spi)
{
    fk_install(FK_LR); fk_busy_step_us = 1; LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg);
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    uint8_t wave[1024] = {0};
    char size_arg[8], crc_arg[16];
    snprintf(size_arg, sizeof(size_arg), "%u", (unsigned)sizeof(wave));
    snprintf(crc_arg, sizeof(crc_arg), "0x%08lX", (unsigned long)tx_test_crc32(wave, sizeof(wave)));
    char *begin[] = {"lora", "lrtx-upload", "begin", size_arg, crc_arg};
    LS_EQ_INT(lr20xx_clock_command(5, begin), 0);
    for (unsigned offset = 0; offset < sizeof(wave);) {
        char offset_arg[16], hex[65];
        const unsigned amount = sizeof(wave) - offset > 32u ? 32u : (unsigned)sizeof(wave) - offset;
        snprintf(offset_arg, sizeof(offset_arg), "%u", offset);
        for (unsigned i = 0; i < amount; ++i)
            snprintf(hex + 2u * i, sizeof(hex) - 2u * i, "%02X", wave[offset + i]);
        char *chunk[] = {"lora", "lrtx-upload", "chunk", offset_arg, hex};
        LS_EQ_INT(lr20xx_clock_command(5, chunk), 0);
        offset += amount;
    }
    char *commit[] = {"lora", "lrtx-upload", "commit"};
    char *send[] = {"lora", "lrtx-send", "4800", "2"};
    LS_EQ_INT(lr20xx_clock_command(3, commit), 0);
    fk.tx_auto_done = true;
    fk.tx_fifo_fail_on_write = 16; /* inject CMD_PERR on the first refill after 896 primed bytes */
    LS_EQ_INT(lr20xx_clock_command(4, send), 1);
    LS_EQ_UINT(fk.tx_fifo_write_count, 17); /* Stat returns on the next SPI frame */
    LS_EQ_UINT(fk.tx_starts, 1);
    LS_CHECK(!ls_lora_hw_capture_active());
    LS_EQ_UINT(fk.mode, LR20XX_MODE_RX);
    LS_EQ_INT(fk.violations, 0);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    char *abort[] = {"lora", "lrtx-upload", "abort"};
    LS_EQ_INT(lr20xx_clock_command(3, abort), 0);
}

LS_CASE(rec_iq_sender_accepts_own_lease_and_rejects_competing_owner)
{
    fk_install(FK_LR);fk_busy_step_us=1;LS_EQ_INT(ls_lora_start(),ESP_OK);
    ls_lora_cfg_t cfg;ls_lora_cfg_default(&cfg);LS_EQ_INT(ls_lora_configure(&cfg),ESP_OK);
    uint8_t wave[]={20,0,0,20};int frames=fk.nframes;
    s_test_mesh_wanted=true;s_test_mesh_held=false;
    LS_EQ_INT(ls_lora_iq_send(wave,sizeof(wave),433920000,-9),ESP_ERR_INVALID_STATE);
    LS_EQ_INT(fk.nframes,frames);
    s_test_mesh_held=true;fk.tx_auto_done=true;
    LS_EQ_INT(ls_lora_iq_send(wave,sizeof(wave),433920000,-9),ESP_OK);
    LS_EQ_UINT(fk.tx_freq_hz,433920000);
    s_test_mesh_wanted=s_test_mesh_held=false;
}

LS_CASE(rec_iq_maximum_recording_streams_all_pairs)
{
    static uint8_t wave[32767*2];
    for(unsigned i=0;i<sizeof(wave);i+=2){wave[i]=20;wave[i+1]=0;}
    fk_install(FK_LR);fk_busy_step_us=1;LS_EQ_INT(ls_lora_start(),ESP_OK);
    ls_lora_cfg_t cfg;ls_lora_cfg_default(&cfg);LS_EQ_INT(ls_lora_configure(&cfg),ESP_OK);
    fk.tx_auto_done=true;
    LS_EQ_INT(ls_lora_iq_send(wave,sizeof(wave),433920000,10),ESP_OK);
    LS_EQ_UINT(fk.tx_samples,32767);LS_EQ_UINT(fk.tx_rate,125000);
    LS_EQ_UINT(fk.tx_fifo_total_written,sizeof(wave));LS_EQ_UINT(fk.tx_hf,0);
    LS_CHECK(!ls_lora_hw_capture_active());LS_CHECK(!ls_lora_hw_locked_by_me());
}
