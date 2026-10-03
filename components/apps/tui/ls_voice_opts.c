/* See ls_voice_opts.h. A change that can be heard is heard at once,
   replacing any sample still playing. */
#include "ls_voice_opts.h"

#include <stdio.h>

#include "audio/audio_events.h"
#include "audio/audio_out.h"
#include "audio/speech.h"
#include "core/settings.h"

static void sample(const char *text)
{
    speech_cancel();
    audio_out_ensure_unmuted();
    speech_say_async(text);
}

static const char *no_speech(const ls_opt_t *o)
{
    (void)o;
    return speech_available() ? NULL : "No speech in this build";
}

/* ---------------------------------------------------------------- voice -- */

static void o_voice(const ls_opt_t *o)
{
    (void)o;
    speech_voice_step(+1);
    sample("RECEIVER READY.");
}
static void o_voice_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    snprintf(out, n, "%s", speech_voice_name(speech_voice_get()));
}

static double o_level(const ls_opt_t *o) { (void)o; return speech_volume_get(); }
static void o_set_level(const ls_opt_t *o, double v)
{
    (void)o;
    const int pct = (int)(v + 0.5);
    speech_volume_set(pct);
    settings_speech_volume_set(pct);
    sample("LEVEL.");
}
static void o_level_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    snprintf(out, n, "%d%%", speech_volume_get());
}

/* The codec's volume, which everything the speaker plays goes through. */
static double o_speaker(const ls_opt_t *o) { (void)o; return audio_volume_get(); }
static void o_set_speaker(const ls_opt_t *o, double v) { (void)o; audio_volume_set((int)(v + 0.5)); }
static void o_speaker_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    const int v = audio_volume_get();
    if (v > 0) snprintf(out, n, "%d%%", v);
    else snprintf(out, n, "OFF");
}

enum { TEST_ADSB, TEST_MESH, TEST_VOICE };
static void o_test(const ls_opt_t *o)
{
    switch (o->arg) {
    case TEST_ADSB:
        audio_out_ensure_unmuted();
        audio_events_play_test();
        break;
    case TEST_MESH: sample("MESSAGE FROM LAKESHARK. MESH VOICE CHECK."); break;
    default:        sample("RECEIVER READY."); break;
    }
}
static void o_test_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    snprintf(out, n, "say it now");
}

#define ROW_VOICE                                                              \
    { .label = "VOICE", .kind = LS_OPT_ACTION, .act = o_voice,                 \
      .show = o_voice_show, .why_not = no_speech }
#define ROW_LEVEL                                                              \
    { .label = "VOICE LEVEL", .kind = LS_OPT_LEVEL, .num = o_level,            \
      .set_num = o_set_level, .lo = 0, .hi = 100, .step = 5,                   \
      .unit = "% of the speaker, 0 to 100", .show = o_level_show,              \
      .why_not = no_speech }
#define ROW_SPEAKER                                                            \
    { .label = "SPEAKER", .kind = LS_OPT_LEVEL, .num = o_speaker,              \
      .set_num = o_set_speaker, .lo = 0, .hi = 100, .step = 5,                 \
      .unit = "volume, 0 to 100", .show = o_speaker_show }
#define ROW_TEST(t)                                                            \
    { .label = "TEST", .kind = LS_OPT_ACTION, .arg = (t), .act = o_test,       \
      .show = o_test_show, .why_not = no_speech }

/* ---------------------------------------------------------------- ADS-B -- */

static const char *const CALLOUT[] = { "OFF", "TONE", "SPOKEN" };
static int o_callout(const ls_opt_t *o) { return audio_event_mode_get((audio_evt_kind_t)o->arg); }
static void o_set_callout(const ls_opt_t *o, int v)
{
    audio_event_mode_set((audio_evt_kind_t)o->arg, (audio_mode_t)v);
}
#define ROW_CALLOUT(l, k)                                                      \
    { .label = (l), .kind = LS_OPT_CYCLE, .arg = (k), .names = CALLOUT,        \
      .n = AUD_MODE_COUNT, .get = o_callout, .set = o_set_callout }

