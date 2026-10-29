#include "lora_fakes.h"
#include "ls_lora_lr20xx.h"
#include "ls_lora_priv.h"

/* lr20xx_stream_live on the 576k route with the host gain, as lr_wf_session's
   wide mode runs it: continuous until the sink stops it, the host's auto
   gain starts at step 13 after SetRx, and IQ8 peaks near full scale (the
   fake FIFO counts 0..127) step it down. Same fake and route patch as
   test_lr_iq_routes.c. */
static void wide_respond(const uint8_t *tx, uint8_t *rx, size_t n, void *context)
{
    if (!fk.read_pending && n == 13 && tx[0] == 2 && tx[1] == 0x90 && tx[2] != 0xFF) {
        uint8_t saved[13]; memcpy(saved, tx, sizeof(saved));
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

typedef struct { unsigned calls, stop_at; uint64_t pairs; } sink_t;
static bool receive(void *context, const uint8_t *data, size_t n, uint64_t first,
                    uint32_t rate, lr20xx_iq_format_t format)
{
    sink_t *s = context;
    LS_CHECK(data && n && n <= 1024 && n % 2 == 0);
    LS_EQ_UINT(rate, 576000); LS_EQ_INT(format, LR20XX_IQ8);
    LS_EQ_UINT(first, s->pairs);
    LS_CHECK(lr20xx_stream_source2_active());
    if (++s->calls == s->stop_at) return false;
    s->pairs += n / 2;
    return true;
}

LS_CASE(live_576k_route_runs_until_the_sink_stops_on_host_auto_gain)
{
    start_radio();
    const lr20xx_iq_config_t c = {.route = LR20XX_IQ_SOURCE0_576, .frequency_hz = 152500000,
        .gain = 13, .path = LR20XX_PATH_LF, .host_gain = true};
    sink_t s = {.stop_at = 200};
    lr20xx_stream_result_t r;
    LS_EQ_INT(lr20xx_stream_live(&c, receive, &s, &r), ESP_OK);
    LS_EQ_UINT(s.calls, 200); LS_CHECK(r.stopped_by_sink);
    LS_EQ_UINT(r.validated_bytes, s.pairs * 2);
    LS_EQ_UINT(r.sample_rate_hz, 576000); LS_EQ_INT(r.format, LR20XX_IQ8);
    LS_CHECK(!lr20xx_stream_source2_active()); LS_CHECK(!ls_lora_hw_locked_by_me());
    LS_EQ_INT(fk.mode, LR20XX_MODE_RX); LS_EQ_INT(fk.packet_type, 0);
    LS_EQ_INT(fk.tx_starts, 0); LS_EQ_INT(fk.bad_args, 0); LS_EQ_INT(fk.violations, 0);
    /* After SetRx: the host's start step (13), then lower steps on the peaks. */
    int route = -1, rx = -1, stop = -1, first_gain = -1, lowest = 99, gains = 0;
    for (int i = 0; i < fk.nframes; ++i) {
        if (fk.frames[i].read2) continue;
        const unsigned op = fk.frames[i].b[0] * 256u + fk.frames[i].b[1];
        if (op == 0x0290) route = i;
        if (route >= 0 && rx < 0 && op == 0x020C) rx = i;
        if (rx >= 0 && op == 0x0128) { stop = i; break; }
        if (rx >= 0 && op == 0x021A) {
            if (first_gain < 0) first_gain = fk.frames[i].b[2];
            if (fk.frames[i].b[2] < lowest) lowest = fk.frames[i].b[2];
            ++gains;
        }
    }
    /* The frame log fills long before 200 sink calls end; stop may be past it. */
    (void)stop;
    LS_CHECK(route >= 0 && rx > route);
    LS_EQ_INT(first_gain, 13);
    LS_CHECK_MSG(gains >= 2 && lowest < 13, "gain writes=%d lowest=%d", gains, lowest);
}

LS_CASE(live_route_without_host_gain_keeps_its_fixed_step)
{
    start_radio();
    const lr20xx_iq_config_t c = {.route = LR20XX_IQ_SOURCE0_576, .frequency_hz = 152500000,
        .gain = 9, .path = LR20XX_PATH_LF};
    sink_t s = {.stop_at = 200};
    lr20xx_stream_result_t r;
    LS_EQ_INT(lr20xx_stream_live(&c, receive, &s, &r), ESP_OK);
    int rx = -1, gains = 0;
    for (int i = 0; i < fk.nframes; ++i) {
        if (fk.frames[i].read2) continue;
        const unsigned op = fk.frames[i].b[0] * 256u + fk.frames[i].b[1];
        if (rx < 0 && op == 0x020C) rx = i;
        if (rx >= 0 && op == 0x0128) break;
        if (rx >= 0 && op == 0x021A) { LS_EQ_INT(fk.frames[i].b[2], 9); ++gains; }
    }
    LS_EQ_INT(gains, 1);
}

LS_CASE(live_route_refuses_a_second_owner_and_bad_config)
{
    start_radio();
    lr20xx_stream_result_t r;
    sink_t s = {.stop_at = 1};
    LS_EQ_INT(lr20xx_stream_live(NULL, receive, &s, &r), ESP_ERR_INVALID_ARG);
    const lr20xx_iq_config_t bad = {.route = LR20XX_IQ_SOURCE0_576, .frequency_hz = 1000,
        .gain = 13, .path = LR20XX_PATH_LF, .host_gain = true};
    const int before = fk.nframes;
    LS_EQ_INT(lr20xx_stream_live(&bad, receive, &s, &r), ESP_ERR_INVALID_ARG);
    LS_EQ_INT(fk.nframes, before); LS_CHECK(!lr20xx_stream_source2_active());
}
