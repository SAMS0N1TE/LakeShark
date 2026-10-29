/* Airband / CB scanner glue for the FM receive task. See fm_air.h. */
#include "fm_air.h"

#include <stdio.h>
#include <string.h>

#include "air_scan.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fm_bcast.h"
#include "fm_hf.h"
#include "fm_state.h"
#include "hf_demod.h"
#include "spectrum.h"

static const char *TAG = "fm_air";

/* All PSRAM, made on first use: the scanner (about 5 KB), an FFT context,
   the spectrum it reads and the rotated block. */
static air_scan_t *s_air;
static spectrum_t *s_spec;
static float *s_db;
static uint8_t *s_rot;
static void (*s_tune)(uint32_t hz);
static uint32_t s_home_hz;
static uint32_t s_listen_hz;
static bool s_was_listening;
static int s_publish_tick;
static int s_thresh_req;

#define AIR_FRAMES 8

void fm_air_install(void (*tune)(uint32_t hz)) { s_tune = tune; }

/* CB USB/LSB through the HF chain's demodulator (hf_demod.h). The block
   arrives turned so the channel's suppressed carrier sits at 0 Hz, at
   FM_RTL_RATE, so the demod runs with no tune offset and the dongle's DC
   spike stays AIR_LISTEN_OFS away, far outside a 2.8 kHz sideband. Its own
   state, PSRAM, made on the first SSB channel heard: the HF chain's s_hf in
   app_fm.c stands down while the scanner owns the LO, and keeping the two
   apart means neither drags the other's AGC across a band change. The
   filter pair, AGC, blanker and clarifier follow the HF options, so CB SSB
   sounds like the same receiver as 11 m tuned by hand. */
_Static_assert(FM_RTL_RATE == HF_IN_RATE, "the scanner's blocks are the HF demod's input rate");
static hf_demod_t *s_ssb;
static bool s_ssb_failed;       /* no PSRAM: SSB presets play through AM    */
static int s_ssb_demod = -1;    /* fm_hf_demod_t configured, -1 none        */
static uint32_t s_ssb_gen;
static uint32_t s_ssb_hz;       /* the channel its histories belong to      */

int fm_air_ssb(int demod, fm_dsp_t *dsp, const uint8_t *iq, int iq_len,
               int16_t *pcm, int max)
{
    if (demod != AIR_DEMOD_USB && demod != AIR_DEMOD_LSB) return -1;
    if (!s_ssb && !s_ssb_failed) {
        s_ssb = heap_caps_calloc(1, sizeof(*s_ssb), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ssb) {
            s_ssb_failed = true;
            ESP_LOGE(TAG, "no PSRAM for the SSB demodulator: CB USB/LSB play as AM");
            return -1;
        }
        hf_demod_init(s_ssb);
        s_ssb_demod = -1;
    }
    if (!s_ssb) return -1;
    const int want = demod == AIR_DEMOD_USB ? FM_HF_USB : FM_HF_LSB;
    if (want != s_ssb_demod || fm_hf_generation() != s_ssb_gen) {
        fm_hf_settings_t h = *fm_hf_get();
        h.demod = (uint8_t)want;
        const hf_demod_cfg_t cfg = {
            .mode = (hf_demod_mode_t)want, .filter_hz = fm_hf_filter_hz(&h),
            .tune_hz = 0, .rit_hz = h.rit_hz, .cw_pitch_hz = h.pitch_hz,
            .agc = (hf_agc_t)h.agc, .nb = (hf_nb_t)h.nb,
        };
        hf_demod_set(s_ssb, &cfg);
        s_ssb_demod = want;
        s_ssb_gen = fm_hf_generation();
    }
    if (s_listen_hz != s_ssb_hz) { hf_demod_reset(s_ssb); s_ssb_hz = s_listen_hz; }
    const int n = hf_demod_u8(s_ssb, iq, (size_t)iq_len, pcm, max);
    /* The meter on AM's scale, read through the HF meter as app_fm.c does
       for the HF chain. */
    dsp->iq_peak = fm_hf_meter(hf_demod_level_dbfs(s_ssb)) * 0.1f;
    return n;
}

static bool ensure(void)
{
    if (s_air && s_spec && s_db && s_rot) return true;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (!s_air) s_air = heap_caps_calloc(1, sizeof(*s_air), caps);
    if (!s_spec) {
        s_spec = heap_caps_calloc(1, sizeof(*s_spec), caps);
        if (s_spec) spectrum_context_init(s_spec);
    }
    if (!s_db) s_db = heap_caps_calloc(SPEC_FFT_N, sizeof(float), caps);
    if (!s_rot) s_rot = heap_caps_malloc(16384, caps);
    if (s_air && s_spec && s_db && s_rot) return true;
    ESP_LOGE(TAG, "no PSRAM for the channel scanner");
    return false;
}

