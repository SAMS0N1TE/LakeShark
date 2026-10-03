#include "speech_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>

#include "tts/tts.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace {

constexpr int    kChunk   = 256;
constexpr size_t kTextMax = 255;

/* The synthesizer, its output chunk and the unit being spoken: about 2 KB
   that would otherwise sit on the speech task's stack. */
struct Scratch {
    std::optional<tts::StreamSynth> synth;
    float   chunk[kChunk];
    int16_t pcm[kChunk];
    char    unit[kTextMax + 1];
};

tts::VoiceParams *s_voice[SPEECH_VOICE_COUNT];
Scratch          *s_scratch;

void *ext_alloc(size_t n)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return std::malloc(n);
}

/* Low and dark, after the old SAM "Elvis" preset in spirit only: these are
   formant-synthesizer settings, not a conversion of SAM's numbers. */
void make_voice(tts::VoiceParams &vp, speech_voice_t v)
{
    vp.f0_start = 78.0f;
    vp.f0_end = 72.0f;
    vp.final_fall_hz = 2.0f;
    vp.f0_accent_hz = 2.0f;
    vp.f0_flutter_hz = 0.0f;
    vp.jitter = 0.0f;
    vp.shimmer = 0.0f;
    vp.formant_scale = 0.86f;
    vp.duration_scale = 1.06f;
    vp.formant_smooth_ms = 8.0f;
    vp.glottal_open = 0.32f;
    vp.glottal_close = 0.10f;
    vp.output_gain = 0.13f;
    vp.lead_ms = 31.25f;
    vp.tail_ms = 62.5f;
    if (v == SPEECH_VOICE_GLITCH) {
        vp.f0_start = 65.0f;
        vp.f0_end = 62.0f;
        vp.formant_scale = 0.82f;
    }
    if (v == SPEECH_VOICE_FEMALE) {
        /* A synthetic woman, not a human one. The pitch sits level and moves
           only in semitone steps, up on a stressed syllable, so it reads as
           pitch-corrected rather than sung. Measured and exact, no breath, a
           tense bright source, and southern British vowels with no r after
           them. */
        vp.f0_start = 185.0f;
        vp.f0_end = 185.0f;
        vp.final_fall_hz = 0.0f;
        vp.f0_accent_hz = 22.0f;
        vp.f0_flutter_hz = 0.0f;
        vp.f0_step_semitones = 1.0f;
        vp.formant_scale = 1.12f;
        vp.duration_scale = 1.0f;
        vp.stress_len_scale = 1.08f;
        vp.glottal_open = 0.30f;
        vp.glottal_close = 0.06f;
        vp.breath = 0.0f;
        vp.formant_smooth_ms = 10.0f;
        vp.non_rhotic = 1.0f;
        /* Wider formants at this pitch, or a harmonic sitting on a narrow
           one rings several times louder than the rest of the word. */
        vp.bw_f0_coef = 0.45f;
        vp.output_gain = 0.040f;
        for (auto &p : vp.phones) {
            if (!std::strcmp(p.ipa, "\u025D")) { p.f2 = 1400; p.f3 = 2500; }  /* NURSE, no r */
            if (!std::strcmp(p.ipa, "\u0251")) { p.f1 = 650; p.f2 = 920; }    /* LOT, rounded */
            if (!std::strcmp(p.ipa, "o"))      { p.f1 = 480; p.f2 = 1180; }   /* GOAT, fronted */
        }
    }
}

/* Output stage, in the order it was tuned: bass lift, then either a 6-bit
   sample-and-hold with a slow two-level flutter (GLITCH) or plain 8-bit
   steps (DARK), then a one-pole low pass. FEMALE has none of that: a swept
   short delay and a lighter low pass. The constants assume 16 kHz. */
struct Effect {
    float    bass = 0.0f;
    float    lowpass = 0.0f;
    float    held = 0.0f;
    uint32_t n = 0;
    /* FEMALE's metallic shimmer: a copy of the voice a millisecond or two
       behind, the gap sweeping slowly, added back. */
    static constexpr int kComb = 64;
    float    line[kComb] = {};
    /* And level: open vowels at this pitch come out several times louder
       than the rest of the word, so peaks above a threshold are pressed
       down 3:1, attack at once, release over about 60 ms. */
    float    env = 0.0f;

