#include "esp_attr.h"
/* Capture yields between batches on core 0; RX owner runs IQ DSP on core 1. */
#include "lr2021_iq_queue.h"
#include "lr_iq_spectrum.h"
#include "lr_wf_session.h"
#include "p25_lr_frontend.h"
#include "p25_lr_wide.h"
#include "p25_lr_iq_record.inc"
typedef struct {
    lr2021_iq_queue queue;
    atomic_bool stop, done, full;
    lr20xx_stream_result_t result;
    esp_err_t error;
    uint32_t generation;
} p25_lr_capture_t;

static bool IRAM_ATTR p25_lr_iq_collect(void *context, const uint8_t *data, size_t bytes,
                              uint64_t first_pair)
{
    p25_lr_capture_t *c = context;
    if (atomic_load(&c->stop)) return false;
    if (lr2021_iq_queue_push(&c->queue, data, bytes, first_pair, c->generation)) return true;
    atomic_store(&c->full, true);
    return false;
}
static void p25_lr_capture_task(void *arg)
{
    p25_lr_capture_t *c = arg;
    c->error = lr20xx_stream_source2_live(p25_lr_iq_collect, c, &c->result);
    /* No context access after publication: owner may free it. */
    atomic_store_explicit(&c->done, true, memory_order_release);
    vTaskDelete(NULL);
}
static bool p25_lr_iq_wanted(void)
{
    return s_app_active && atomic_load(&s_lr_iq_requested) &&
           !ls_mesh_radio_wanted() && p25_lora_wanted();
}

/* Wide or narrow for the channel asked for now. A channel whose wide LO
   falls off the band (the band's bottom edge) stays narrow. */
static uint32_t s_lr_wide_refused_hz;
static bool p25_lr_wide_now(void)
{
    return p25_lr_wide_pick(p25_lr_wide_pref(), lr_wf_wide_on()) &&
           s_tune_freq_hz != s_lr_wide_refused_hz;
}
/* A hop (first start or retune) is timed from the request to the first
   block of the new channel, per path. */
static int64_t s_lr_hop_t0;
static void p25_lr_hop_landed(bool wide)
{
    if (!s_lr_hop_t0) return;
    const uint32_t us = (uint32_t)(esp_timer_get_time() - s_lr_hop_t0);
    s_lr_hop_t0 = 0;
    P25.lr_iq_hops[wide]++; P25.lr_iq_hop_us[wide] = us;
    if (us > P25.lr_iq_hop_max_us[wide]) P25.lr_iq_hop_max_us[wide] = us;
}

/* The per-generation decoder state both paths start from. */
static void p25_lr_generation_begin(void)
{
    p25_decoder_retuned(s_tune_freq_hz);
    dsp_init(&s_dsp); dsp_set_mode(&s_dsp, DEMOD_C4FM);
    dsp_set_gain(&s_dsp, p25_demod_output_gain(DEMOD_C4FM, P25.demod_invert));
    P25.demod_active = DEMOD_C4FM; P25.demod_auto = false;
}
typedef struct {
    p25_lr_frontend_t *frontend;
    float *iq;
    int16_t *audio;
    uint64_t expected, received, audio_received;
    int64_t rate_start;
    int64_t fe_us, dsp_us;        /* this second's front end and demod time */
} p25_lr_feed_t;
/* One checked block: front end, release, demodulator, decoder ring. False
   when the run is broken (a gap, a bad block, no room in the ring). */