bool fm_air_running(void) { return s_air && air_scan_active(s_air); }
uint32_t fm_air_listen_hz(void) { return s_listen_hz; }

static void publish(void)
{
    static EXT_RAM_BSS_ATTR fm_air_status_t st;
    memset(&st, 0, sizeof(st));
    st.now_us = esp_timer_get_time();
    if (s_air && air_scan_active(s_air)) {
        const air_scan_t *a = s_air;
        st.preset = a->preset;
        st.phase = a->phase;
        st.lo_hz = a->lo_hz;
        st.listen_hz = s_listen_hz;
        st.listen_ch = a->phase == AIR_PH_LISTEN ? a->listen_ch : -1;
        st.nch = a->nch;
        st.win = a->win;
        st.nwin = a->nwin;
        const int k0 = a->win_first[a->win], k1 = k0 + a->win_n[a->win] - 1;
        st.win_lo_hz = air_channel_hz(a->p, a->p->list ? a->order[k0] : k0);
        st.win_hi_hz = air_channel_hz(a->p, a->p->list ? a->order[k1] : k1);
        st.carrier = a->carrier && a->phase == AIR_PH_LISTEN;
        st.held = a->held;
        st.guard_on = a->guard_on && a->guard_ch >= 0;
        st.guard_now = a->phase == AIR_PH_GUARD;
        st.demod_as_am = (a->p->demod == AIR_DEMOD_USB || a->p->demod == AIR_DEMOD_LSB) &&
                         s_ssb_failed;
        st.thresh_db = a->thresh_db;
        st.sweeps = a->sweeps;
        st.sweep_ms = a->sweep_ms;
        st.images = a->images_rejected;
        st.lockouts = air_scan_lockout_count(a);
        st.nhits = a->nhits;
        memcpy(st.hits, a->hits, sizeof(st.hits));
        fm_air_publish(&st, a->level, a->nch);
    } else {
        fm_air_publish(&st, NULL, 0);
    }
}

static void apply(air_action_t a)
{
    if (a.tune_hz && s_tune) s_tune(a.tune_hz);
    s_listen_hz = a.listen ? a.listen_hz : 0;
    /* The dial follows the scan: the window being measured, or the channel
       being heard. */
    if (a.listen) FM.freq_hz = a.listen_hz;
    else if (a.tune_hz) FM.freq_hz = a.tune_hz;
}

void fm_air_stop(bool go_home)
{
    if (!s_air || !air_scan_active(s_air)) return;
    air_scan_stop(s_air);
    s_listen_hz = 0;
    s_ssb_hz = 0;
    s_was_listening = false;
    if (go_home && s_home_hz) { FM.freq_hz = s_home_hz; if (s_tune) s_tune(s_home_hz); }
    publish();
}

void fm_air_poll(uint32_t home_hz)
{
    int req, arg;
    while (fm_air_take(&req, &arg)) {
        const int64_t now = esp_timer_get_time();
        if (req == FM_AIR_REQ_PRESET) {
            if (!arg) { fm_air_stop(true); continue; }
            if (!ensure()) continue;
            if (!air_scan_active(s_air)) s_home_hz = home_hz;
            if (s_thresh_req) s_air->thresh_db = (float)s_thresh_req;
            apply(air_scan_start(s_air, arg, now));
            s_air->guard_on = s_air->guard_on && !fm_bcast_opt(FM_BC_GUARD_OFF);
            ESP_LOGI(TAG, "%s: %d channels in %d windows%s", s_air->p->name, s_air->nch,
                     s_air->nwin, s_air->guard_on ? ", guard 121.5" : "");
            publish();
            continue;
        }
        if (req == FM_AIR_REQ_THRESH) {
            s_thresh_req = arg;
            if (s_air) s_air->thresh_db = (float)arg;
            continue;
        }
        if (!s_air || !air_scan_active(s_air)) continue;
        switch (req) {
        case FM_AIR_REQ_SKIP:    apply(air_scan_skip(s_air, now)); break;
        case FM_AIR_REQ_LOCKOUT: apply(air_scan_lockout(s_air, now)); break;
        case FM_AIR_REQ_HOLD:    air_scan_hold(s_air, arg != 0); break;
        case FM_AIR_REQ_LISTEN:  apply(air_scan_listen(s_air, arg, now)); break;
        case FM_AIR_REQ_CLEAR_LOCKOUTS: air_scan_clear_lockouts(s_air); break;
        default: break;
        }
        publish();
    }
    if (s_air && air_scan_active(s_air))
        s_air->guard_on = s_air->guard_ch >= 0 && !fm_bcast_opt(FM_BC_GUARD_OFF);
}

