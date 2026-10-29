/* NOAA Weather Radio glue for the FM receive task. See fm_wx.h. */
#include "fm_wx.h"

#include <string.h>

#include "air_scan.h"
#include "audio_out.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fm_air.h"
#include "fm_bcast.h"
#include "fm_state.h"
#include "lakeshark_backend.h"
#include "same_store.h"
#include "scan_engine.h"
#include "spectrum.h"

static const char *TAG = "fm_wx";

/* The longest a header keeps the speaker open without its end-of-message. */
#define WX_ALERT_MAX_US (10LL * 60 * 1000000)
/* About a second of looking before saying nothing is on the air. */
#define WX_FIND_BLOCKS 30

static air_scan_t *s_scan;            /* PSRAM, made on first FIND        */
static void (*s_tune)(uint32_t hz);
static bool s_finding, s_unmuted;
static int s_find_blocks;
static uint32_t s_home_hz;
static int64_t s_alert_until;
static fm_wx_status_t s_pub;

void fm_wx_install(void (*tune)(uint32_t hz)) { s_tune = tune; }
bool fm_wx_finding(void) { return s_finding; }

static void publish(void)
{
    s_pub.finding = s_finding;
    s_pub.alert_open = s_alert_until && esp_timer_get_time() < s_alert_until;
    s_pub.audio = fm_bcast_wx_audio();
    s_pub.channel = air_channel_find(air_preset(AIR_PRESET_WX), FM.freq_hz);
    fm_wx_publish(&s_pub);
}

void fm_wx_on_same(const same_alert_t *a, void *user)
{
    same_store_receive(a, user);
    same_options_t o;
    same_options_get(&o);
    const bool mine = (!a->test || o.include_tests) && (!o.only_mine || same_matches(a, &o));
    if (a->ended) s_alert_until = 0;
    else if (mine) s_alert_until = esp_timer_get_time() + WX_ALERT_MAX_US;
    ESP_LOGI(TAG, "SAME %s %s: speaker %s", a->event, a->ended ? "end" : "header",
             s_alert_until ? "open for the alert" : "as set");
    publish();
}

static void find_start(void)
{
    if (!s_scan) s_scan = heap_caps_calloc(1, sizeof(*s_scan), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scan) { ESP_LOGE(TAG, "no PSRAM to look for a weather channel"); return; }
    s_home_hz = FM.freq_hz;
    const air_action_t a = air_scan_start(s_scan, AIR_PRESET_WX, esp_timer_get_time());
    if (a.tune_hz && s_tune) s_tune(a.tune_hz);
    s_finding = true;
    s_find_blocks = 0;
    s_pub.found_none = false;
    memset(s_pub.level, 0, sizeof(s_pub.level));
    publish();
}

static void find_done(uint32_t hz)
{
    s_finding = false;
    air_scan_stop(s_scan);
    if (hz) {
        lakeshark_fm_frequency_lock(false);
        lakeshark_fm_set_freq(hz);
        ESP_LOGI(TAG, "strongest weather channel %.3f MHz", hz / 1e6);
    } else {
        s_pub.found_none = true;
        if (s_tune) s_tune(s_home_hz);
        ESP_LOGI(TAG, "no weather channel above the floor");
    }
    publish();
}

void fm_wx_block(const uint8_t *iq, int iq_len)
{
    if (!s_finding) return;
    const float *db = fm_air_measure(iq, iq_len);
    const air_action_t a = air_scan_block(s_scan, db, SPEC_FFT_N, FM_RTL_RATE, esp_timer_get_time());
    for (int i = 0; i < 7 && i < s_scan->nch; ++i) s_pub.level[i] = s_scan->level[i];
    if (a.listen) { find_done(a.listen_hz); return; }
    if (a.tune_hz && s_tune) s_tune(a.tune_hz);
    if (++s_find_blocks >= WX_FIND_BLOCKS) find_done(0);
    else publish();
}

void fm_wx_poll(void)
{
    int req, arg;
    while (fm_air_take(&req, &arg)) {
        if (req == FM_AIR_REQ_WX_FIND && !s_finding) find_start();
    }
    if (s_alert_until && esp_timer_get_time() >= s_alert_until) { s_alert_until = 0; publish(); }
}

void fm_wx_audio(fm_dsp_t *dsp, int demod_samples, const int16_t *pcm, int n)
{
    const int mode = fm_bcast_wx_audio();
    if (mode == FM_WX_AUDIO_OFF) return;
    if (!s_unmuted) { audio_out_ensure_unmuted(); s_unmuted = true; }
    const bool alert = s_alert_until && esp_timer_get_time() < s_alert_until;
    const bool open = mode == FM_WX_AUDIO_ALERTS ? alert
                    : fm_nfm_squelch(dsp, FM.squelch_tenths, demod_samples) != 0;
    FM.noise = dsp->demod_noise;
    FM.squelch_open = open;
    if (open && pcm && n > 0 && scan_engine_audio_open()) audio_write_mono(pcm, n);
}

void fm_wx_stop(void)
{
    if (s_finding) { s_finding = false; air_scan_stop(s_scan); }
    s_unmuted = false;
    publish();
}