static bool p25_lr_block(p25_lr_feed_t *f, lr2021_iq_queue *q, const lr2021_iq_block *b,
                         uint32_t generation)
{
    if (b->generation != generation || b->first_pair != f->expected) return false;
    p25_lr_record_feed(b);
    const int64_t t0 = esp_timer_get_time();
    size_t pairs = p25_lr_frontend_process(f->frontend, b->data, b->bytes, f->iq, 640);
    const int64_t t1 = esp_timer_get_time();
    f->fe_us += t1 - t0;
    if (pairs == SIZE_MAX) return false;
    float peak = 0;
    for (size_t k = 0; k < 2 * pairs; k += 4) {
        float v = fmaxf(fabsf(f->iq[k]), fabsf(f->iq[k + 1]));
        if (v > peak) peak = v;
    }
    P25.iq_level = 0.6f * P25.iq_level + 0.4f * peak;
    p25_rx_power = P25.iq_level;
    f->expected += b->bytes / 4;
    f->received += b->bytes; P25.iq_bytes_total += b->bytes;
    lr2021_iq_queue_release(q);
    const int64_t t2 = esp_timer_get_time();
    int count = dsp_process_float_iq(&s_dsp, f->iq, (int)pairs, f->audio, 1024);
    f->dsp_us += esp_timer_get_time() - t2;
    if (count < 0 || dsd_ring_available(&s_ring) + count >= DSD_SAMPLE_RING_SIZE) return false;
    for (int i = 0; i < count; ++i) {
        s_ring.buf[s_ring.write_idx] = f->audio[i];
        s_ring.write_idx = (s_ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
    }
    f->audio_received += count;
    P25.ring_fill = dsd_ring_available(&s_ring);
    P25.lr_iq_good++; P25.lr_iq_sample_rate = 125000;
    ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
    int64_t elapsed = esp_timer_get_time() - f->rate_start;
    if (elapsed >= 1000000) {
        P25.iq_bytes_sec = f->received * 1000000 / elapsed;
        P25.audio_samples_sec = f->audio_received * 1000000 / elapsed;
        P25.lr_fe_permille = (uint32_t)(f->fe_us * 1000 / elapsed);
        P25.lr_dsp_permille = (uint32_t)(f->dsp_us * 1000 / elapsed);
        f->received = f->audio_received = 0; f->fe_us = f->dsp_us = 0;
        f->rate_start = esp_timer_get_time();
    }
    return true;
}
/* A tune request taken from the control or the scanner; true when it moves. */
static bool p25_lr_tune_request(void)
{
    ls_iq_control_request_t request;
    if (ls_iq_control_take(&s_radio_control, &request) && (request.flags & LS_IQ_CONTROL_TUNE)) {
        s_tune_freq_hz = request.center_hz; return true;
    }
    if (SCAN.request_tune && SCAN.peak_freq) {
        SCAN.request_tune = false; s_tune_freq_hz = SCAN.peak_freq; return true;
    }
    return false;
}

/* WIDE: the shared LR session (lr_wf_session.h) captures the 576k route and
   down-converts it on its own task; the sink regroups its output into this
   queue. A grant hops the session in place with lr_wf_retune. Returns when
   P25 leaves, the path changes, the channel moves off the band or the
   session ends; the caller's loop decides what next. */
static void p25_lr_wide_session(p25_lr_wide_sink_t *k, p25_lr_feed_t *f, uint32_t *generation)
{
    const bool pinned = p25_lr_wide_pref() == P25_LR_WIDE_ON;
    if (pinned) lr_wf_set_wide_force(true);
    uint32_t hz = s_tune_freq_hz;
    s_lr_hop_t0 = esp_timer_get_time();
    if (!p25_lr_wide_sink_open(k, ++*generation)) { lr_wf_set_wide_force(false); return; }
    esp_err_t e = lr_wf_start_consumer(hz, p25_lr_wide_sink_consume, k);
    if (e != ESP_OK) {
        p25_lr_wide_sink_close(k);
        lr_wf_set_wide_force(false);
        if (e == ESP_ERR_INVALID_ARG) {
            s_lr_wide_refused_hz = hz;   /* this channel's LO is off the band */
            sys_log(3, "LR wide: %lu Hz has no LO below it; narrow here", (unsigned long)hz);
        } else {
            sys_log(4, "LR wide session refused: %s", esp_err_to_name(e));
            P25.lr_iq_bad++;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        return;
    }
    P25.lr_iq_wide = true;
    sys_log(1, "LR raw IQ: wide 576k route, hops in session");
    for (;;) {
        s_radio_freq_hz = hz;
        ls_iq_control_note_tune_result(&s_radio_control, hz, LS_RADIO_OK, hz);
        p25_lr_generation_begin();
        if (*generation > 1) P25.lr_iq_gaps++;
        if (!p25_lr_frontend_init(f->frontend, 0, false)) break;
        f->expected = 0;
        bool bad = false, hop = false, ended = false;
        while (p25_lr_iq_wanted() && p25_lr_wide_now()) {
            if (p25_lr_tune_request()) {
                hop = s_tune_freq_hz != hz;
                if (hop) break;
                /* Already there: answer, or the control waits on PENDING. */
                ls_iq_control_note_tune_result(&s_radio_control, hz, LS_RADIO_OK, hz);
            }
            if (atomic_load(&k->broken)) { bad = true; break; }
            const lr2021_iq_block *b = lr2021_iq_queue_peek(&k->queue);
            if (!b) {
                lr_wf_stats st; lr_wf_get_stats(&st);
                if (st.done) { bad = ended = true; break; }
                vTaskDelay(1); continue;
            }
            p25_lr_hop_landed(true);
            if (!p25_lr_block(f, &k->queue, b, *generation)) { bad = true; break; }
        }
        p25_lr_record_gap();
        p25_decoder_retuned(s_tune_freq_hz);
        ls_iq_control_set_streaming(&s_radio_control, false, LS_RADIO_ERR_STOPPED);
        if (bad) P25.lr_iq_bad++;
        if (ended || !p25_lr_iq_wanted() || !p25_lr_wide_now()) break;
        p25_lr_wide_sink_close(k);
        if (hop) {
            if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), s_tune_freq_hz)) break;
            const unsigned resets = atomic_load(&k->resets);
            s_lr_hop_t0 = esp_timer_get_time();
            if (lr_wf_retune(s_tune_freq_hz) != ESP_OK) break;
            /* The old channel's stream ends with a reset; what follows it is
               the new one. */
            const int64_t until = esp_timer_get_time() + 1000000;
            lr_wf_stats st = {0};
            while (atomic_load(&k->resets) == resets && esp_timer_get_time() < until) {
                lr_wf_get_stats(&st); if (st.done) break;
                vTaskDelay(1);
            }
            if (st.done || atomic_load(&k->resets) == resets) break;
            hz = s_tune_freq_hz;
        }
        if (!p25_lr_wide_sink_open(k, ++*generation)) break;
    }
    p25_lr_wide_sink_close(k);
    lr_wf_stats st; lr_wf_stop_stats(&st);
    lr_wf_set_wide_force(false);
    P25.lr_iq_wide = false;
    if (st.error != ESP_OK) {
        sys_log(4, "LR wide session ended: %s", esp_err_to_name(st.error));
        P25.read_errors_total++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void p25_lr_iq_run(void)
{
    /* The mesh task pauses itself only after it is told: a first answer of
       no is the handshake, not another owner. Give it the time the other
       holders give before saying the chip is in use elsewhere. */
    bool held = false;
    const int64_t hold_until = esp_timer_get_time() + 500000;
    while (!ls_mesh_radio_wanted() && p25_lr_iq_wanted() &&
           !(held = ls_mesh_radio_hold_bg(true)) && esp_timer_get_time() < hold_until)
        vTaskDelay(pdMS_TO_TICKS(5));
    if (!held && !ls_mesh_radio_wanted() && !p25_lr_iq_wanted()) {
        ls_mesh_radio_hold_bg(false);   /* leaving, not refused */
        vTaskDelay(pdMS_TO_TICKS(20));
        return;
    }
    if (!held) {
        P25.lora_wait = true;
        ls_iq_control_receiver_lost(&s_radio_control, LS_RADIO_ERR_BUSY);
        vTaskDelay(pdMS_TO_TICKS(200));
        return;
    }
    /* A stopped mesh leaves the chip parked in reset; it is still fitted
       (p25_lora_wanted), so it is started here, before its caps are read. */
    if (!ls_lora_present()) {
        const esp_err_t e = ls_lora_start();
        if (e != ESP_OK) {
            sys_log(4, "LR raw IQ: the parked LR2021 did not start: %s", esp_err_to_name(e));
            ls_mesh_radio_hold_bg(false);
            vTaskDelay(pdMS_TO_TICKS(1000));
            return;
        }
    }
    /* PSRAM, as the wide sink's queue below: 10.6 KB the DMA pool could not
       spare while P25 runs. The capture task copies each validated batch in
       from the driver's internal buffer; nothing here is a DMA target. */
    p25_lr_capture_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    float *iq = heap_caps_malloc(640 * 2 * sizeof(*iq), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *audio = heap_caps_malloc(1024 * sizeof(*audio), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Wide only: its producer is the session's owner task, already working
       from PSRAM, so its queue lives there and costs no internal RAM. */
    p25_lr_wide_sink_t *wide = NULL;
    p25_lr_frontend_t frontend;
    ls_lora_cfg_t saved;
    const ls_lora_cfg_t *previous = ls_lora_cfg();
    if (previous) saved = *previous;
    else ls_lora_cfg_default(&saved);
    ls_lora_cfg_t cfg = saved;
    bool configured = false;
    if (!c || !iq || !audio) {
        sys_log(4, "LR raw IQ: insufficient memory; IQ remains selected");
        goto done;
    }
    atomic_init(&c->stop, false); atomic_init(&c->done, true); atomic_init(&c->full, false);
    P25.lora_wait = false;
    P25.lora_rx = P25.lr_iq_active = true;
    p25_p2_enable(false);
    s_dsd_opts.play_unproven = 1;
    s_dsd_opts.erasure_marks = s_dsd_opts.soft_symbols = 0;
    cfg.sf = 7; cfg.bw_hz = 31250; cfg.invert_iq = false;
    uint32_t configured_hz = 0;
    unsigned failures = 0;
    s_lr_wide_refused_hz = 0;
    s_lr_hop_t0 = esp_timer_get_time();
    sys_log(1, "LR raw IQ EXP: queued C4FM stream; real losses reset acquisition");
    while (p25_lr_iq_wanted()) {
        if (p25_lr_tune_request()) s_lr_hop_t0 = esp_timer_get_time();
        if (SCAN.request_scan) {
            SCAN.request_scan = false;
            sys_log(4, "LR raw IQ listens to one channel; band scan unavailable");
        }
        if (!ls_lora_packet_rx_hz_ok(ls_lora_caps(), s_tune_freq_hz)) {
            ls_iq_control_note_tune_result(&s_radio_control, s_tune_freq_hz,
                                           LS_RADIO_ERR_UNSUPPORTED, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (p25_lr_wide_now()) {
            if (!wide) wide = heap_caps_calloc(1, sizeof(*wide), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!wide) {
                sys_log(4, "LR wide: no memory for its queue; narrow");
                s_lr_wide_refused_hz = s_tune_freq_hz;
                continue;
            }
            /* The modem is the session's to set; P25's own setting is stale. */
            configured_hz = 0;
            p25_lr_feed_t f = {.frontend = &frontend, .iq = iq, .audio = audio,
                               .rate_start = esp_timer_get_time()};
            p25_lr_wide_session(wide, &f, &c->generation);
            continue;
        }
        if (configured_hz != s_tune_freq_hz) {
            cfg.freq_hz = s_tune_freq_hz;
            configured = true;
            esp_err_t e = ls_lora_configure(&cfg);
            if (e == ESP_OK) e = ls_lora_receive();
            if (e != ESP_OK) {
                sys_log(4, "LR raw IQ configuration failed: %s", esp_err_to_name(e));
                break;
            }
            configured_hz = s_radio_freq_hz = cfg.freq_hz;
        }
        /* Confirm every session, as the FSK path does: a request for the
           frequency already set must not leave the decoder waiting on PENDING. */
        ls_iq_control_note_tune_result(&s_radio_control, configured_hz, LS_RADIO_OK, configured_hz);
        /* Only a real stop/loss/retune creates a new acquisition generation. */
        if (c->generation) P25.lr_iq_gaps++;
        ++c->generation;
        p25_lr_generation_begin();
        if (!p25_lr_frontend_init(&frontend, 0, false) || !lr2021_iq_queue_init(&c->queue)) break;
        atomic_store(&c->stop, false); atomic_store(&c->done, false); atomic_store(&c->full, false);
        if (xTaskCreatePinnedToCore(p25_lr_capture_task, "lr_iq_rx", 4096, c,
                                  configMAX_PRIORITIES - 1, NULL, 0) != pdPASS) {
            /* Internal RAM is short for a moment (a retune frees and takes
               it again): wait and try again here, still holding the chip,
               rather than leaving LR mode and coming back a second later. */
            atomic_store(&c->done, true);
            P25.lr_iq_bad++;
            if (failures == 0) sys_log(4, "LR raw IQ: capture task allocation failed; retrying");
            vTaskDelay(pdMS_TO_TICKS(++failures >= 5 ? 1000 : 100));
            if (failures >= 5) failures = 0;
            continue;
        }
        lr_spec_begin(configured_hz);
        p25_lr_feed_t f = {.frontend = &frontend, .iq = iq, .audio = audio,
                           .rate_start = esp_timer_get_time()};
        bool consumer_bad = false, retune = false;
        while (p25_lr_iq_wanted() && !atomic_load_explicit(&c->done, memory_order_acquire)) {
            if (p25_lr_tune_request()) { retune = true; s_lr_hop_t0 = esp_timer_get_time(); break; }
            /* WIDE I/Q switched on (or P25 pinned to it): hand over. */
            if (p25_lr_wide_now()) break;
            const lr2021_iq_block *b = lr2021_iq_queue_peek(&c->queue);
            if (!b) { vTaskDelay(1); continue; }
            if (b->generation != c->generation || b->first_pair != f.expected) { consumer_bad = true; break; }
            lr_spec_feed(b->data, b->bytes, b->first_pair, b->generation,
                         (uint32_t)(esp_timer_get_time() / 1000),
                         atomic_load(&c->queue.write_index) - atomic_load(&c->queue.read_index) <= 1);
            p25_lr_hop_landed(false);
            if (!p25_lr_block(&f, &c->queue, b, c->generation)) { consumer_bad = true; break; }
        }
        atomic_store(&c->stop, true);
        lr_spec_end();
        p25_lr_record_gap();
        while (!atomic_load_explicit(&c->done, memory_order_acquire)) vTaskDelay(1);
        lr20xx_stream_result_t *result = &c->result;
        bool bad = consumer_bad || atomic_load(&c->full) || c->error != ESP_OK;
        /* WARN when bad: release builds compile INFO out, and this line names the cause. */
        if (bad) ESP_LOGW("LRIQ", "bad session: %lu ms, %lu KiB, maxfifo=%lu gap=%lu us wake_us=%lu isr_late=%lu resume=%lu wakefifo=%lu wakes=%lu drain_us=%lu drain_spi=%lu max_drain_spi=%lu flags=%02x queuefull=%u consumer=%u err=%d",
                (unsigned long)(result->elapsed_us / 1000), (unsigned long)(result->validated_bytes / 1024),
                (unsigned long)result->max_fifo_level, (unsigned long)result->max_service_gap_us,
                (unsigned long)result->max_wake_latency_us,
                (unsigned long)result->max_wake_isr_lateness_us,
                (unsigned long)result->max_wake_resume_us,
                (unsigned long)result->max_wake_fifo_level,
                (unsigned long)result->wakes, (unsigned long)result->max_drain_us,
                (unsigned long)result->worst_drain_spi_bytes, (unsigned long)result->max_drain_spi_bytes,
                result->fifo_flags, atomic_load(&c->full), consumer_bad, (int)c->error);
        else ESP_LOGI("LRIQ", "session: %lu ms, %lu KiB, maxfifo=%lu gap=%lu us wake_us=%lu isr_late=%lu resume=%lu wakefifo=%lu wakes=%lu drain_us=%lu drain_spi=%lu max_drain_spi=%lu flags=%02x queuefull=%u consumer=%u",
                (unsigned long)(result->elapsed_us / 1000), (unsigned long)(result->validated_bytes / 1024),
                (unsigned long)result->max_fifo_level, (unsigned long)result->max_service_gap_us,
                (unsigned long)result->max_wake_latency_us,
                (unsigned long)result->max_wake_isr_lateness_us,
                (unsigned long)result->max_wake_resume_us,
                (unsigned long)result->max_wake_fifo_level,
                (unsigned long)result->wakes, (unsigned long)result->max_drain_us,
                (unsigned long)result->worst_drain_spi_bytes, (unsigned long)result->max_drain_spi_bytes,
                result->fifo_flags, atomic_load(&c->full), consumer_bad);
        if (bad) {
            P25.lr_iq_bad++;
            if ((result->fifo_capacity && result->max_fifo_level >= result->fifo_capacity) ||
                (result->fifo_flags & 0x30)) P25.lr_iq_fifo_overruns++;
            else P25.read_errors_total++;
        }
        p25_decoder_retuned(s_tune_freq_hz);
        ls_iq_control_set_streaming(&s_radio_control, false, LS_RADIO_ERR_STOPPED);
        if (result->recovery_error != ESP_OK) break;
        /* NO_MEM is the driver's capture buffer: retried below, in the run. */
        if (c->error == ESP_ERR_NOT_SUPPORTED || c->error == ESP_ERR_INVALID_STATE) break;
        if (!p25_lr_iq_wanted()) break;
        if (bad && !retune) {
            vTaskDelay(pdMS_TO_TICKS(++failures >= 5 ? 1000 : 100));
            if (failures >= 5) failures = 0;
        } else failures = 0;
    }
    s_dsd_opts.play_unproven = 0;
    P25.lora_rx = P25.lr_iq_active = false;
    P25.iq_bytes_sec = P25.audio_samples_sec = 0;
    P25.iq_level = p25_rx_power = 0;
    p25_decoder_retuned(s_tune_freq_hz);
    ls_iq_control_receiver_lost(&s_radio_control, LS_RADIO_ERR_STOPPED);
    dsp_init(&s_dsp);
    dsp_set_mode(&s_dsp, s_demod_control.active);
    dsp_set_gain(&s_dsp, p25_demod_output_gain(s_demod_control.active, P25.demod_invert));
    if (configured && ls_lora_configure(&saved) != ESP_OK)
        sys_log(4, "LR raw IQ: restoring LoRa configuration failed");
done:
    heap_caps_free(wide);
    heap_caps_free(audio); heap_caps_free(iq); heap_caps_free(c);
    ls_mesh_radio_hold_bg(false);
    if (s_app_active && atomic_load(&s_lr_iq_requested)) vTaskDelay(pdMS_TO_TICKS(1000));
}
