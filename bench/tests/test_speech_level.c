/* The spoken-callout level, end to end on the host: the real synthesizer and
   the real equalizer, with the speech level applied where audio_out.c's
   player applies it (audio_speech_level.h). The level used to be applied
   before the ring, ahead of an equalizer whose leveler flattens it; these
   cases fail if the level stops following its percentage at the output. */
#include "ls_test.h"
#include "speech_engine.h"
#include "audio_eq.h"
#include "audio_speech_level.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* audio_eq.c reads and saves its preset through settings; the host keeps
   the stock one (VOICE), and the cases choose presets directly. */
int  settings_eq_preset_get(void)  { return AUDIO_EQ_VOICE; }
void settings_eq_preset_set(int v) { (void)v; }
int  settings_eq_hp_get(void)      { return 18; }
void settings_eq_hp_set(int v)     { (void)v; }
int  settings_eq_bass_get(void)    { return 6; }
void settings_eq_bass_set(int v)   { (void)v; }
int  settings_eq_treb_get(void)    { return -2; }
void settings_eq_treb_set(int v)   { (void)v; }
int  settings_eq_punch_get(void)   { return 30; }
void settings_eq_punch_set(int v)  { (void)v; }
int  settings_eq_loud_get(void)    { return 1; }
void settings_eq_loud_set(int v)   { (void)v; }

#define ARENA_BYTES (64 * 1024)
#define MAX_SAMPLES (20 * SPEECH_RATE_HZ)
#define CHUNK       256   /* the engine's chunk and the player's read */

static uint8_t s_arena[ARENA_BYTES];

typedef struct {
    int16_t raw[MAX_SAMPLES];   /* what the speech worker hands audio_write_speech */
    int     n;
} clip_t;

static bool grab(const int16_t *pcm, int n, void *ctx)
{
    clip_t *c = (clip_t *)ctx;
    for (int i = 0; i < n && c->n < MAX_SAMPLES; i++) c->raw[c->n++] = pcm[i];
    return true;
}

static void render(clip_t *c, speech_voice_t v, const char *text)
{
    memset(c, 0, sizeof(*c));
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    LS_EQ_INT(speech_engine_say(v, text, s_arena, sizeof(s_arena), grab, c, NULL),
              SPEECH_ENGINE_OK);
    LS_CHECK(c->n > SPEECH_RATE_HZ / 2);
}

static void select_preset(int preset)
{
    audio_eq_init(SPEECH_RATE_HZ);
    LS_CHECK(audio_eq_apply_preset(preset));
    audio_eq_reset_state();
}

/* The ring and the player, as audio_out.c has them: byte positions, the
   speech marked before it is visible, 256-sample reads, equalizer, then the
   level. `queue_first` lets the whole announcement sit in the ring before the
   player starts; otherwise the player keeps up with the worker. */
static void play(const clip_t *c, int pct, int *out, bool queue_first, uint32_t base)
{
    audio_speech_span_t span = { base, base };
    const int32_t gain = audio_speech_gain(pct);
    uint32_t wr = base, rd = base;
    int written = 0, played = 0;
    while (played < c->n) {
        if (written < c->n) {
            const int m = c->n - written < CHUNK ? c->n - written : CHUNK;
            audio_speech_span_add(&span, rd, wr, wr + 2u * (uint32_t)m);
            wr += 2u * (uint32_t)m;
            written += m;
            if (queue_first && written < c->n) continue;
        }
        while (played < written) {
            const int m = written - played < CHUNK ? written - played : CHUNK;
            int16_t buf[CHUNK];
            for (int i = 0; i < m; i++) buf[i] = c->raw[played + i];
            audio_eq_process(buf, m);
            audio_speech_level_apply(&span, rd, buf, m, gain);
            rd += 2u * (uint32_t)m;
            for (int i = 0; i < m; i++) out[played + i] = buf[i];
            played += m;
        }
    }
}

