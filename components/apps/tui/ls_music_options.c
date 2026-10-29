#include "ls_music_options.h"
#include "esp_attr.h"
#include "ls_music_backend.h"
#include <math.h>
#include <stdio.h>

enum {
  FFT,
  WINDOW,
  OVERLAP,
  AXIS,
  FLOOR,
  RANGE,
  GAIN,
  LOW,
  HIGH,
  SPEED,
  SEEK,
  LOOP_A,
  LOOP_B,
  LOOP_OFF,
  MARK_ADD,
  MARK_NEXT,
  MARK_CLEAR,
  GEN_KIND,
  GEN_HZ,
  GEN_END,
  GEN_SECONDS,
  GEN_LEVEL,
  GEN_RUN
};
EXT_RAM_BSS_ATTR static struct {
  ls_audio_layers *cursor;
  uint32_t loop_a;
  int kind, hz, end, seconds, level;
} state;
void ls_music_options_cursor(ls_audio_layers *p) {
  state.cursor = p;
  if (!state.hz) {
    state.hz = 1000;
    state.end = 5000;
    state.seconds = 5;
    state.level = -24;
  }
}
static int get(const ls_opt_t *o) {
  ls_aa_config c = ls_audio_analysis_config(ls_music_analysis());
  switch (o->arg) {
  case FFT: {
    int i = 0;
    for (unsigned n = 256; n < c.fft_size && i < 4; n *= 2)
      i++;
    return i;
  }
  case WINDOW:
    return c.window;
  case OVERLAP:
    return c.hop == c.fft_size / 8 ? 2 : c.hop == c.fft_size / 4 ? 1 : 0;
  case AXIS:
    return c.log_axis;
  case GEN_KIND:
    return state.kind;
  default:
    return 0;
  }
}
static void set(const ls_opt_t *o, int v) {
  ls_audio_analysis *a = ls_music_analysis();
  ls_aa_config c = ls_audio_analysis_config(a);
  switch (o->arg) {
  case FFT: {
    unsigned div = c.hop ? c.fft_size / c.hop : 4;
    c.fft_size = 256u << (v < 0 ? 0 : v > 4 ? 4 : v);
    c.hop = c.fft_size / div;
    break;
  }
  case WINDOW:
    c.window = v ? LS_AA_BLACKMAN_HARRIS : LS_AA_HANN;
    break;
  case OVERLAP:
    c.hop = c.fft_size / (2u << (v < 0 ? 0 : v > 2 ? 2 : v));
    break;
  case AXIS:
    c.log_axis = v != 0;
    break;
  case GEN_KIND:
    state.kind = v < 0 ? 0 : v > 2 ? 2 : v;
    return;
  default:
    return;
  }
  ls_audio_analysis_configure(a, &c);
}
static double num(const ls_opt_t *o) {
  ls_aa_config c = ls_audio_analysis_config(ls_music_analysis());
  switch (o->arg) {
  case FLOOR:
    return c.floor_db;
  case RANGE:
    return c.range_db;
  case GAIN:
    return c.gain_db;
  case LOW:
    return c.lo_hz;
  case HIGH:
    return c.hi_hz;
  case SPEED:
    return ls_music_speed_get() / 1000.0;
  case GEN_HZ:
    return state.hz;
  case GEN_END:
    return state.end;
  case GEN_SECONDS:
    return state.seconds;
  case GEN_LEVEL:
    return state.level;
  default:
    return get(o);
  }
}
static void set_num(const ls_opt_t *o, double value) {
  double v = fmax(o->lo, fmin(o->hi, value));
  ls_audio_analysis *a = ls_music_analysis();
  ls_aa_config c = ls_audio_analysis_config(a);
  switch (o->arg) {
  case FLOOR:
    c.floor_db = (float)v;
    break;
  case RANGE:
    c.range_db = (float)v;
    break;
  case GAIN:
    c.gain_db = (float)v;
    break;
  case LOW:
    c.lo_hz = fminf((float)v, c.hi_hz - 1);
    break;
  case HIGH:
    c.hi_hz = fmaxf((float)v, c.lo_hz + 1);
    break;
  case SPEED:
    ls_music_speed_set((unsigned)lround(v * 1000));
    return;
  case GEN_HZ:
    state.hz = (int)v;
    return;
  case GEN_END:
    state.end = (int)v;
    return;
  case GEN_SECONDS:
    state.seconds = (int)v;
    return;
  case GEN_LEVEL:
    state.level = (int)v;
    return;
  default:
    set(o, (int)v);
    return;
  }
  ls_audio_analysis_configure(a, &c);
}
static void act(const ls_opt_t *o) {
  uint32_t now = ls_music_position_ms();
  switch (o->arg) {
  case SEEK:
    ls_music_seek_ms(ls_audio_layers_time(state.cursor));
    break;
  case LOOP_A:
    state.loop_a = now;
    ls_music_loop_set(0, 0, false);
    break;
  case LOOP_B:
    if (now > state.loop_a)
      ls_music_loop_set(state.loop_a, now, true);
    break;
  case LOOP_OFF:
    ls_music_loop_set(0, 0, false);
    break;
  case MARK_ADD:
    ls_music_marker_add();
    break;
  case MARK_CLEAR:
    ls_music_markers_clear();
    break;
  case MARK_NEXT: {
    uint32_t target = 0;
    unsigned n = ls_music_marker_count();
    for (unsigned i = 0; i < n; i++) {
      ls_music_marker_get(i, &target);
      if (target > now)
        break;
    }
    if (n) {
      if (target <= now)
        ls_music_marker_get(0, &target);
      ls_music_seek_ms(target);
    }
    break;
  }
  case GEN_RUN:
    if (ls_music_tone_active())
      ls_music_mic_stop();
    else
      ls_music_tone_start((ls_gen_kind)state.kind, state.hz, state.end,
                          state.seconds * 1000, state.level);
    break;
  }
}
static void show(const ls_opt_t *o, char *out, size_t n) {
  uint32_t a, b;
  if (o->arg == GEN_RUN)
    snprintf(out, n, "%s", ls_music_tone_active() ? "STOP" : "START");
  else if (o->arg == MARK_ADD || o->arg == MARK_CLEAR || o->arg == MARK_NEXT)
    snprintf(out, n, "%u saved", ls_music_marker_count());
  else if (o->arg == SEEK)
    snprintf(out, n, "%.3f s", ls_audio_layers_time(state.cursor) / 1000.0);
  else if (o->arg == LOOP_A)
    snprintf(out, n, "%.3f s", state.loop_a / 1000.0);
  else if (ls_music_loop_get(&a, &b))
    snprintf(out, n, "%.2f - %.2f s", a / 1000.0, b / 1000.0);
  else
    snprintf(out, n, "OFF");
}
static const char *const sizes[] = {"256", "512", "1024", "2048", "4096"};
static const char *const windows[] = {"HANN", "BLACKMAN-HARRIS"};
static const char *const overlaps[] = {"50%", "75%", "87.5%"};
static const char *const axes[] = {"LINEAR", "LOG"};
static const char *const waves[] = {"SINE", "SQUARE", "SWEEP"};
#define CYCLE(label_, id_, names_)                                             \
  {.label = label_,                                                            \
   .kind = LS_OPT_CYCLE,                                                       \
   .arg = id_,                                                                 \
   .names = names_,                                                            \
   .n = sizeof(names_) / sizeof(names_[0]),                                    \
   .step = 1,                                                                  \
   .get = get,                                                                 \
   .set = set}
