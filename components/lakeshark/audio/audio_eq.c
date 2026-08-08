#include "audio_eq.h"
#include "settings.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "audio_eq";

#define EQ_BASS_HZ       300.0f
#define EQ_BASS_Q        0.85f
#define EQ_TREB_HZ       2600.0f
#define EQ_TREB_S        0.80f
#define EQ_PUNCH_IN_HZ   110.0f
#define EQ_PUNCH_IN_Q    0.90f
#define EQ_PUNCH_OUT_HZ  300.0f
#define EQ_PUNCH_OUT_Q   0.70f
#define EQ_PUNCH_TAU_S   0.030f
#define EQ_DC_HZ         12.0f

#define EQ_LIM_CEIL      0.95f
#define EQ_LIM_ATK_S     0.0015f
#define EQ_LIM_REL_S     0.150f
#define EQ_DET_REL_S     0.100f

typedef struct {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;
} bq_t;

static const float HP_Q[2] = { 0.54120f, 1.30656f };

static const audio_eq_cfg_t EQ_PRESETS[AUDIO_EQ_PRESET_COUNT] = {
    { AUDIO_EQ_FLAT,   0,  0,  0,  0, 0 },
    { AUDIO_EQ_VOICE, 20,  6, -3, 20, 2 },
    { AUDIO_EQ_PUNCH, 16,  9, -2, 60, 2 },
    { AUDIO_EQ_FULL,  12,  5,  0, 35, 1 },
    { AUDIO_EQ_CUSTOM,18,  6, -2, 30, 1 },
};

static const char *EQ_NAMES[AUDIO_EQ_PRESET_COUNT] = {
    "flat", "voice", "punch", "full", "custom"
};

static const float LOUD_T[AUDIO_EQ_LOUD_MAX + 1] = { 0.95f, 0.50f, 0.28f, 0.16f };

static float          s_fs      = 16000.0f;
static audio_eq_cfg_t s_cfg;
static bool           s_ready   = false;
static volatile bool  s_dirty   = false;

static bool  s_use_hp, s_use_bass, s_use_treb, s_use_punch, s_active;
static float s_punch_mix, s_loud_t, s_loud_makeup;
static float s_dc_a, s_punch_env_c, s_lim_atk, s_lim_rel, s_det_rel;

static bq_t  s_hp[2], s_bass, s_treb, s_pin, s_pout;
static float s_dc_x1, s_dc_y1;
static float s_punch_env;
static float s_det, s_gain;
static float s_gr_min = 1.0f;

static void bq_norm(bq_t *f, float b0, float b1, float b2, float a0, float a1, float a2)
{
    float ia = 1.0f / a0;
    f->b0 = b0 * ia; f->b1 = b1 * ia; f->b2 = b2 * ia;
    f->a1 = a1 * ia; f->a2 = a2 * ia;
}

static void bq_clear(bq_t *f)
{
    f->x1 = f->x2 = f->y1 = f->y2 = 0.0f;
}

static inline float bq_run(bq_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

static void mk_highpass(bq_t *f, float f0, float q)
{
    float w0 = 2.0f * (float)M_PI * f0 / s_fs;
    float cw = cosf(w0), sw = sinf(w0);
    float al = sw / (2.0f * q);
    bq_norm(f, (1.0f + cw) * 0.5f, -(1.0f + cw), (1.0f + cw) * 0.5f,
               1.0f + al, -2.0f * cw, 1.0f - al);
}

static void mk_peaking(bq_t *f, float f0, float q, float db)
{
    float A  = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / s_fs;
    float cw = cosf(w0), sw = sinf(w0);
    float al = sw / (2.0f * q);
    bq_norm(f, 1.0f + al * A, -2.0f * cw, 1.0f - al * A,
               1.0f + al / A, -2.0f * cw, 1.0f - al / A);
}

static void mk_highshelf(bq_t *f, float f0, float s, float db)
{
    float A  = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * (float)M_PI * f0 / s_fs;
    float cw = cosf(w0), sw = sinf(w0);
    float al = (sw * 0.5f) * sqrtf((A + 1.0f / A) * (1.0f / s - 1.0f) + 2.0f);
    float be = 2.0f * sqrtf(A) * al;
    bq_norm(f,
            A * ((A + 1.0f) + (A - 1.0f) * cw + be),
            -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw),
            A * ((A + 1.0f) + (A - 1.0f) * cw - be),
            (A + 1.0f) - (A - 1.0f) * cw + be,
            2.0f * ((A - 1.0f) - (A + 1.0f) * cw),
            (A + 1.0f) - (A - 1.0f) * cw - be);
}

static void mk_bandpass(bq_t *f, float f0, float q)
{
    float w0 = 2.0f * (float)M_PI * f0 / s_fs;
    float cw = cosf(w0), sw = sinf(w0);
    float al = sw / (2.0f * q);
    bq_norm(f, al, 0.0f, -al, 1.0f + al, -2.0f * cw, 1.0f - al);
}

