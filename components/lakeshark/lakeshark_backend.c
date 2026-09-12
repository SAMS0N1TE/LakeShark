#include "lakeshark_backend.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "app_registry.h"
#include "settings.h"
/**/
#include "usb_autoreboot_pref.h"
#include "scan_channels.h"
#include "scan_engine.h"
#include "perf.h"
#include "event_bus.h"
#include "usb_host.h"

#include "p25_state.h"
#include "p25_health.h"
#include "p25_controls.h"
#include "fm_state.h"
#include "rec_state.h"
#include "adsb_state.h"
#include "adsb_app.h"
#include "dsp_pipeline.h"
#include "p25_demod_mode.h"
#include "p25_demod_control.h"
#include "p25_program.h"
#include "radio_endpoint.h"
#include "radio_health.h"
#include "receiver_diagnostics.h"
#include "fm_mode_label.h"

#include "audio_out.h"
#include "audio_events.h"
#include "sam_tts.h"
#include "tone.h"
#include "event_stream.h"

/**/
#include "bsp/esp-bsp.h"

static const char *TAG = "lakeshark";

extern int adsb_app_register(void);
extern int p25_app_register(void);
extern int fm_app_register(void);
/**/
extern int rec_app_register(void);

extern int          autoscan_bch_ok_flag;
extern int          dsd_bch_fail_counter;

static int s_adsb_idx = -1;
static int s_p25_idx  = -1;
static int s_fm_idx   = -1;
static int s_rec_idx  = -1;
static bool s_started = false;

void lakeshark_backend_start(void)
{
    if (s_started) return;
    s_started = true;

    esp_log_level_set("USBH",     ESP_LOG_INFO);
    esp_log_level_set("HUB",      ESP_LOG_INFO);
    esp_log_level_set("ENUM",     ESP_LOG_INFO);
    esp_log_level_set("USB_HOST", ESP_LOG_INFO);
    esp_log_level_set("CLASS",    ESP_LOG_INFO);

    event_bus_init();
    settings_init();
    /**/
    usb_autoreboot_pref_init(settings_set_usb_autoreboot,
                             settings_get_usb_autoreboot());
    scan_channels_init();

    event_stream_init();

    s_adsb_idx = adsb_app_register();
    s_p25_idx  = p25_app_register();
    s_fm_idx   = fm_app_register();
    s_rec_idx  = rec_app_register();

    app_switch_worker_start();
    scan_engine_init();

    esp_log_level_set("P25DIAG", ESP_LOG_ERROR);
    esp_log_level_set("P25DBG",  ESP_LOG_ERROR);

    {
        int p  = settings_voice_preset_get();
        int lp = settings_voice_lowpass_get();
        int sh = settings_voice_lowshelf_get();
        if (p >= 0 && p < SAM_PRESET_COUNT) sam_tts_set_preset((sam_tts_voice_preset_t)p);
        sam_tts_set_lowpass(lp);
        sam_tts_set_lowshelf(sh);
    }

    ESP_LOGW(TAG, "heap before USB host: internal=%u DMA=%u largest-DMA=%u (bytes)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false));

    BaseType_t ok = xTaskCreatePinnedToCore(class_driver_task, "class",
                                            5 * 1024, NULL, 14, NULL, 1);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "failed to start class_driver_task");
    }

    if (audio_out_init() == ESP_OK) {
        audio_events_init();

    }

    perf_init();

    event_bus_publish_simple(EVT_BOOT, "lakeshark");

    ESP_LOGW(TAG, "heap after USB+audio: internal=%u DMA=%u largest-DMA=%u PSRAM=%u largest-PSRAM=%u (bytes)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG, "backend started (adsb=%d p25=%d)", s_adsb_idx, s_p25_idx);
}