#define LEVEL(label_, id_, lo_, hi_, step_, unit_)                             \
  {.label = label_,                                                            \
   .kind = LS_OPT_LEVEL,                                                       \
   .arg = id_,                                                                 \
   .lo = lo_,                                                                  \
   .hi = hi_,                                                                  \
   .step = step_,                                                              \
   .unit = unit_,                                                              \
   .num = num,                                                                 \
   .set_num = set_num}
#define ACTION(label_, id_)                                                    \
  {.label = label_, .kind = LS_OPT_ACTION, .arg = id_, .act = act, .show = show}
static const ls_opt_t analysis[] = {
    CYCLE("FFT", FFT, sizes),
    CYCLE("WINDOW", WINDOW, windows),
    CYCLE("OVERLAP", OVERLAP, overlaps),
    CYCLE("AXIS", AXIS, axes),
    LEVEL("FLOOR", FLOOR, -130, -20, 5, "dBFS"),
    LEVEL("RANGE", RANGE, 20, 120, 5, "dB"),
    LEVEL("GAIN", GAIN, -30, 30, 1, "dB display gain"),
    LEVEL("LOW", LOW, 1, 1000, 10, "Hz"),
    LEVEL("HIGH", HIGH, 1600, 22000, 100, "Hz")};
static const ls_opt_t playback[] = {
    LEVEL("VARISPEED", SPEED, .5, 2, .1, "x, pitch changes"),
    ACTION("SEEK CURSOR", SEEK),
    ACTION("SET LOOP A", LOOP_A),
    ACTION("SET B / LOOP", LOOP_B),
    ACTION("LOOP OFF", LOOP_OFF),
    ACTION("ADD MARKER", MARK_ADD),
    ACTION("NEXT MARKER", MARK_NEXT),
    ACTION("CLEAR MARKERS", MARK_CLEAR)};
static const ls_opt_t generator[] = {
    CYCLE("WAVE", GEN_KIND, waves),
    LEVEL("FREQUENCY", GEN_HZ, 40, 10000, 10, "Hz"),
    LEVEL("SWEEP END", GEN_END, 40, 10000, 10, "Hz"),
    LEVEL("SWEEP TIME", GEN_SECONDS, 1, 30, 1, "seconds"),
    LEVEL("LEVEL", GEN_LEVEL, -60, -12, 1, "dBFS, obeys mute and volume"),
    ACTION("GENERATOR", GEN_RUN)};
const ls_opt_ctx_t ls_music_analysis_options = {.name = "ANALYSIS",
                                                .job = -1,
                                                .radio = LS_RSEL_NONE,
                                                LS_OPT_ROWS(analysis)};
const ls_opt_ctx_t ls_music_playback_options = {.name = "PLAYBACK",
                                                .job = -1,
                                                .radio = LS_RSEL_NONE,
                                                LS_OPT_ROWS(playback)};
const ls_opt_ctx_t ls_music_generator_options = {.name = "GENERATOR",
                                                 .job = -1,
                                                 .radio = LS_RSEL_NONE,
                                                 LS_OPT_ROWS(generator)};