/* ----------------------------------------------------------------- MESH -- */

static const char *const READ_OUT[] = { "OFF", "SENDER", "SENDER + TEXT" };
static int o_read_out(const ls_opt_t *o) { (void)o; return audio_events_mesh_say_get(); }
static void o_set_read_out(const ls_opt_t *o, int v) { (void)o; audio_events_mesh_say_set((audio_mesh_say_t)v); }

static const char *const WHICH[] = { "ALL", "DIRECT ONLY" };
static int o_which(const ls_opt_t *o) { (void)o; return audio_events_mesh_direct_only(); }
static void o_set_which(const ls_opt_t *o, int v) { (void)o; audio_events_mesh_set_direct_only(v != 0); }

#define ROW_READ_OUT                                                           \
    { .label = "READ OUT", .kind = LS_OPT_CYCLE, .names = READ_OUT,            \
      .n = AUD_MESH_COUNT, .get = o_read_out, .set = o_set_read_out,           \
      .why_not = no_speech }
#define ROW_WHICH                                                              \
    { .label = "MESSAGES", .kind = LS_OPT_TOGGLE, .names = WHICH,              \
      .get = o_which, .set = o_set_which, .why_not = no_speech }

/* ---------------------------------------------------------------- lists -- */

static const ls_opt_t OPT_ADSB[] = {
    ROW_CALLOUT("NEW AIRCRAFT", AUDIO_EVT_NEW_CONTACT),
    ROW_CALLOUT("LOST AIRCRAFT", AUDIO_EVT_LOST_CONTACT),
    ROW_CALLOUT("POSITION", AUDIO_EVT_POSITION),
    ROW_VOICE, ROW_LEVEL, ROW_SPEAKER, ROW_TEST(TEST_ADSB),
};
const ls_opt_ctx_t ls_voice_ctx_adsb = { .name = "VOICE", .job = -1,
                                         .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_ADSB) };

static const ls_opt_t OPT_MESH[] = {
    ROW_READ_OUT, ROW_WHICH,
    ROW_VOICE, ROW_LEVEL, ROW_SPEAKER, ROW_TEST(TEST_MESH),
};
const ls_opt_ctx_t ls_voice_ctx_mesh = { .name = "VOICE", .job = -1,
                                         .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_MESH) };

static const ls_opt_t OPT_ADSB_CALLS[] = {
    ROW_CALLOUT("NEW AIRCRAFT", AUDIO_EVT_NEW_CONTACT),
    ROW_CALLOUT("LOST AIRCRAFT", AUDIO_EVT_LOST_CONTACT),
    ROW_CALLOUT("POSITION", AUDIO_EVT_POSITION),
    ROW_TEST(TEST_ADSB),
};
static const ls_opt_ctx_t CTX_ADSB_CALLS = { .name = "ADS-B", .job = -1,
                                             .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_ADSB_CALLS) };

static const ls_opt_t OPT_MESH_CALLS[] = { ROW_READ_OUT, ROW_WHICH, ROW_TEST(TEST_MESH) };
static const ls_opt_ctx_t CTX_MESH_CALLS = { .name = "MESH", .job = -1,
                                             .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_MESH_CALLS) };

static const ls_opt_t OPT_ALL[] = {
    ROW_VOICE, ROW_LEVEL, ROW_SPEAKER, ROW_TEST(TEST_VOICE),
    { .label = "ADS-B CALLOUTS", .kind = LS_OPT_MENU, .sub = &CTX_ADSB_CALLS },
    { .label = "MESH MESSAGES", .kind = LS_OPT_MENU, .sub = &CTX_MESH_CALLS },
};
const ls_opt_ctx_t ls_voice_ctx_all = { .name = "VOICE", .job = -1,
                                        .radio = LS_RSEL_NONE, LS_OPT_ROWS(OPT_ALL) };