void lakeshark_boot_sound(void)
{
    int mode = settings_get_boot_sound();
    /* The same fallback as snd_boot_start: no engine, no greeting. */
    if (mode == 2 && !sam_tts_available()) mode = 1;
    if (mode == 1) {
        snd_boot();
        static const int16_t sil[1600] = {0};
        audio_write_mono(sil, 1600);
    } else if (mode == 2) {
        sam_tts_speak("WELCOME.");
    } else {
        return;
    }

    for (int i = 0; i < 250 && audio_out_ring_avail() > 320; i++)
        vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(90));
}

void lakeshark_select_adsb(void) { if (s_adsb_idx >= 0) app_switch_to(s_adsb_idx); }
void lakeshark_select_p25(void)  { if (s_p25_idx  >= 0) app_switch_to(s_p25_idx);  }
void lakeshark_select_fm(void)   { if (s_fm_idx   >= 0) app_switch_to(s_fm_idx);   }
/**/
void lakeshark_select_rec(void)  { if (s_rec_idx  >= 0) app_switch_to(s_rec_idx);  }

void lakeshark_acars_start(void)
{
    if (s_fm_idx < 0) return;
    app_switch_to(s_fm_idx);
    lakeshark_fm_set_mode(FM_MODE_ACARS);
    /* The FM rx task reads the saved per-mode freq for FM_MODE_ACARS when
       it acts on the mode request, defaulting to FM_FREQ_ACARS the first
       time.  Nothing to force here. */
}
void     lakeshark_acars_stop    (void)         { lakeshark_radio_park(); }
uint32_t lakeshark_acars_get_freq(void)         { return lakeshark_fm_get_freq(); }
void     lakeshark_acars_set_freq(uint32_t hz)  { lakeshark_fm_set_freq(hz); }

void lakeshark_radio_park(void)    { app_park();   }
void lakeshark_radio_unpark(void)  { app_unpark(); }
bool lakeshark_radio_running(void) { return !app_parked(); }

bool lakeshark_radio_ready(const ls_radio_requirements_t *requirements)
{
    return ls_radio_endpoint_available(requirements);
}

bool lakeshark_iq_receiver_ready(void)
{
    const ls_radio_requirements_t requirements = {
        .required_caps = LS_RADIO_RX_IQ_U8,
        .iq_format = LS_RADIO_IQ_FORMAT_U8_INTERLEAVED,
    };
    return lakeshark_radio_ready(&requirements);
}

bool lakeshark_radio_endpoint_ready(const char *endpoint_id)
{
    ls_radio_endpoint_info_t info;
    return endpoint_id &&
           ls_radio_endpoint_get(endpoint_id, &info) == LS_RADIO_OK &&
           info.present;
}

const char *lakeshark_recovery_take_app(void) { return app_recovery_take(); }

static void diag_copy(char *out, size_t len, const char *value)
{
    if (!out || len == 0) return;
    if (!value) value = "";
    strlcpy(out, value, len);
}

typedef struct {
    ls_receiver_diag_t diag;
    ls_radio_endpoint_info_t endpoint;
    ls_radio_endpoint_info_t scan_endpoint;
    radio_health_snapshot_t health;
    ls_iq_control_status_t radio;
    union {
        lakeshark_adsb_tel_t adsb;
        rec_hub_status_t rec;
    } app;
} receiver_diag_workspace_t;

static bool diag_selected_endpoint(ls_radio_endpoint_info_t *selected,
                                   ls_radio_endpoint_info_t *scan)
{
    size_t iq_count = 0;
    size_t count = ls_radio_endpoint_count();
    for (size_t i = 0; i < count; ++i) {
        if (ls_radio_endpoint_info(i, scan) != LS_RADIO_OK ||
            (scan->capabilities & LS_RADIO_RX_IQ_U8) == 0)
            continue;
        *selected = *scan;
        if (selected->leased) return true;
        ++iq_count;
    }
    /* A parked/disconnected receiver has no lease.  One registered IQ slot
       is still an unambiguous endpoint; with two, report unknown instead of
       guessing which radio the app would acquire next. */
    return iq_count == 1;
}

