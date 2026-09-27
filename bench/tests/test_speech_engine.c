/* The spoken announcements, rendered on the host through the same adapter
   and synthesizer the firmware links. Set LS_SPEECH_WAV_DIR to also write
   each clip as a WAV for listening. */
#include "ls_test.h"
#include "speech_engine.h"
#include "plane_audio.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BYTES (64 * 1024)
#define MAX_SAMPLES (20 * SPEECH_RATE_HZ)

static uint8_t s_arena[ARENA_BYTES];
static int16_t s_pcm[MAX_SAMPLES];

typedef struct {
    int      n;
    int      calls;
    int      stop_after;   /* sink returns false once this many samples arrived */
    int      peak;
} capture_t;

static bool capture_sink(const int16_t *pcm, int n, void *ctx)
{
    capture_t *c = (capture_t *)ctx;
    c->calls++;
    for (int i = 0; i < n && c->n < MAX_SAMPLES; i++) {
        s_pcm[c->n++] = pcm[i];
        const int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > c->peak) c->peak = a;
    }
    return !(c->stop_after && c->n >= c->stop_after);
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 65535); put16(f, v >> 16); }

static void maybe_write_wav(const char *name, int n)
{
    const char *dir = getenv("LS_SPEECH_WAV_DIR");
    if (!dir || !*dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.wav", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + (unsigned)n * 2);
    fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, SPEECH_RATE_HZ); put32(f, SPEECH_RATE_HZ * 2); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (unsigned)n * 2);
    fwrite(s_pcm, 2, (size_t)n, f);
    fclose(f);
}

static int say(speech_voice_t v, const char *text, size_t arena,
               capture_t *c, speech_engine_stats_t *st)
{
    memset(c, 0, sizeof(*c));
    return speech_engine_say(v, text, s_arena, arena, capture_sink, c, st);
}

static double rms(int n)
{
    double sq = 0;
    for (int i = 0; i < n; i++) sq += (double)s_pcm[i] * s_pcm[i];
    return n ? sqrt(sq / n) / 32767.0 : 0;
}

LS_CASE(init_builds_both_voices)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    LS_EQ_STR(speech_voice_name(SPEECH_VOICE_GLITCH), "glitch");
    LS_EQ_STR(speech_voice_name(SPEECH_VOICE_DARK), "dark");
}

/* Every phrase the firmware says, in both voices, inside the arena the
   firmware allocates. */
LS_CASE(every_announcement_renders_within_the_arena)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    char contact[160], lost[160], pos[160];
    plane_phrase_new_contact(contact, sizeof(contact), 0xA1B2C3, "N123AB",
                             PLANE_GA, false);
    plane_phrase_lost_contact(lost, sizeof(lost), 0xA1B2C3, "UAL1234");
    plane_phrase_position(pos, sizeof(pos), 0xADF7C9, NULL);
    const struct { const char *name, *text; } P[] = {
        { "welcome", "WELCOME." },
        { "ready",   "RECEIVER READY." },
        { "check",   "TEST. THIS IS THE A D S B VOICE CHECK." },
        { "contact", contact },
        { "lost",    lost },
        { "position", pos },
        { "more",    "AND 7 MORE." },
    };
    for (int v = 0; v < SPEECH_VOICE_COUNT; v++) {
        for (size_t i = 0; i < sizeof(P) / sizeof(P[0]); i++) {
            capture_t c; speech_engine_stats_t st;
            LS_EQ_INT(say((speech_voice_t)v, P[i].text, ARENA_BYTES, &c, &st),
                      SPEECH_ENGINE_OK);
            LS_CHECK(st.arena_peak > 0 && st.arena_peak <= ARENA_BYTES);
            LS_EQ_INT(st.samples, c.n);
            LS_CHECK(c.n > SPEECH_RATE_HZ / 2);
            LS_CHECK_MSG(c.peak <= 32112, "%s peak %d", P[i].name, c.peak);
            LS_CHECK_MSG(rms(c.n) > 0.02, "%s is near silent", P[i].name);
            ls_note("%-6s %-8s %5.2f s  %u unit(s)  arena %6u B  peak %5d  rms %.3f  [%s]",
                    speech_voice_name((speech_voice_t)v), P[i].name,
                    (double)c.n / SPEECH_RATE_HZ, (unsigned)st.units,
                    (unsigned)st.arena_peak, c.peak, rms(c.n), P[i].text);
            char name[48];
            snprintf(name, sizeof(name), "%s-%s", speech_voice_name((speech_voice_t)v), P[i].name);
            maybe_write_wav(name, c.n);
        }
    }
}

/* The longest contact the phrase builder can make: shaky CRC, eight of the
   longest phonetic words, and the longest category. */