static double rms(const int *p, int n)
{
    double sq = 0;
    for (int i = 0; i < n; i++) sq += (double)p[i] * p[i];
    return n ? sqrt(sq / n) / 32768.0 : 0;
}

static double db(double ratio) { return 20.0 * log10(ratio); }

static int s_out[MAX_SAMPLES];
static clip_t s_clip;

/* Each step of the Settings > Voice > Level menu, and the ones between, has
   to come out that much quieter, whatever the equalizer is doing. */
LS_CASE(speech_output_follows_the_level_percentage_under_every_equalizer)
{
    static const char *const TEXT = "NEW CONTACT. NOVEMBER ONE TWO THREE ALFA BRAVO.";
    static const int PCT[] = { 100, 90, 75, 50, 25 };
    static const char *const PRESET[] = { "flat", "voice", "punch", "full" };
    static const int PRESETS[] = { AUDIO_EQ_FLAT, AUDIO_EQ_VOICE,
                                   AUDIO_EQ_PUNCH, AUDIO_EQ_FULL };

    for (int v = 0; v < SPEECH_VOICE_COUNT; v++) {
        render(&s_clip, (speech_voice_t)v, TEXT);
        for (unsigned e = 0; e < sizeof(PRESETS) / sizeof(PRESETS[0]); e++) {
            double ref = 0;
            for (unsigned k = 0; k < sizeof(PCT) / sizeof(PCT[0]); k++) {
                select_preset(PRESETS[e]);
                play(&s_clip, PCT[k], s_out, false, 0);
                const double r = rms(s_out, s_clip.n);
                if (k == 0) ref = r;
                LS_CHECK_MSG(r > 0.005, "%s/%s is near silent at %d%%",
                             speech_voice_name((speech_voice_t)v), PRESET[e], PCT[k]);
                const double want = db(PCT[k] / 100.0);
                const double got = db(r / ref);
                ls_note("%-6s %-5s level %3d%%  rms %.3f  %+6.2f dB (want %+6.2f)",
                        speech_voice_name((speech_voice_t)v), PRESET[e], PCT[k],
                        r, got, want);
                LS_CHECK_MSG(fabs(got - want) < 0.25,
                             "%s/%s: %d%% is %+.2f dB from 100%%, wanted %+.2f",
                             speech_voice_name((speech_voice_t)v), PRESET[e],
                             PCT[k], got, want);
            }
        }
    }
}

/* 100 % is the engine and equalizer exactly as they were: the level can only
   take away, so the codec volume stays the bound on how loud speech is. */
LS_CASE(full_level_changes_nothing_and_no_level_raises_anything)
{
    render(&s_clip, SPEECH_VOICE_DARK, "RECEIVER READY.");
    static int full[MAX_SAMPLES], other[MAX_SAMPLES];

    select_preset(AUDIO_EQ_VOICE);
    play(&s_clip, 100, full, false, 0);

    /* The same chain without the level stage at all. */
    select_preset(AUDIO_EQ_VOICE);
    for (int i = 0; i < s_clip.n; i += CHUNK) {
        const int m = s_clip.n - i < CHUNK ? s_clip.n - i : CHUNK;
        int16_t buf[CHUNK];
        memcpy(buf, s_clip.raw + i, (size_t)m * sizeof(int16_t));
        audio_eq_process(buf, m);
        for (int k = 0; k < m; k++) other[i + k] = buf[k];
    }
    LS_CHECK(memcmp(full, other, (size_t)s_clip.n * sizeof(int)) == 0);

    for (int pct = -5; pct <= 400; pct += 7) {
        select_preset(AUDIO_EQ_VOICE);
        play(&s_clip, pct, other, false, 0);
        for (int i = 0; i < s_clip.n; i++)
            if (abs(other[i]) > abs(full[i])) {
                LS_CHECK_MSG(0, "level %d%% raised sample %d: %d -> %d",
                             pct, i, full[i], other[i]);
                return;
            }
    }
    LS_EQ_INT(audio_speech_gain(400), AUDIO_SPEECH_UNITY);
    LS_EQ_INT(audio_speech_gain(-3), 0);
    LS_EQ_INT(audio_speech_gain(50), AUDIO_SPEECH_UNITY / 2);
}