static void diag_from_control(ls_receiver_diag_t *diag,
                              const ls_iq_control_status_t *radio)
{
    diag->requested_frequency_known = radio->requested_center_hz != 0;
    diag->requested_frequency_hz = radio->requested_center_hz;
    diag->effective_frequency_known = radio->effective_center_known;
    diag->effective_frequency_hz = radio->effective_center_hz;
    diag->requested_gain_known = true;
    diag->requested_gain_tenths_db = radio->requested_gain_tenths_db;
    diag->effective_gain_known = radio->effective_gain_known;
    diag->effective_gain_tenths_db = radio->effective_gain_tenths_db;
    diag->receiver_error_known = true;
    diag_copy(diag->receiver_error, sizeof(diag->receiver_error),
              ls_radio_err_name(radio->receiver_error));
    if (radio->receiver_streaming)
        diag->rx_state = LS_RECEIVER_RX_ACTIVE;
    else if (radio->receiver_error == LS_RADIO_ERR_DISCONNECTED ||
             radio->receiver_error == LS_RADIO_ERR_UNAVAILABLE)
        diag->rx_state = LS_RECEIVER_RX_DISCONNECTED;
    else
        diag->rx_state = LS_RECEIVER_RX_IDLE;
}

int lakeshark_receiver_status(char *out, size_t len)
{
    if (!out || len == 0) return 0;

    /* the first receiver snapshot compiled to a 1536-byte frame:
     * a 320-byte diagnostic plus several mutually exclusive 392-byte endpoint
     * copies were kept live above newlib's 1328-byte snprintf path.  The LCD
     * console's 4096-byte stack crossed its guard on every STAT query.  This
     * per-call PSRAM workspace keeps concurrent UART, BLE, and console queries
     * independent without consuming the 19 bytes of measured DMA headroom. */
    receiver_diag_workspace_t *work = heap_caps_calloc(
        1, sizeof(*work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!work) {
        strlcpy(out, "app=? park=? rx=? err=nomem", len);
        return (int)strnlen(out, len);
    }

    ls_receiver_diag_t *diag = &work->diag;
    ls_radio_endpoint_info_t *endpoint = &work->endpoint;
    ls_iq_control_status_t *radio = &work->radio;
    const app_t *app = app_current();
    if (app && app->name) {
        diag->app_known = true;
        diag_copy(diag->app, sizeof(diag->app), app->name);
    }
    diag->parked_known = true;
    diag->parked = app_parked();

    if (diag_selected_endpoint(endpoint, &work->scan_endpoint)) {
        diag->endpoint_known = true;
        diag_copy(diag->endpoint_id, sizeof(diag->endpoint_id),
                  endpoint->endpoint_id);
        diag_copy(diag->owner, sizeof(diag->owner), endpoint->owner);
        diag->endpoint_present = endpoint->present;
        diag->endpoint_streaming = endpoint->streaming;
        diag->iq_total_known = true;
        diag->iq_bytes_total = endpoint->bytes_read;
        if (radio_health_get_for_endpoint(endpoint, &work->health)) {
            diag->health_known = true;
            diag_copy(diag->health, sizeof(diag->health),
                      radio_health_state_name(work->health.state));
            diag->health_rate_known = true;
            diag->health_bytes_per_second = work->health.bytes_per_second;
        }
    }

    if (app_current_index() == s_p25_idx) {
        p25_get_receiver_status(radio);
        diag_from_control(diag, radio);
        diag_copy(diag->demod, sizeof(diag->demod), lakeshark_p25_mode_name());
        diag_copy(diag->decoder, sizeof(diag->decoder),
                  P25.dsd_voice_count &&
                          esp_timer_get_time() < P25.voice_active_until_us
                      ? "voice" : (P25.dsd_has_sync ? "sync" : "hunt"));
        diag->iq_rate_known = true;
        diag->iq_bytes_per_second = P25.iq_bytes_sec;
        diag->frame_count_known = true;
        diag->frame_count = (uint32_t)P25.dsd_sync_count;
        diag->valid_count_known = true;
        diag->valid_count = (uint32_t)P25.dsd_bch_ok_count;
        diag->failed_count_known = true;
        diag->failed_count = (uint32_t)P25.dsd_bch_fail_count;
        diag->control_count_known = true;
        diag->control_count = P25.p25_tsbk_ok_count;
        diag->voice_count_known = true;
        diag->voice_count = (uint32_t)P25.dsd_voice_count;
        diag->audio_drop_count_known = true;
        diag->audio_drop_count = P25.audio_drops;
    } else if (app_current_index() == s_fm_idx) {
        fm_get_receiver_status(radio);
        diag_from_control(diag, radio);
        diag_copy(diag->demod, sizeof(diag->demod),
                  fm_mode_command_name(FM.mode));
        bool sync = FM.mode == FM_MODE_POCSAG ? FM.pocsag_sync
                  : FM.mode == FM_MODE_FLEX   ? FM.flex_sync
                  : FM.squelch_open;
        diag_copy(diag->decoder, sizeof(diag->decoder), sync ? "sync" : "hunt");
        diag->iq_rate_known = true;
        diag->iq_bytes_per_second = FM.iq_bytes_sec;
        if (FM.mode == FM_MODE_POCSAG) {
            diag->frame_count_known = true;
            diag->frame_count = FM.pocsag_frames;
            diag->valid_count_known = true;
            diag->valid_count = FM.pocsag_frames;
            diag->failed_count_known = true;
            diag->failed_count = FM.pocsag_cw_errs;
        } else if (FM.mode == FM_MODE_FLEX) {
            diag->frame_count_known = true;
            diag->frame_count = FM.flex_frames;
            diag->valid_count_known = true;
            diag->valid_count = FM.flex_frames;
            diag->failed_count_known = true;
            diag->failed_count = FM.flex_cw_errs;
        }
    } else if (app_current_index() == s_adsb_idx) {
        lakeshark_adsb_tel_t *adsb = &work->app.adsb;
        lakeshark_adsb_telemetry(adsb);
        diag->requested_frequency_known = true;
        diag->requested_frequency_hz = adsb->freq_hz;
        diag->requested_gain_known = true;
        diag->requested_gain_tenths_db = adsb->gain_tenths;
        diag->iq_rate_known = true;
        diag->iq_bytes_per_second = adsb->iq_bytes_sec;
        diag->frame_count_known = true;
        diag->frame_count = (uint32_t)adsb->msgs_total;
        diag->valid_count_known = true;
        diag->valid_count = (uint32_t)adsb->crc_good;
        diag->failed_count_known = true;
        diag->failed_count = (uint32_t)adsb->crc_err;
        diag_copy(diag->decoder, sizeof(diag->decoder),
                  adsb->last_msg_ms >= 0 ? "frames" : "hunt");
        if (diag->endpoint_known) {
            if (endpoint->configured) {
                diag->effective_frequency_known = endpoint->actual_iq.center_hz != 0;
                diag->effective_frequency_hz = endpoint->actual_iq.center_hz;
                diag->effective_gain_known = true;
                diag->effective_gain_tenths_db = endpoint->actual_iq.gain_tenths_db;
            }
            diag->receiver_error_known = true;
            diag_copy(diag->receiver_error, sizeof(diag->receiver_error),
                      ls_radio_err_name(endpoint->last_error));
            diag->rx_state = endpoint->streaming ? LS_RECEIVER_RX_ACTIVE
                          : !endpoint->present ? LS_RECEIVER_RX_DISCONNECTED
                          : LS_RECEIVER_RX_IDLE;
        }
    } else if (app_current_index() == s_rec_idx) {
        rec_hub_status_t *rec = &work->app.rec;
        rec_get_hub_status(rec);
        rec_get_receiver_status(radio);
        diag_from_control(diag, radio);
        diag_copy(diag->demod, sizeof(diag->demod), "raw");
        static const char *const phase[] = {
            "idle", "armed", "capture", "done"
        };
        unsigned p = (unsigned)rec->phase;
        diag_copy(diag->decoder, sizeof(diag->decoder),
                  p < sizeof(phase) / sizeof(phase[0]) ? phase[p] : "?");
        diag->iq_rate_known = true;
        diag->iq_bytes_per_second = rec->bytes_sec;
        diag->frame_count_known = true;
        diag->frame_count = rec->captures;
        diag->valid_count_known = true;
        diag->valid_count = rec->captures;
    }

    if (diag->parked) diag->rx_state = LS_RECEIVER_RX_PARKED;
    diag->memory_known = true;
    diag->internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    diag->internal_largest =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    diag->dma_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    diag->dma_largest =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    int result = ls_receiver_diag_format(diag, out, len);
    heap_caps_free(work);
    return result;
}

void lakeshark_radio_recover(const char *endpoint_id)
{
    app_request_recover(endpoint_id);
}
void lakeshark_set_usb_autoreboot(bool en)      { app_set_usb_autoreboot(en); }
bool lakeshark_usb_autoreboot(void)             { return app_usb_autoreboot(); }

static void adsb_apply_gain(int g);

void lakeshark_radio_set_gain(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    int idx = app_current_index();
    if (idx == s_p25_idx) {
        p25_request_gain(tenths);
        const app_t *a = app_current();
        if (a) settings_set_gain(a, tenths);
    } else if (idx == s_fm_idx) {
        lakeshark_fm_set_gain(tenths);
    } else if (idx == s_adsb_idx) {
        adsb_apply_gain(tenths);
    }
}

int lakeshark_radio_get_gain_tenths(void)
{
    int idx = app_current_index();
    if (idx == s_adsb_idx) return lakeshark_adsb_gain_tenths();
    if (idx == s_p25_idx)  return lakeshark_p25_gain_tenths();
    if (idx == s_fm_idx)   return lakeshark_fm_gain_tenths();
    return 0;
}

void lakeshark_radio_set_gain_live(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    int idx = app_current_index();
    if (idx == s_p25_idx)      p25_request_gain(tenths);
    else if (idx == s_fm_idx)  lakeshark_fm_set_gain_live(tenths);
}

void lakeshark_p25_tune(int delta_hz)
{
    const app_t *a = app_current();
    uint32_t f = a ? settings_get_freq(a) : s_tune_freq_hz;
    int64_t next = (int64_t)f + (int64_t)delta_hz;
    if (next < (int64_t)P25_CONTROL_TUNER_MIN_HZ)
        next = P25_CONTROL_TUNER_MIN_HZ;
    if (next > (int64_t)P25_CONTROL_TUNER_MAX_HZ)
        next = P25_CONTROL_TUNER_MAX_HZ;
    f = (uint32_t)next;

    scan_engine_stop();
    /* a dial move is an ownership transfer.  End a profile survey
     * and restore its prior valid control before the manual request replaces
     * that tune in the single-slot radio latch. */
    (void)p25_program_survey_cancel_now(P25_SURVEY_CANCEL_MANUAL_TUNE);
    s_tune_freq_hz = f;
    if (a) settings_set_freq(a, f);
    p25_request_tune(f, false);
}

void lakeshark_p25_set_freq(uint32_t hz)
{
    if (hz < P25_CONTROL_TUNER_MIN_HZ || hz > P25_CONTROL_TUNER_MAX_HZ) return;
    scan_engine_stop();
    (void)p25_program_survey_cancel_now(P25_SURVEY_CANCEL_MANUAL_TUNE);
    const app_t *a = app_current();
    s_tune_freq_hz = hz;
    if (a) settings_set_freq(a, hz);
    p25_request_tune(hz, false);
}

uint32_t lakeshark_p25_get_freq(void) { return s_tune_freq_hz; }

const char *lakeshark_p25_cycle_mode(void)
{
    int preference = p25_demod_get_preference();
    int next = preference >= (int)DEMOD_FSK4_TRACKING
                   ? P25_DEMOD_AUTO : preference + 1;
    p25_demod_set_preference(next);
    return p25_demod_get_name();
}

int lakeshark_p25_mode_index(void) { return (int)p25_demod_get_active(); }

void lakeshark_p25_set_mode(int idx)
{
    if (idx == P25_DEMOD_AUTO) p25_demod_set_preference(idx);
    else p25_demod_set_preference((int)p25_demod_mode_clamp(
                                      idx, p25_demod_get_active()));
}

const char *lakeshark_p25_mode_name(void)
{
    return p25_demod_get_name();
}

void lakeshark_p25_toggle_polarity(void)
{
    P25.demod_invert = !P25.demod_invert;
    P25.demod_gain = p25_demod_output_gain(p25_demod_get_active(), P25.demod_invert);
    dsp_set_gain(&s_dsp, P25.demod_gain);
}

bool lakeshark_p25_polarity_inverted(void) { return P25.demod_invert; }

void lakeshark_p25_reset_stats(void)
{
    /* reset every counter shown by the coherent health snapshot while
     * retaining current identity, acquisition, RF, heap and buffer state. */
    p25_health_reset_counters();
    P25.dsd_sync_count     = 0;
    P25.dsd_voice_count    = 0;
    P25.dsd_bch_ok_count   = 0;
    P25.dsd_bch_fail_count = 0;
    autoscan_bch_ok_flag   = 0;
    dsd_bch_fail_counter   = 0;

    P25.dsd_nac     = 0;
    P25.dsd_tg      = 0;
    P25.dsd_src     = 0;
    P25.nac_seen_us = 0;
    P25.tg_seen_us  = 0;
    P25.src_seen_us = 0;
}

void lakeshark_p25_gain_step(void)
{
    static const int gains[] = { 0, 90, 200, 280, 340, 370, 400, 437, 463, 496 };
    const int n = sizeof(gains) / sizeof(gains[0]);
    int cur = P25.rtl_gain_tenths;
    int next_idx = 0;
    for (int i = 0; i < n; i++) if (gains[i] == cur) { next_idx = (i + 1) % n; break; }
    p25_request_gain(gains[next_idx]);
    const app_t *a = app_current();
    if (a) settings_set_gain(a, gains[next_idx]);
}

extern volatile bool p25_agc_on;

void lakeshark_p25_agc(void)
{
    p25_agc_on = false;
    p25_request_gain(280);
    const app_t *a = app_current();
    if (a) settings_set_gain(a, 280);
}

bool lakeshark_p25_agc_enabled(void) { return p25_agc_on; }

int lakeshark_p25_gain_tenths(void) { return P25.rtl_gain_tenths; }

void lakeshark_p25_beep_toggle(void)
{
    P25.sync_beep_enabled = !P25.sync_beep_enabled;
}

bool lakeshark_p25_beep_enabled(void) { return P25.sync_beep_enabled; }

extern volatile int p25_voice_gate;
void lakeshark_p25_set_voice_gate(int v)
{
    if (v < 6)  v = 6;
    if (v > 99) v = 99;
    p25_voice_gate = v;
}
int lakeshark_p25_voice_gate(void) { return p25_voice_gate; }

void lakeshark_p25_telemetry(lakeshark_p25_tel_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    int64_t now = esp_timer_get_time();

    out->freq_hz           = s_tune_freq_hz;
    out->demod_mode        = (int)s_dsp.mode;
    out->gain_tenths       = P25.rtl_gain_tenths;
    out->agc_on            = p25_agc_on ? 1 : 0;
    out->nac               = P25.dsd_nac;
    out->tg                = P25.dsd_tg;
    out->src               = P25.dsd_src;
    out->has_sync          = (P25.dsd_has_sync || now < P25.sync_active_until_us) ? 1 : 0;
    out->voice_active      = (now < P25.voice_active_until_us) ? 1 : 0;
    out->sync_count        = P25.dsd_sync_count;
    out->voice_count       = P25.dsd_voice_count;
    out->bch_ok            = P25.dsd_bch_ok_count;
    out->bch_fail          = P25.dsd_bch_fail_count;
    out->iq_level          = (int)(P25.iq_level * 1000.0f);
    out->polarity_inverted = P25.demod_invert ? 1 : 0;
    out->beep              = P25.sync_beep_enabled ? 1 : 0;
    out->voice_gate        = p25_voice_gate;
    out->rtl_ready         = lakeshark_iq_receiver_ready() ? 1 : 0;
    out->ring_fill         = P25.ring_fill;
    out->ring_size         = P25.ring_size;
    out->read_errors       = P25.read_errors;
    out->iq_bytes_sec      = P25.iq_bytes_sec;
    out->audio_drops       = P25.audio_drops;
    out->decode_us         = (int)(P25.dsd_decode_ms * 1000.0f);

    out->nac_age_ms = P25.nac_seen_us ? (int)((now - P25.nac_seen_us) / 1000) : -1;
    out->tg_age_ms  = P25.tg_seen_us  ? (int)((now - P25.tg_seen_us)  / 1000) : -1;
    out->src_age_ms = P25.src_seen_us ? (int)((now - P25.src_seen_us) / 1000) : -1;

    strlcpy(out->ftype, P25.dsd_ftype, sizeof(out->ftype));
    strlcpy(out->err,   P25.dsd_err_str, sizeof(out->err));
}

void lakeshark_adsb_telemetry(lakeshark_adsb_tel_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    int64_t now = esp_timer_get_time();

    out->freq_hz      = 1090000000UL;
    out->gain_tenths  = lakeshark_adsb_gain_tenths();
    out->rtl_ready    = lakeshark_iq_receiver_ready() ? 1 : 0;
    out->iq_bytes_sec = perf_get_bytes_per_sec();

    out->tracked      = adsb_state_active_count();
    out->msgs_total   = perf_get_msgs_total();
    out->msgs_sec     = perf_get_msgs_per_sec();
    out->crc_good     = perf_get_crc_good();
    out->crc_err      = perf_get_crc_err();
    out->bursts_sec   = perf_get_bursts_per_sec();
    out->mag_avg      = perf_get_mag_avg();
    out->mag_peak     = perf_get_mag_peak();

    int64_t last_good = perf_get_last_good_us();
    out->last_msg_ms  = last_good ? (int)((now - last_good) / 1000) : -1;

    for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
        const adsb_aircraft_t *a = adsb_state_get(slot);
        if (a && a->active) out->n_aircraft++;
    }
    if (out->n_aircraft > LAKESHARK_ADSB_MAX) out->n_aircraft = LAKESHARK_ADSB_MAX;
}

