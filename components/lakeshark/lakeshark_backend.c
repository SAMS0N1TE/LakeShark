
#include "lakeshark_backend.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"

#include "app_registry.h"
#include "settings.h"
#include "scan_channels.h"
#include "scan_engine.h"
#include "perf.h"
#include "event_bus.h"
#include "usb_host.h"

#include "p25_state.h"
#include "dsp_pipeline.h"
#include "rtl-sdr.h"

#include "audio_out.h"
#include "audio_events.h"
#include "sam_tts.h"
#include "tone.h"
#include "event_stream.h"

#include "bsp/esp32_p4_wifi6_touch_lcd_4b.h"

static const char *TAG = "lakeshark";

extern int adsb_app_register(void);
extern int p25_app_register(void);
extern int fm_app_register(void);

extern volatile int rtl_gain_request;
extern int          autoscan_bch_ok_flag;
extern int          dsd_bch_fail_counter;

static int s_adsb_idx = -1;
static int s_p25_idx  = -1;
static int s_fm_idx   = -1;
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
    scan_channels_init();

    event_stream_init();

    s_adsb_idx = adsb_app_register();
    s_p25_idx  = p25_app_register();
    s_fm_idx   = fm_app_register();

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

void lakeshark_radio_park(void)    { app_park();   }
void lakeshark_radio_unpark(void)  { app_unpark(); }
bool lakeshark_radio_running(void) { return !app_parked(); }

extern rtlsdr_dev_t *rtlsdr_dev_get(void);
bool lakeshark_radio_device_ready(void) { return rtlsdr_dev_get() != NULL; }

const char *lakeshark_recovery_take_app(void) { return app_recovery_take(); }

static void adsb_apply_gain(int g);

void lakeshark_radio_set_gain(int tenths)
{
    if (tenths < 0)   tenths = 0;
    if (tenths > 496) tenths = 496;
    int idx = app_current_index();
    if (idx == s_p25_idx) {
        rtl_gain_request = tenths;
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
    if (idx == s_p25_idx)      rtl_gain_request = tenths;
    else if (idx == s_fm_idx)  lakeshark_fm_set_gain_live(tenths);
}

static const char *MODE_NAMES[] = { "C4FM", "CQPSK", "DIFF_4FSK", "FSK4_TRACKING" };

void lakeshark_p25_tune(int delta_hz)
{
    const app_t *a = app_current();
    uint32_t f = a ? settings_get_freq(a) : s_tune_freq_hz;
    if (delta_hz < 0) {
        uint32_t d = (uint32_t)(-delta_hz);
        f = (f > d) ? f - d : f;
    } else {
        f += (uint32_t)delta_hz;
    }
    s_tune_freq_hz = f;
    if (a) settings_set_freq(a, f);
    s_p25_freq_req = f;
}

void lakeshark_p25_set_freq(uint32_t hz)
{
    if (hz < 1000000UL) return;
    const app_t *a = app_current();
    s_tune_freq_hz = hz;
    if (a) settings_set_freq(a, hz);
    s_p25_freq_req = hz;
}

uint32_t lakeshark_p25_get_freq(void) { return s_tune_freq_hz; }

const char *lakeshark_p25_cycle_mode(void)
{
    int next = (s_dsp.mode + 1) % 4;
    dsp_set_mode(&s_dsp, (demod_mode_t)next);
    return MODE_NAMES[next & 3];
}

int lakeshark_p25_mode_index(void) { return (int)s_dsp.mode; }

void lakeshark_p25_set_mode(int idx)
{
    if (idx < 0 || idx > 3) return;
    dsp_set_mode(&s_dsp, (demod_mode_t)idx);
}

const char *lakeshark_p25_mode_name(void)
{
    int m = s_dsp.mode;
    if (m < 0 || m > 3) return "?";
    return MODE_NAMES[m];
}

void lakeshark_p25_toggle_polarity(void)
{
    P25.demod_invert = !P25.demod_invert;
    P25.demod_gain   = P25.demod_invert ? 9000.0f : -9000.0f;
    dsp_set_gain(&s_dsp, P25.demod_gain);
}

bool lakeshark_p25_polarity_inverted(void) { return P25.demod_invert; }

void lakeshark_p25_reset_stats(void)
{
    P25.dsd_sync_count     = 0;
    P25.dsd_voice_count    = 0;
    P25.dsd_bch_ok_count   = 0;
    P25.dsd_bch_fail_count = 0;
    autoscan_bch_ok_flag   = 0;
    dsd_bch_fail_counter   = 0;
}

void lakeshark_p25_gain_step(void)
{
    static const int gains[] = { 0, 90, 200, 280, 340, 370, 400, 437, 463, 496 };
    const int n = sizeof(gains) / sizeof(gains[0]);
    int cur = P25.rtl_gain_tenths;
    int next_idx = 0;
    for (int i = 0; i < n; i++) if (gains[i] == cur) { next_idx = (i + 1) % n; break; }
    rtl_gain_request = gains[next_idx];
    const app_t *a = app_current();
    if (a) settings_set_gain(a, gains[next_idx]);
}

extern volatile bool p25_agc_on;

void lakeshark_p25_agc(void)
{
    p25_agc_on = false;
    rtl_gain_request = 280;
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

extern rtlsdr_dev_t *rtlsdr_dev_get(void);

static const int ADSB_GAINS[] = { 0, 90, 200, 280, 340, 370, 400, 437, 463, 496 };

static void adsb_apply_gain(int g)
{
    const app_t *a = (s_adsb_idx >= 0) ? app_at(s_adsb_idx) : NULL;
    if (a) settings_set_gain(a, g);
    rtlsdr_dev_t *dev = rtlsdr_dev_get();
    if (dev) {
        rtlsdr_set_tuner_gain_mode(dev, g == 0 ? 0 : 1);
        if (g > 0) rtlsdr_set_tuner_gain(dev, g);
        rtlsdr_set_agc_mode(dev, g == 0 ? 1 : 0);
    }
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