    float step(float x, speech_voice_t v)
    {
        if (v == SPEECH_VOICE_FEMALE) {
            bass += 0.07f * (x - bass);   /* below ~180 Hz out: thin, like a PA */
            x -= bass;
            const float a = std::fabs(x);
            env = a > env ? a : env * 0.999f;
            constexpr float kKnee = 0.18f;
            if (env > kKnee) x *= std::pow(kKnee / env, 2.0f / 3.0f);
            x *= 2.2f;
            line[n % kComb] = x;
            const float sweep = 0.5f + 0.5f * std::sin(static_cast<float>(n) * 2.0f * 3.14159265f * 0.35f / SPEECH_RATE_HZ);
            const int d = 14 + static_cast<int>(sweep * 18.0f);
            x = 0.78f * x + 0.22f * line[(n + kComb - d) % kComb];
            ++n;
            lowpass += 0.85f * (x - lowpass);
            return std::clamp(lowpass, -0.98f, 0.98f);
        }
        bass += 0.075f * (x - bass);
        x = (x + 0.7f * bass) * 0.8f;
        if (v == SPEECH_VOICE_GLITCH) {
            if (n % 2 == 0) held = std::round(x * 63.0f) / 63.0f;
            x = held * ((n % 448) < 224 ? 1.0f : 0.72f);
        } else {
            x = std::round(x * 127.0f) / 127.0f;
        }
        ++n;
        lowpass += 0.55f * (x - lowpass);
        return std::clamp(lowpass, -0.98f, 0.98f);
    }
};

int16_t to_pcm(float x)
{
    if (!std::isfinite(x)) x = 0.0f;
    return static_cast<int16_t>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f));
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool has_word(const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            return true;
    }
    return false;
}

/* One unit: begin, then stream it through the effect into the sink. */
int render(speech_voice_t v, const char *text, size_t len,
           uint8_t *arena, size_t arena_size,
           speech_sink_fn sink, void *ctx, speech_engine_stats_t *st)
{
    Scratch &s = *s_scratch;
    std::memcpy(s.unit, text, len);
    s.unit[len] = 0;

    tts::StreamOptions opts;
    opts.sample_rate = static_cast<float>(SPEECH_RATE_HZ);

    int rc;
    try {
        s.synth.emplace(*s_voice[v], arena, arena_size);
        rc = s.synth->BeginText(s.unit, opts);
    } catch (const std::bad_alloc &) {
        return SPEECH_ENGINE_NO_MEMORY;
    } catch (...) {
        return SPEECH_ENGINE_BAD_ARG;
    }
    switch (rc) {
    case tts::kStreamOk:           break;
    case tts::kStreamErrArenaFull: return SPEECH_ENGINE_ARENA;
    case tts::kStreamErrNoPhones:  return SPEECH_ENGINE_NO_PHONES;
    default:                       return SPEECH_ENGINE_BAD_ARG;
    }
    st->arena_peak = std::max(st->arena_peak, s.synth->arena_used_bytes());
    st->units++;

    Effect fx;
    for (;;) {
        const int n = s.synth->Read(s.chunk, kChunk);
        if (n <= 0) break;
        for (int i = 0; i < n; ++i) s.pcm[i] = to_pcm(fx.step(s.chunk[i], v));
        if (!sink(s.pcm, n, ctx)) return SPEECH_ENGINE_STOPPED;
        st->samples += static_cast<uint32_t>(n);
    }
    return SPEECH_ENGINE_OK;
}

/* A unit that does not fit is halved at the space nearest its middle, so an
   identifier is spoken in two groups rather than cut short. */