LS_CASE(longest_contact_is_spoken_whole_in_bounded_units)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    char phrase[160];
    plane_phrase_new_contact(phrase, sizeof(phrase), 0, "NNNNNNNN", PLANE_GA, true);
    LS_CHECK(strstr(phrase, "NOVEMBER NOVEMBER NOVEMBER NOVEMBER "
                            "NOVEMBER NOVEMBER NOVEMBER NOVEMBER") != NULL);

    capture_t c; speech_engine_stats_t st;
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, phrase, ARENA_BYTES, &c, &st), SPEECH_ENGINE_OK);
    LS_CHECK(st.arena_peak <= ARENA_BYTES);
    const int whole = c.n;
    ls_note("longest: %.2f s in %u units, arena %u B",
            (double)whole / SPEECH_RATE_HZ, (unsigned)st.units, (unsigned)st.arena_peak);
    maybe_write_wav("glitch-longest", c.n);

    /* Half the arena: the callsign has to go in smaller groups, and all of
       it is still said. */
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, phrase, ARENA_BYTES / 2, &c, &st), SPEECH_ENGINE_OK);
    LS_CHECK(st.arena_peak <= ARENA_BYTES / 2);
    LS_CHECK(st.units > 4);
    LS_CHECK(c.n >= whole);
    ls_note("longest in half the arena: %.2f s in %u units, arena %u B",
            (double)c.n / SPEECH_RATE_HZ, (unsigned)st.units, (unsigned)st.arena_peak);
}

LS_CASE(same_text_renders_the_same_samples)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    capture_t c; speech_engine_stats_t st;
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "RECEIVER READY.", ARENA_BYTES, &c, &st), SPEECH_ENGINE_OK);
    const int n = c.n;
    int16_t *first = malloc((size_t)n * sizeof(int16_t));
    LS_CHECK(first != NULL);
    if (!first) return;
    memcpy(first, s_pcm, (size_t)n * sizeof(int16_t));
    LS_EQ_INT(say(SPEECH_VOICE_DARK, "LOST CONTACT. ALFA.", ARENA_BYTES, &c, &st), SPEECH_ENGINE_OK);
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "RECEIVER READY.", ARENA_BYTES, &c, &st), SPEECH_ENGINE_OK);
    LS_EQ_INT(c.n, n);
    LS_CHECK(memcmp(first, s_pcm, (size_t)n * sizeof(int16_t)) == 0);
    free(first);
}

LS_CASE(a_stopping_sink_ends_the_announcement)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    capture_t c; speech_engine_stats_t st;
    memset(&c, 0, sizeof(c));
    c.stop_after = 1000;
    LS_EQ_INT(speech_engine_say(SPEECH_VOICE_GLITCH,
                                "NEW CONTACT. ALFA BRAVO. COMMERCIAL.",
                                s_arena, ARENA_BYTES, capture_sink, &c, &st),
              SPEECH_ENGINE_STOPPED);
    LS_CHECK(c.n >= 1000 && c.n < 1000 + 256);
    LS_EQ_INT(st.units, 1);
}

LS_CASE(bad_input_is_refused_without_output)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    capture_t c; speech_engine_stats_t st;
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "", ARENA_BYTES, &c, &st), SPEECH_ENGINE_NO_PHONES);
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, " . ! ?", ARENA_BYTES, &c, &st), SPEECH_ENGINE_NO_PHONES);
    LS_EQ_INT(c.calls, 0);
    LS_EQ_INT(say(SPEECH_VOICE_COUNT, "HELLO.", ARENA_BYTES, &c, &st), SPEECH_ENGINE_BAD_ARG);
    LS_EQ_INT(speech_engine_say(SPEECH_VOICE_GLITCH, NULL, s_arena, ARENA_BYTES,
                                capture_sink, &c, &st), SPEECH_ENGINE_BAD_ARG);

    char longtext[300];
    memset(longtext, 'A', sizeof(longtext) - 1);
    longtext[sizeof(longtext) - 1] = 0;
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, longtext, ARENA_BYTES, &c, &st), SPEECH_ENGINE_BAD_ARG);

    /* One word that cannot fit is an error, not a truncated word. */
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "NOVEMBER.", 1024, &c, &st), SPEECH_ENGINE_ARENA);
    LS_EQ_INT(c.calls, 0);
}

LS_CASE(a_decimal_point_is_not_a_sentence_end)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    capture_t c; speech_engine_stats_t st;
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "AT 1.5 MILES.", ARENA_BYTES, &c, &st), SPEECH_ENGINE_OK);
    LS_EQ_INT(st.units, 1);
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "READY. 1.5.", ARENA_BYTES, &c, &st), SPEECH_ENGINE_OK);
    LS_EQ_INT(st.units, 2);
    /* Past what the number reader can name it reads digit by digit; this
       many digits is one word too long for the arena, and is refused rather
       than cut. */
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "12345678901234567890123.", ARENA_BYTES, &c, &st),
              SPEECH_ENGINE_ARENA);
    LS_EQ_INT(c.calls, 0);
}

LS_CASE(non_ascii_and_digits_do_not_break_the_front_end)
{
    LS_EQ_INT(speech_engine_init(), SPEECH_ENGINE_OK);
    capture_t c; speech_engine_stats_t st;
    LS_EQ_INT(say(SPEECH_VOICE_GLITCH, "CAF\xC3\x89 1090 MHZ, 3.5 DB.", ARENA_BYTES, &c, &st),
              SPEECH_ENGINE_OK);
    LS_CHECK(c.n > 0);
}