static float tau_coef(float tau_s)
{
    return 1.0f - expf(-1.0f / (tau_s * s_fs));
}

static void clamp_cfg(audio_eq_cfg_t *c)
{
    if (c->preset >= AUDIO_EQ_PRESET_COUNT) c->preset = AUDIO_EQ_VOICE;
    if (c->hp10 != 0) {
        if (c->hp10 < AUDIO_EQ_HP_MIN) c->hp10 = AUDIO_EQ_HP_MIN;
        if (c->hp10 > AUDIO_EQ_HP_MAX) c->hp10 = AUDIO_EQ_HP_MAX;
    }
    if (c->bass_db < AUDIO_EQ_BASS_MIN) c->bass_db = AUDIO_EQ_BASS_MIN;
    if (c->bass_db > AUDIO_EQ_BASS_MAX) c->bass_db = AUDIO_EQ_BASS_MAX;
    if (c->treb_db < AUDIO_EQ_TREB_MIN) c->treb_db = AUDIO_EQ_TREB_MIN;
    if (c->treb_db > AUDIO_EQ_TREB_MAX) c->treb_db = AUDIO_EQ_TREB_MAX;
    if (c->punch > 100) c->punch = 100;
    if (c->loud > AUDIO_EQ_LOUD_MAX) c->loud = AUDIO_EQ_LOUD_MAX;
}

static void rebuild(void)
{
    clamp_cfg(&s_cfg);

    s_use_hp    = (s_cfg.hp10 != 0);
    s_use_bass  = (s_cfg.bass_db != 0);
    s_use_treb  = (s_cfg.treb_db != 0);
    s_use_punch = (s_cfg.punch != 0);
    s_active    = s_use_hp || s_use_bass || s_use_treb || s_use_punch || (s_cfg.loud != 0);

    if (s_use_hp) {
        float f0 = (float)s_cfg.hp10 * 10.0f;
        for (int i = 0; i < 2; i++) mk_highpass(&s_hp[i], f0, HP_Q[i]);
    }
    if (s_use_bass)  mk_peaking(&s_bass, EQ_BASS_HZ, EQ_BASS_Q, (float)s_cfg.bass_db);
    if (s_use_treb)  mk_highshelf(&s_treb, EQ_TREB_HZ, EQ_TREB_S, (float)s_cfg.treb_db);
    if (s_use_punch) {
        mk_bandpass(&s_pin,  EQ_PUNCH_IN_HZ,  EQ_PUNCH_IN_Q);
        mk_bandpass(&s_pout, EQ_PUNCH_OUT_HZ, EQ_PUNCH_OUT_Q);
    }

    s_punch_mix   = (float)s_cfg.punch * 0.03f;
    s_loud_t      = LOUD_T[s_cfg.loud];
    s_loud_makeup = EQ_LIM_CEIL / s_loud_t;

    s_dc_a        = 1.0f - (2.0f * (float)M_PI * EQ_DC_HZ / s_fs);
    s_punch_env_c = tau_coef(EQ_PUNCH_TAU_S);
    s_lim_atk     = tau_coef(EQ_LIM_ATK_S);
    s_lim_rel     = tau_coef(EQ_LIM_REL_S);
    s_det_rel     = tau_coef(EQ_DET_REL_S);
}

void audio_eq_reset_state(void)
{
    for (int i = 0; i < 2; i++) bq_clear(&s_hp[i]);
    bq_clear(&s_bass); bq_clear(&s_treb); bq_clear(&s_pin); bq_clear(&s_pout);
    s_dc_x1 = s_dc_y1 = 0.0f;
    s_punch_env = 0.0f;
    s_det = 0.0f;
    s_gain = 1.0f;
    s_gr_min = 1.0f;
}

void audio_eq_init(int rate_hz)
{
    if (rate_hz > 0) s_fs = (float)rate_hz;

    s_cfg.preset  = (uint8_t)settings_eq_preset_get();
    if (s_cfg.preset >= AUDIO_EQ_PRESET_COUNT) s_cfg.preset = AUDIO_EQ_VOICE;

    if (s_cfg.preset == AUDIO_EQ_CUSTOM) {
        s_cfg.hp10    = (uint8_t)settings_eq_hp_get();
        s_cfg.bass_db = (int8_t) settings_eq_bass_get();
        s_cfg.treb_db = (int8_t) settings_eq_treb_get();
        s_cfg.punch   = (uint8_t)settings_eq_punch_get();
        s_cfg.loud    = (uint8_t)settings_eq_loud_get();
    } else {
        uint8_t p = s_cfg.preset;
        s_cfg = EQ_PRESETS[p];
    }

    rebuild();
    audio_eq_reset_state();
    s_ready = true;

    ESP_LOGI(TAG, "eq ready: %s hp=%dHz bass=%+d treb=%+d punch=%d loud=%d",
             EQ_NAMES[s_cfg.preset], s_cfg.hp10 * 10, s_cfg.bass_db,
             s_cfg.treb_db, s_cfg.punch, s_cfg.loud);
}