/* Eight windowed 512-point frames spread over the block, power-averaged. */
static bool measure(const uint8_t *iq, int iq_len)
{
    const int nsamp = iq_len / 2;
    if (nsamp < SPEC_FFT_N) return false;
    spectrum_context_reset(s_spec);
    const int stride = (nsamp - SPEC_FFT_N) / (AIR_FRAMES - 1);
    for (int f = 0; f < AIR_FRAMES; ++f) {
        const uint8_t *p = iq + 2 * f * stride;
        for (int i = 0; i < SPEC_FFT_N; ++i) {
            const float w = s_spec->win[i];
            s_spec->re[i] = ((float)p[2 * i] - 127.5f) * (1.0f / 127.5f) * w;
            s_spec->im[i] = ((float)p[2 * i + 1] - 127.5f) * (1.0f / 127.5f) * w;
        }
        spectrum_context_accum(s_spec);
    }
    return spectrum_context_read_db(s_spec, s_db, SPEC_FFT_N);
}

/* The channel sits AIR_LISTEN_OFS (fs/4) below the LO: multiply by j^n to
   bring it to 0 Hz. Exact on 8-bit samples: negation about 127.5 is 255-x. */
static void rotate_up_quarter(const uint8_t *in, uint8_t *out, int len)
{
    for (int n = 0, k = 0; k + 1 < len; ++n, k += 2) {
        const uint8_t i = in[k], q = in[k + 1];
        switch (n & 3) {
        case 0: out[k] = i;       out[k + 1] = q;       break;
        case 1: out[k] = 255 - q; out[k + 1] = i;       break;
        case 2: out[k] = 255 - i; out[k + 1] = 255 - q; break;
        default: out[k] = q;      out[k + 1] = 255 - i; break;
        }
    }
}

_Static_assert(AIR_LISTEN_OFS * 4 == FM_RTL_RATE, "the listen offset is a quarter of the rate");

int fm_air_block(fm_dsp_t *dsp, const uint8_t *iq, int iq_len, int16_t *pcm, int max)
{
    if (!fm_air_running()) return 0;
    const int64_t now = esp_timer_get_time();
    const bool ok = measure(iq, iq_len);
    const air_action_t a = air_scan_block(s_air, ok ? s_db : NULL, SPEC_FFT_N, FM_RTL_RATE, now);
    const bool listening_before = s_was_listening;
    int out = 0;
    /* Demodulate the block as it was taken, before any retune this action
       asks for: it belongs to the channel the LO was parked on. */
    if (s_listen_hz && listening_before && iq_len <= 16384) {
        rotate_up_quarter(iq, s_rot, iq_len);
        if (s_air->p->demod == AIR_DEMOD_USB || s_air->p->demod == AIR_DEMOD_LSB)
            out = fm_air_ssb(s_air->p->demod, dsp, s_rot, iq_len, pcm, max);
        if (out < 0 || (s_air->p->demod != AIR_DEMOD_USB && s_air->p->demod != AIR_DEMOD_LSB))
            out = fm_demod_am(dsp, s_rot, iq_len, pcm, max);
        if (!a.audio) out = 0;
    }
    const uint32_t was = s_listen_hz;
    apply(a);
    s_was_listening = s_listen_hz != 0 && !a.tune_hz;
    if (s_listen_hz && s_listen_hz != was) fm_dsp_init(dsp);
    if (a.tune_hz || ++s_publish_tick >= 3) { s_publish_tick = 0; publish(); }
    return out;
}

const char *fm_air_summary(char *out, int n)
{
    if (!fm_air_running()) { snprintf(out, (size_t)n, "off"); return out; }
    const air_scan_t *a = s_air;
    snprintf(out, (size_t)n, "%s sweeps=%lu (%lu ms) win=%d/%d %s%.4f hits=%d locked=%d images=%lu guard=%s",
             a->p->name, (unsigned long)a->sweeps, (unsigned long)a->sweep_ms, a->win + 1, a->nwin,
             a->phase == AIR_PH_LISTEN ? (a->carrier ? "LISTEN " : "hang ") : "lo ",
             (a->phase == AIR_PH_LISTEN ? air_channel_hz(a->p, a->listen_ch) : a->lo_hz) / 1e6,
             a->nhits, air_scan_lockout_count(a), (unsigned long)a->images_rejected,
             a->guard_on && a->guard_ch >= 0 ? "on" : "off");
    return out;
}

const float *fm_air_measure(const uint8_t *iq, int iq_len)
{
    if (!ensure()) return NULL;
    return measure(iq, iq_len) ? s_db : NULL;
}
