#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"
#include "ls_lora_priv.h"

/* Extend the existing fake locally to recognize raw route selection. All
   framing, FIFO, gain, mapping, BUSY and recovery behavior uses that fake.
   The shared fake and existing tests are deliberately unchanged. */
static void wide_respond(const uint8_t *tx, uint8_t *rx, size_t n, void *context)
{
    if (!fk.read_pending && n == 13 && tx[0] == 2 && tx[1] == 0x90 && tx[2] != 0xFF) {
        uint8_t saved[13]; memcpy(saved, tx, sizeof(saved));
        LS_EQ_INT(fk.packet_type, 2); LS_EQ_INT(fk.mode, 1);
        const uint8_t source2[] = {2, 0x90, 0xFF, 0, 0, 0, 0, 0x0A, 0, 0, 0, 0, 1};
        const int before = fk.nframes;
        fk_respond(source2, rx, n, context);
        if (fk.nframes > before) memcpy(fk.frames[before].b, saved, sizeof(saved));
        return;
    }
    fk_respond(tx, rx, n, context);
}

static void start_radio(void)
{
    fk_install(FK_LR); fk.model_reset = true;
    LS_EQ_INT(ls_lora_start(), ESP_OK);
    ls_lora_cfg_t cfg; ls_lora_cfg_default(&cfg); cfg.bw_hz = 31250;
    LS_EQ_INT(ls_lora_configure(&cfg), ESP_OK);
    LS_EQ_INT(ls_lora_receive(), ESP_OK);
    ls_shim_spi_on_transfer(wide_respond, &fk);
    fk.nframes = 0;
}

typedef struct {
    uint32_t rate;
    lr20xx_iq_format_t format;
    unsigned calls;
    uint64_t bytes;
} delivery_t;
static bool receive_block(void *context, const uint8_t *data, size_t n, uint64_t first,
        uint32_t rate, lr20xx_iq_format_t format)
{
    delivery_t *d = context;
    LS_CHECK(data && n && n <= 1024);
    LS_EQ_UINT(rate, d->rate); LS_EQ_INT(format, d->format);
    LS_EQ_UINT(first, d->bytes / (format == LR20XX_IQ8 ? 2 : 4));
    LS_CHECK(lr20xx_stream_source2_active());
    LS_CHECK(fk.capture_flag_reads > 0);
    if (++d->calls == 3) return false;
    d->bytes += n;
    return true;
}

LS_CASE(all_routes_deliver_metadata_restore_rx_and_reapply_gain_after_setrx)
{
    const lr20xx_iq_route_t kinds[] = {LR20XX_IQ_SOURCE2, LR20XX_IQ_SOURCE0_576,
        LR20XX_IQ_SOURCE1, LR20XX_IQ_SOURCE1, LR20XX_IQ_SOURCE1, LR20XX_IQ_SOURCE1};
    const uint8_t codes[] = {0, 0x11, 0x1A, 0x12, 0x21, 0x19};
    const uint32_t rates[] = {125000, 576000, 500000, 666667, 800000, 1000000};
    for (unsigned k = 0; k < 6; ++k) {
        start_radio();
        const lr20xx_iq_config_t c = {.route = kinds[k], .code = codes[k],
            .frequency_hz = 154785000, .gain = 7, .path = LR20XX_PATH_LF};
        delivery_t d = {.rate = rates[k], .format = k ? LR20XX_IQ8 : LR20XX_IQ16};
        lr20xx_stream_result_t r;
        LS_EQ_INT(lr20xx_stream(&c, 1000, receive_block, &d, &r), ESP_OK);
        LS_EQ_UINT(d.calls, 3); LS_EQ_UINT(r.validated_bytes, d.bytes);
        /* IQ16 publishes 1024-byte batches (its consumers' size), IQ8 4 KiB. */
        LS_EQ_UINT(r.bytes, k ? 4096 : 3072); LS_EQ_UINT(r.sample_rate_hz, rates[k]);
        LS_EQ_INT(r.format, d.format); LS_CHECK(r.stopped_by_sink);
        LS_EQ_UINT(r.fifo_capacity, 1024); LS_EQ_UINT(r.fifo_flags, 0);
        LS_EQ_INT(r.first_fifo_error_us, -1);
        LS_CHECK(!lr20xx_stream_source2_active()); LS_CHECK(!ls_lora_hw_locked_by_me());
        LS_EQ_INT(fk.mode, LR20XX_MODE_RX); LS_EQ_INT(fk.packet_type, 0);
        LS_EQ_INT(fk.tx_starts, 0); LS_EQ_INT(fk.bad_args, 0); LS_EQ_INT(fk.violations, 0);
        int route = -1, rx = -1, gain = -1, stop = -1;
        for (int i = 0; i < fk.nframes; ++i) {
            if (fk.frames[i].read2) continue;
            const unsigned op = fk.frames[i].b[0] * 256u + fk.frames[i].b[1];
            if (op == 0x0290) { route = i; LS_CHECK(memcmp(fk.frames[i].b + 2, r.route_bytes, 11) == 0); }
            if (route >= 0 && rx < 0 && op == 0x020C) rx = i;
            if (rx >= 0 && gain < 0 && op == 0x021A) { gain = i; LS_EQ_INT(fk.frames[i].b[2], 7); }
            if (rx >= 0 && op == 0x0128) { stop = i; break; }
            if (k && route >= 0) LS_CHECK(op != 0x0221);
        }
        LS_CHECK(route >= 0 && rx > route && gain > rx && stop > gain);
    }
}