bool lakeshark_adsb_aircraft_at(int index, lakeshark_adsb_ac_t *out)
{
    if (!out || index < 0) return false;

    int64_t now = esp_timer_get_time();
    int dense = 0;

    for (int slot = 0; slot < ADSB_MAX_TRACKED; slot++) {
        const adsb_aircraft_t *a = adsb_state_get(slot);
        if (!a || !a->active) continue;
        if (dense++ != index) continue;

        memset(out, 0, sizeof(*out));
        out->icao      = a->icao;
        strlcpy(out->callsign, a->callsign, sizeof(out->callsign));
        out->altitude  = a->altitude;
        out->velocity  = a->velocity;
        out->heading   = a->heading;
        out->vert_rate = a->vert_rate;
        out->lat       = a->lat;
        out->lon       = a->lon;
        out->pos_valid = a->pos_valid ? 1 : 0;
        out->msg_count = a->msg_count;
        out->age_ms    = a->last_seen_us ? (int)((now - a->last_seen_us) / 1000) : -1;
        return true;
    }
    return false;
}

void lakeshark_fm_telemetry(lakeshark_fm_tel_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->submode        = (int)FM.mode;
    out->freq_hz        = FM.freq_hz;
    out->gain_tenths    = FM.gain_tenths;
    ls_iq_control_status_t radio;
    fm_get_receiver_status(&radio);
    out->effective_freq_hz = radio.effective_center_hz;
    out->effective_gain_tenths = radio.effective_gain_tenths_db;
    out->effective_freq_known = radio.effective_center_known ? 1 : 0;
    out->effective_gain_known = radio.effective_gain_known ? 1 : 0;
    out->tune_state = (int)radio.tune_state;
    out->gain_state = (int)radio.gain_state;
    out->tune_error = (int)radio.tune_error;
    out->gain_error = (int)radio.gain_error;
    out->receiver_streaming = radio.receiver_streaming ? 1 : 0;
    out->receiver_error = (int)radio.receiver_error;
    out->iq_level       = radio.receiver_streaming
                            ? (int)(FM.iq_level * 1000.0f) : 0;
    out->audio_level    = radio.receiver_streaming
                            ? (int)(FM.audio_level * 1000.0f) : 0;
    out->squelch_tenths = FM.squelch_tenths;
    out->squelch_open   = radio.receiver_streaming && FM.squelch_open ? 1 : 0;
    out->iq_bytes_sec   = radio.receiver_streaming ? FM.iq_bytes_sec : 0;
    out->read_errors    = (int)FM.read_errors;

    out->scan_start_hz  = FM.scan_start_hz;
    out->scan_stop_hz   = FM.scan_stop_hz;
    out->scan_peak_hz   = FM.scan_peak_hz;
    out->scan_peak_db   = (int)(FM.scan_peak_db * 10.0f);
    out->scan_sweeps    = FM.scan_sweeps;

    out->pocsag_baud   = FM.pocsag_baud;
    out->pocsag_auto   = FM.pocsag_auto ? 1 : 0;
    out->pocsag_sync   = FM.pocsag_sync ? 1 : 0;
    out->pocsag_pages  = FM.pocsag_pages;
    out->pocsag_frames = FM.pocsag_frames;

    for (int k = 0; k < FM.page_count; k++) {
        int idx = (FM.page_head - 1 - k + FM_PAGE_LOG_MAX * 2) % FM_PAGE_LOG_MAX;
        const fm_page_t *p = &FM.pages[idx];
        if (p->protocol != FM_PAGE_PROTOCOL_POCSAG) continue;
        out->pocsag_last_addr = p->address;
        out->pocsag_last_baud = p->baud;
        out->pocsag_last_type = p->type;
        strlcpy(out->pocsag_last_text, p->text, sizeof(out->pocsag_last_text));
        break;
    }
}

