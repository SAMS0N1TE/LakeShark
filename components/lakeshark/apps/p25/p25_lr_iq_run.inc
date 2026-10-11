/* Capture yields between batches on core 0; RX owner runs IQ DSP on core 1. */
#include "lr2021_iq_queue.h"
#include "p25_lr_frontend.h"
#include "p25_lr_iq_record.inc"
typedef struct {
    lr2021_iq_queue queue;
    atomic_bool stop, done, full;
    lr20xx_stream_result_t result;
    esp_err_t error;
    uint32_t generation;
} p25_lr_capture_t;

static bool p25_lr_iq_collect(void *context, const uint8_t *data, size_t bytes,
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
static void p25_lr_iq_run(void)
{
    if (ls_mesh_radio_wanted() || !ls_mesh_radio_hold_bg(true)) {
        P25.lora_wait = true;
        ls_iq_control_receiver_lost(&s_radio_control, LS_RADIO_ERR_BUSY);
        vTaskDelay(pdMS_TO_TICKS(200));
        return;
    }
    p25_lr_capture_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    float *iq = heap_caps_malloc(640 * 2 * sizeof(*iq), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *audio = heap_caps_malloc(1024 * sizeof(*audio), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    sys_log(1, "LR raw IQ EXP: queued C4FM stream; real losses reset acquisition");
    while (p25_lr_iq_wanted()) {
        ls_iq_control_request_t request = {0};
        if (ls_iq_control_take(&s_radio_control, &request) &&
            (request.flags & LS_IQ_CONTROL_TUNE)) s_tune_freq_hz = request.center_hz;
        if (SCAN.request_tune && SCAN.peak_freq) {
            SCAN.request_tune = false; s_tune_freq_hz = SCAN.peak_freq;
        }
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
        p25_decoder_retuned(s_tune_freq_hz);
        if (c->generation) P25.lr_iq_gaps++;
        ++c->generation;
        dsp_init(&s_dsp); dsp_set_mode(&s_dsp, DEMOD_C4FM);
        dsp_set_gain(&s_dsp, p25_demod_output_gain(DEMOD_C4FM, P25.demod_invert));
        P25.demod_active = DEMOD_C4FM; P25.demod_auto = false;
        if (!p25_lr_frontend_init(&frontend, 0, false) || !lr2021_iq_queue_init(&c->queue)) break;
        atomic_store(&c->stop, false); atomic_store(&c->done, false); atomic_store(&c->full, false);
        if (xTaskCreatePinnedToCore(p25_lr_capture_task, "lr_iq_rx", 4096, c,
                                  configMAX_PRIORITIES - 2, NULL, 0) != pdPASS) {
            atomic_store(&c->done, true);
            sys_log(4, "LR raw IQ: capture task allocation failed");
            break;
        }
        uint64_t expected = 0, received = 0, audio_received = 0;
        int64_t rate_start = esp_timer_get_time();
        bool consumer_bad = false, retune = false;
        while (p25_lr_iq_wanted() && !atomic_load_explicit(&c->done, memory_order_acquire)) {
            if (ls_iq_control_take(&s_radio_control, &request) &&
                (request.flags & LS_IQ_CONTROL_TUNE)) {
                s_tune_freq_hz = request.center_hz; retune = true; break;
            }
            if (SCAN.request_tune && SCAN.peak_freq) {
                SCAN.request_tune = false; s_tune_freq_hz = SCAN.peak_freq; retune = true; break;
            }
            const lr2021_iq_block *b = lr2021_iq_queue_peek(&c->queue);
            if (!b) { vTaskDelay(1); continue; }
            if (b->generation != c->generation || b->first_pair != expected) { consumer_bad = true; break; }
            p25_lr_record_feed(b);
            size_t pairs = p25_lr_frontend_process(&frontend, b->data, b->bytes, iq, 640);
            if (pairs == SIZE_MAX) { consumer_bad = true; break; }
            float peak = 0;
            for (size_t k = 0; k < 2 * pairs; k += 4) {
                float v = fmaxf(fabsf(iq[k]), fabsf(iq[k + 1]));
                if (v > peak) peak = v;
            }
            P25.iq_level = 0.6f * P25.iq_level + 0.4f * peak;
            p25_rx_power = P25.iq_level;
            expected += b->bytes / 4;
            received += b->bytes; P25.iq_bytes_total += b->bytes;
            lr2021_iq_queue_release(&c->queue);
            int count = dsp_process_float_iq(&s_dsp, iq, (int)pairs, audio, 1024);
            if (count < 0 || dsd_ring_available(&s_ring) + count >= DSD_SAMPLE_RING_SIZE) {
                consumer_bad = true; break;
            }
            for (int i = 0; i < count; ++i) {
                s_ring.buf[s_ring.write_idx] = audio[i];
                s_ring.write_idx = (s_ring.write_idx + 1) % DSD_SAMPLE_RING_SIZE;
            }
            audio_received += count;
            P25.ring_fill = dsd_ring_available(&s_ring);
            P25.lr_iq_good++; P25.lr_iq_sample_rate = 125000;
            ls_iq_control_set_streaming(&s_radio_control, true, LS_RADIO_OK);
            int64_t elapsed = esp_timer_get_time() - rate_start;
            if (elapsed >= 1000000) {
                P25.iq_bytes_sec = received * 1000000 / elapsed;
                P25.audio_samples_sec = audio_received * 1000000 / elapsed;
                received = audio_received = 0; rate_start = esp_timer_get_time();
            }
        }
        atomic_store(&c->stop, true);
        p25_lr_record_gap();
        while (!atomic_load_explicit(&c->done, memory_order_acquire)) vTaskDelay(1);
        lr20xx_stream_result_t *result = &c->result;
        bool bad = consumer_bad || atomic_load(&c->full) || c->error != ESP_OK;
        ESP_LOGI("LRIQ", "session: %lu ms, %lu KiB, maxfifo=%lu gap=%lu us flags=%02x queuefull=%u consumer=%u",
                (unsigned long)(result->elapsed_us / 1000), (unsigned long)(result->validated_bytes / 1024),
                (unsigned long)result->max_fifo_level, (unsigned long)result->max_service_gap_us,
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
        if (c->error == ESP_ERR_NOT_SUPPORTED || c->error == ESP_ERR_INVALID_STATE ||
            c->error == ESP_ERR_NO_MEM) break;
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
    heap_caps_free(audio); heap_caps_free(iq); heap_caps_free(c);
    ls_mesh_radio_hold_bg(false);
    if (s_app_active && atomic_load(&s_lr_iq_requested)) vTaskDelay(pdMS_TO_TICKS(1000));
}