int render_fit(speech_voice_t v, const char *text, size_t len,
               uint8_t *arena, size_t arena_size,
               speech_sink_fn sink, void *ctx, speech_engine_stats_t *st)
{
    if (!has_word(text, len)) return SPEECH_ENGINE_NO_PHONES;
    const int rc = render(v, text, len, arena, arena_size, sink, ctx, st);
    if (rc != SPEECH_ENGINE_ARENA) return rc;

    size_t cut = 0;
    for (size_t d = 0; d < len / 2 && !cut; ++d) {
        if (is_space(text[len / 2 + d])) cut = len / 2 + d;
        else if (d && is_space(text[len / 2 - d])) cut = len / 2 - d;
    }
    if (!cut) return SPEECH_ENGINE_ARENA;

    const int left = render_fit(v, text, cut, arena, arena_size, sink, ctx, st);
    if (left != SPEECH_ENGINE_OK && left != SPEECH_ENGINE_NO_PHONES) return left;
    return render_fit(v, text + cut + 1, len - cut - 1, arena, arena_size, sink, ctx, st);
}

void free_voices()
{
    for (auto &vp : s_voice) {
        if (!vp) continue;
        vp->~VoiceParams();
        std::free(vp);
        vp = nullptr;
    }
}

}  // namespace

/* All or nothing: a failure leaves nothing allocated, and a later call
   starts again. */
extern "C" int speech_engine_init(void)
{
    if (s_scratch) return SPEECH_ENGINE_OK;
    for (int v = 0; v < SPEECH_VOICE_COUNT; ++v) {
        void *mem = ext_alloc(sizeof(tts::VoiceParams));
        if (!mem) { free_voices(); return SPEECH_ENGINE_NO_MEMORY; }
        try {
            s_voice[v] = new (mem) tts::VoiceParams(tts::DefaultVoiceParams());
        } catch (const std::bad_alloc &) {
            std::free(mem);
            free_voices();
            return SPEECH_ENGINE_NO_MEMORY;
        }
        make_voice(*s_voice[v], static_cast<speech_voice_t>(v));
    }
    void *mem = ext_alloc(sizeof(Scratch));
    if (!mem) { free_voices(); return SPEECH_ENGINE_NO_MEMORY; }
    s_scratch = new (mem) Scratch();
    return SPEECH_ENGINE_OK;
}

/* Sentences are the units: each ends at . ! or ? and keeps its stop, so the
   pause and the falling pitch land where the text puts them. */
extern "C" int speech_engine_say(speech_voice_t voice, const char *text,
                                 uint8_t *arena, size_t arena_size,
                                 speech_sink_fn sink, void *ctx,
                                 speech_engine_stats_t *stats)
{
    speech_engine_stats_t local{};
    speech_engine_stats_t *st = stats ? stats : &local;
    *st = speech_engine_stats_t{};

    if (!s_scratch || !text || !sink || !arena ||
        voice < 0 || voice >= SPEECH_VOICE_COUNT)
        return SPEECH_ENGINE_BAD_ARG;
    const size_t len = strnlen(text, kTextMax + 1);
    if (len > kTextMax) return SPEECH_ENGINE_BAD_ARG;

    bool spoke = false;
    size_t i = 0;
    while (i < len) {
        while (i < len && is_space(text[i])) ++i;
        size_t end = i;
        while (end < len) {
            const char c = text[end];
            /* A point between digits is a decimal ("433.920"), not a stop. */
            const bool decimal = c == '.' && end > 0 && end + 1 < len &&
                                 is_digit(text[end - 1]) && is_digit(text[end + 1]);
            if ((c == '.' && !decimal) || c == '!' || c == '?') break;
            ++end;
        }
        if (end < len) ++end;
        if (has_word(text + i, end - i)) {
            const int rc = render_fit(voice, text + i, end - i, arena, arena_size,
                                      sink, ctx, st);
            if (rc == SPEECH_ENGINE_OK) spoke = true;
            else if (rc != SPEECH_ENGINE_NO_PHONES) return rc;
        }
        i = end;
    }
    return spoke ? SPEECH_ENGINE_OK : SPEECH_ENGINE_NO_PHONES;
}

extern "C" const char *speech_voice_name(speech_voice_t voice)
{
    switch (voice) {
    case SPEECH_VOICE_GLITCH: return "glitch";
    case SPEECH_VOICE_DARK:   return "dark";
    case SPEECH_VOICE_FEMALE: return "female";
    default:                  return "?";
    }
}