static const int ADSB_GAINS[] = { 0, 90, 200, 280, 340, 370, 400, 437, 463, 496 };

static void adsb_apply_gain(int g)
{
    const app_t *a = (s_adsb_idx >= 0) ? app_at(s_adsb_idx) : NULL;
    if (a) settings_set_gain(a, g);
    adsb_request_gain(g);
}

void lakeshark_adsb_gain_step(void)
{
    const int n = sizeof(ADSB_GAINS) / sizeof(ADSB_GAINS[0]);
    int cur = lakeshark_adsb_gain_tenths();
    int next_idx = 0;
    for (int i = 0; i < n; i++) if (ADSB_GAINS[i] == cur) { next_idx = (i + 1) % n; break; }
    adsb_apply_gain(ADSB_GAINS[next_idx]);
}

void lakeshark_adsb_agc(void)
{
    adsb_apply_gain(0);
}

int lakeshark_adsb_gain_tenths(void)
{
    const app_t *a = (s_adsb_idx >= 0) ? app_at(s_adsb_idx) : NULL;
    return a ? settings_get_gain(a) : 496;
}

void lakeshark_cartotui_set_enabled(bool en) { event_stream_set_enabled(en); }
bool lakeshark_cartotui_enabled(void)        { return event_stream_enabled(); }