void audio_eq_get(audio_eq_cfg_t *out)
{
    if (out) *out = s_cfg;
}

void audio_eq_set(const audio_eq_cfg_t *in)
{
    if (!in) return;

    audio_eq_cfg_t c = *in;
    clamp_cfg(&c);
    if (!memcmp(&c, &s_cfg, sizeof(c))) return;

    s_cfg = c;
    s_dirty = true;

    settings_eq_preset_set(s_cfg.preset);
    if (s_cfg.preset == AUDIO_EQ_CUSTOM) {
        settings_eq_hp_set(s_cfg.hp10);
        settings_eq_bass_set(s_cfg.bass_db);
        settings_eq_treb_set(s_cfg.treb_db);
        settings_eq_punch_set(s_cfg.punch);
        settings_eq_loud_set(s_cfg.loud);
    }
}

bool audio_eq_apply_preset(int preset)
{
    if (preset < 0 || preset >= AUDIO_EQ_PRESET_COUNT) return false;
    if (preset == AUDIO_EQ_CUSTOM) {
        audio_eq_cfg_t c = s_cfg;
        c.preset = AUDIO_EQ_CUSTOM;
        audio_eq_set(&c);
    } else {
        audio_eq_set(&EQ_PRESETS[preset]);
    }
    return true;
}

const char *audio_eq_preset_name(int preset)
{
    if (preset < 0 || preset >= AUDIO_EQ_PRESET_COUNT) return "?";
    return EQ_NAMES[preset];
}

int audio_eq_preset_from_name(const char *s)
{
    if (!s || !*s) return -1;
    for (int i = 0; i < AUDIO_EQ_PRESET_COUNT; i++) {
        const char *n = EQ_NAMES[i];
        int k = 0;
        while (n[k] && s[k] && n[k] == (char)tolower((unsigned char)s[k])) k++;
        if (!n[k] && !s[k]) return i;
    }
    return -1;
}

bool audio_eq_enabled(void) { return s_ready && s_active; }

int audio_eq_gr_db10(void)
{
    float g = s_gr_min;
    if (g >= 1.0f) return 0;
    if (g < 0.0001f) g = 0.0001f;
    return (int)lroundf(-200.0f * log10f(g));
}

void audio_eq_process(int16_t *pcm, int n)
{
    if (!s_ready) return;
    if (s_dirty) { s_dirty = false; rebuild(); }
    if (!s_active || n <= 0 || !pcm) return;

    const bool use_hp    = s_use_hp;
    const bool use_bass  = s_use_bass;
    const bool use_treb  = s_use_treb;
    const bool use_punch = s_use_punch;
    const float pmix     = s_punch_mix;
    const float thr      = s_loud_t;
    const float makeup   = s_loud_makeup;

    float gr_min = s_gr_min + 0.02f;
    if (gr_min > 1.0f) gr_min = 1.0f;

    for (int i = 0; i < n; i++) {
        float x = (float)pcm[i] * (1.0f / 32768.0f);

        float dc = x - s_dc_x1 + s_dc_a * s_dc_y1;
        s_dc_x1 = x;
        s_dc_y1 = dc;
        x = dc;

        float h = 0.0f;
        if (use_punch) {
            float lo = bq_run(&s_pin, x);
            float la = fabsf(lo);
            s_punch_env += (la - s_punch_env) * s_punch_env_c;
            h = bq_run(&s_pout, la - s_punch_env) * pmix;
        }

        if (use_hp) {
            x = bq_run(&s_hp[0], x);
            x = bq_run(&s_hp[1], x);
        }
        x += h;

        if (use_bass) x = bq_run(&s_bass, x);
        if (use_treb) x = bq_run(&s_treb, x);

        float a = fabsf(x);
        if (a > s_det) s_det = a;
        else           s_det += (a - s_det) * s_det_rel;

        float target = (s_det > thr) ? (thr / s_det) : 1.0f;
        s_gain += (target - s_gain) * ((target < s_gain) ? s_lim_atk : s_lim_rel);
        if (s_gain < gr_min) gr_min = s_gain;

        float y = x * s_gain * makeup * 32767.0f;
        if (y >  32767.0f) y =  32767.0f;
        if (y < -32768.0f) y = -32768.0f;
        pcm[i] = (int16_t)y;
    }

    s_gr_min = gr_min;

    if (!isfinite(s_gain) || !isfinite(s_det)) audio_eq_reset_state();
}