LS_CASE(overflow_discards_unpublished_batch_and_restores_ownership)
{
    start_radio(); fk.capture_fifo_error_flags = 0x30;
    const lr20xx_iq_config_t c = {.route = LR20XX_IQ_SOURCE1, .code = 0x21,
        .frequency_hz = 154785000, .gain = 13, .path = LR20XX_PATH_LF};
    delivery_t d = {.rate = 800000, .format = LR20XX_IQ8};
    lr20xx_stream_result_t r;
    LS_EQ_INT(lr20xx_stream(&c, 1000, receive_block, &d, &r), ESP_ERR_INVALID_RESPONSE);
    LS_EQ_UINT(r.validated_bytes, 0); LS_EQ_UINT(d.calls, 0);
    LS_EQ_UINT(r.bytes, 4096); LS_EQ_UINT(r.overflows, 1); LS_EQ_UINT(r.underflows, 1);
    LS_CHECK(r.first_fifo_error_us >= 0); LS_EQ_INT(fk.mode, LR20XX_MODE_RX);
    LS_CHECK(!lr20xx_stream_source2_active()); LS_CHECK(!ls_lora_hw_locked_by_me());
}

LS_CASE(general_api_refuses_bad_configs_and_durations_before_wire_io)
{
    start_radio(); const int before = fk.nframes;
    lr20xx_iq_config_t c = {.route = LR20XX_IQ_SOURCE1, .code = 0x19,
        .frequency_hz = 154785000, .gain = 13, .path = LR20XX_PATH_LF};
    delivery_t d = {0}; lr20xx_stream_result_t r;
    LS_EQ_INT(lr20xx_stream(&c, 0, receive_block, &d, &r), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(lr20xx_stream(&c, 120001, receive_block, &d, &r), ESP_ERR_INVALID_ARG);
    c.gain = 14;
    LS_EQ_INT(lr20xx_stream(&c, 1000, receive_block, &d, &r), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, before); LS_CHECK(!lr20xx_stream_source2_active());
}

LS_CASE(wide_fifo_frame_reads_1020_bytes_in_one_command)
{
    start_radio();
    uint8_t frame[1024] = {0, 1};
    for (unsigned i = 0; i < 1020; ++i) fk.fifo[i] = (uint8_t)(i * 73);
    fk.fifo_n = 1020;
    ls_lora_hw_lock();
    LS_EQ_INT(ls_lora_hw_capture_fast(true), ESP_OK);
    const int before = fk.nframes;
    LS_EQ_INT(ls_lora_hw_capture_frame(frame, 1022), ESP_OK);
    LS_EQ_INT(fk.nframes, before + 1); LS_EQ_UINT(fk.frames[before].full, 1022);
    for (unsigned i = 0; i < 1020; ++i) LS_EQ_INT(frame[i + 2], (uint8_t)(i * 73));
    LS_EQ_INT(fk.fifo_n, 0);
    LS_EQ_INT(ls_lora_hw_capture_fast(false), ESP_OK);
    ls_lora_hw_unlock();
}