/* Queued behind the worker or played as it goes, across the wrap of the
   32-bit byte counters, the output is the same. */
LS_CASE(the_level_does_not_depend_on_how_the_ring_was_filled)
{
    render(&s_clip, SPEECH_VOICE_GLITCH, "AND 7 MORE.");
    static int a[MAX_SAMPLES], b[MAX_SAMPLES], c[MAX_SAMPLES];
    select_preset(AUDIO_EQ_VOICE); play(&s_clip, 40, a, false, 0);
    select_preset(AUDIO_EQ_VOICE); play(&s_clip, 40, b, true, 0);
    select_preset(AUDIO_EQ_VOICE); play(&s_clip, 40, c, false, 0xFFFFFF00u);
    LS_CHECK(memcmp(a, b, (size_t)s_clip.n * sizeof(int)) == 0);
    LS_CHECK(memcmp(a, c, (size_t)s_clip.n * sizeof(int)) == 0);
}

/* Only speech takes the level. A tone or live radio written outside the
   span plays as it is. */
LS_CASE(only_the_speech_span_is_scaled)
{
    audio_speech_span_t span = { 1000, 1000 };
    audio_speech_span_add(&span, 1000, 1000, 1064);   /* 32 samples of speech */
    int16_t pcm[96];
    for (int i = 0; i < 96; i++) pcm[i] = 1000;
    /* The chunk starts 32 samples (64 bytes) before the speech. */
    audio_speech_level_apply(&span, 1000 - 64, pcm, 96, audio_speech_gain(50));
    for (int i = 0; i < 32; i++)  LS_EQ_INT(pcm[i], 1000);
    for (int i = 32; i < 64; i++) LS_EQ_INT(pcm[i], 500);
    for (int i = 64; i < 96; i++) LS_EQ_INT(pcm[i], 1000);

    /* Played out, the next announcement starts a new span at its own start
       instead of reaching back over the cue that was between them. */
    audio_speech_span_add(&span, 1064, 1200, 1264);
    LS_EQ_UINT(span.begin, 1200);
    LS_EQ_UINT(span.end, 1264);
    /* Still queued: the new speech joins it, and the gap is quieter, never
       louder. */
    audio_speech_span_add(&span, 1210, 1300, 1364);
    LS_EQ_UINT(span.begin, 1200);
    LS_EQ_UINT(span.end, 1364);
}

/* Why the level is applied here and not before the ring: through the stock
   equalizer, scaling first leaves 100, 75 and 50 % within a few dB of each
   other. Kept so that moving the stage back is a decision, not an accident. */
LS_CASE(scaling_before_the_equalizer_leveler_would_not_work)
{
    render(&s_clip, SPEECH_VOICE_GLITCH, "NEW CONTACT. NOVEMBER ONE TWO THREE.");
    double full = 0, half = 0;
    for (int pass = 0; pass < 2; pass++) {
        const int pct = pass ? 50 : 100;
        select_preset(AUDIO_EQ_VOICE);
        for (int i = 0; i < s_clip.n; i += CHUNK) {
            const int m = s_clip.n - i < CHUNK ? s_clip.n - i : CHUNK;
            int16_t buf[CHUNK];
            for (int k = 0; k < m; k++) buf[k] = (int16_t)(s_clip.raw[i + k] * pct / 100);
            audio_eq_process(buf, m);
            for (int k = 0; k < m; k++) s_out[i + k] = buf[k];
        }
        if (pass) half = rms(s_out, s_clip.n);
        else      full = rms(s_out, s_clip.n);
    }
    ls_note("level applied before the equalizer: 50%% is %+.2f dB from 100%% (a halving is -6.02)",
            db(half / full));
    LS_CHECK(fabs(db(half / full)) < 3.0);
}
