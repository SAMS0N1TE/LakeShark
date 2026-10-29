/* Source-agnostic audio tools. DSP runs on the low-priority analysis worker,
   never on the codec callback. Snapshots keep the UI non-blocking. */
#include "ls_audio_tools.h"
#include "../../lakeshark/apps/fm/aprs.h"
#include "../../lakeshark/apps/fm/fm_tone.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define TOOL_ALLOC(n)                                                          \
  heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define TOOL_FREE(p) heap_caps_free(p)
#else
#define TOOL_ALLOC(n) calloc(1, n)
#define TOOL_FREE(p) free(p)
#endif
#define TOOL_PI 3.14159265358979323846f
#define EVENTS 64u
typedef struct {
  unsigned rate, resample_phase, dtmf_fill, cw_fill;
  uint64_t samples;
  float previous, previous16, b[3], a[2], z[2];
  float dtmf[320], dtmf_coeff[8], cw[128], cw_coeff[19], sub_samples[2];
  unsigned stable, release, cw_stable;
  char candidate, emitted, digits[32];
  bool cw_candidate, cw_on, cw_measured;
  unsigned cw_change;
  float cw_unit;
  char code[9], word[64];
  unsigned code_len, word_len;
  fm_tone_t subtone;
  unsigned reported;
  ls_tool_event events[EVENTS];
  unsigned head, count, total;
} tool_state;
struct ls_audio_tools {
  atomic_flag lock;
  aprs_ctx_t *aprs;
  uint32_t next, epoch;
  int16_t input[1024 * 2];
  tool_state s;
};
static void lock(ls_audio_tools *t) {
  while (atomic_flag_test_and_set_explicit(&t->lock, memory_order_acquire)) {
  }
}
static void unlock(ls_audio_tools *t) {
  atomic_flag_clear_explicit(&t->lock, memory_order_release);
}
static void event(ls_audio_tools *t, ls_tool_kind kind, const char *text) {
  tool_state *s = &t->s;
  ls_tool_event *e = &s->events[s->head];
  e->kind = kind;
  e->time_ms = (uint32_t)(s->samples / 16);
  snprintf(e->text, sizeof(e->text), "%s", text);
  s->head = (s->head + 1) % EVENTS;
  if (s->count < EVENTS)
    s->count++;
  s->total++;
}
static void packet(const aprs_packet_t *p, void *user) {
  ls_audio_tools *t = user;
  tool_state *s = &t->s;
  ls_tool_event *e = &s->events[s->head];
  e->kind = LS_TOOL_APRS;
  e->time_ms = (uint32_t)(s->samples / 16);
  snprintf(e->text, sizeof(e->text), "%s: %s", p->call, p->text);
  s->head = (s->head + 1) % EVENTS;
  if (s->count < EVENTS)
    s->count++;
  s->total++;
}
static void reset_state(ls_audio_tools *t) {
  memset(&t->s, 0, sizeof(t->s));
  t->s.cw_unit = 80;
  static const unsigned tones[8] = {697, 770, 852, 941, 1209, 1336, 1477, 1633};
  for (unsigned i = 0; i < 8; i++)
    t->s.dtmf_coeff[i] = 2 * cosf(2 * TOOL_PI * tones[i] / 16000);
  for (unsigned i = 0; i < 19; i++)
    t->s.cw_coeff[i] = 2 * cosf(2 * TOOL_PI * (300 + 50 * i) / 16000);
  fm_tone_init(&t->s.subtone);
  aprs_reset(t->aprs);
}
ls_audio_tools *ls_audio_tools_new(void) {
  ls_audio_tools *t = TOOL_ALLOC(sizeof(*t));
  if (!t)
    return NULL;
  atomic_flag_clear(&t->lock);
  t->aprs = aprs_create(packet, t);
  if (!t->aprs) {
    TOOL_FREE(t);
    return NULL;
  }
  reset_state(t);
  return t;
}
void ls_audio_tools_free(ls_audio_tools *t) {
  if (t) {
    aprs_destroy(t->aprs);
    TOOL_FREE(t);
  }
}
size_t ls_audio_tools_bytes(void) { return sizeof(ls_audio_tools); }
void ls_audio_tools_reset(ls_audio_tools *t) {
  if (t) {
    lock(t);
    reset_state(t);
    t->next = t->epoch = 0;
    unlock(t);
  }
}
static float goertzel(const float *x, unsigned n, float coefficient) {
  float a = 0, b = 0;
  for (unsigned i = 0; i < n; i++) {
    float v = x[i] + coefficient * a - b;
    b = a;
    a = v;
  }
  return fmaxf(0, 4 * (a * a + b * b - coefficient * a * b) / (n * n));
}
static void dtmf(ls_audio_tools *t) {
  tool_state *s = &t->s;
  float row = 0, column = 0, row2 = 0, column2 = 0;
  unsigned r = 0, c = 0;
  for (unsigned i = 0; i < 8; i++) {
    float p = goertzel(s->dtmf, 320, s->dtmf_coeff[i]);
    if (i < 4) {
      if (p > row) {
        row2 = row;
        row = p;
        r = i;
      } else if (p > row2)
        row2 = p;
    } else {
      if (p > column) {
        column2 = column;
        column = p;
        c = i - 4;
      } else if (p > column2)
        column2 = p;
    }
  }
  char digit = 0;
  if (row > .0025f && column > .0025f && row > row2 * 4 &&
      column > column2 * 4) {
    float twist = 10 * log10f(column / row);
    if (twist >= -8 && twist <= 4)
      digit = "123A456B789C*0#D"[r * 4 + c];
  }
  if (!digit) {
    if (++s->release >= 2) {
      s->emitted = s->candidate = 0;
      s->stable = 0;
    }
    return;
  }
  s->release = 0;
  if (digit == s->candidate)
    s->stable++;
  else {
    s->candidate = digit;
    s->stable = 1;
  }
  if (s->stable >= 2 && digit != s->emitted) {
    s->emitted = digit;
    size_t n = strlen(s->digits);
    if (n >= 31) {
      memmove(s->digits, s->digits + 1, 30);
      n = 30;
    }
    s->digits[n] = digit;
    s->digits[n + 1] = 0;
    char text[2] = {digit, 0};
    event(t, LS_TOOL_DTMF, text);
  }
}
static void cw_letter(tool_state *s) {
  static const char *const codes[] = {
      ".-",    "-...",  "-.-.",  "-..",   ".",     "..-.",  "--.",   "....",
      "..",    ".---",  "-.-",   ".-..",  "--",    "-.",    "---",   ".--.",
      "--.-",  ".-.",   "...",   "-",     "..-",   "...-",  ".--",   "-..-",
      "-.--",  "--..",  "-----", ".----", "..---", "...--", "....-", ".....",
      "-....", "--...", "---..", "----."};
  char decoded = '?';
  s->code[s->code_len] = 0;
  for (unsigned i = 0; i < 36; i++)
    if (!strcmp(s->code, codes[i])) {
      decoded = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[i];
      break;
    }
  if (s->word_len < sizeof(s->word) - 1) {
    s->word[s->word_len++] = decoded;
    s->word[s->word_len] = 0;
  }
  s->code_len = 0;
}
static void cw(ls_audio_tools *t) {
  tool_state *s = &t->s;
  float energy = 0, best = 0;
  for (unsigned i = 0; i < 128; i++)
    energy += s->cw[i] * s->cw[i] / 128;
  for (unsigned i = 0; i < 19; i++) {
    float p = goertzel(s->cw, 128, s->cw_coeff[i]);
    if (p > best)
      best = p;
  }
  bool on = energy > .001f && best > energy * 1.4f;
  if (on == s->cw_candidate)
    s->cw_stable++;
  else {
    s->cw_candidate = on;
    s->cw_stable = 1;
  }
  unsigned now = (unsigned)(s->samples / 16);
  if (s->cw_stable >= 2 && on != s->cw_on) {
    unsigned duration = now - s->cw_change;
    if (!on && duration >= 24 && duration <= 600) {
      bool dash = duration > s->cw_unit * 2;
      float unit = dash ? duration / 3.0f : (float)duration;
      if (unit >= 25 && unit <= 200) {
        s->cw_unit = s->cw_unit * .75f + unit * .25f;
        s->cw_measured = true;
      }
      if (s->code_len < 8)
        s->code[s->code_len++] = dash ? '-' : '.';
    }
    s->cw_on = on;
    s->cw_change = now;
  }
  if (!s->cw_on) {
    unsigned gap = now - s->cw_change;
    if (s->code_len && gap > s->cw_unit * 2.5f)
      cw_letter(s);
    if (s->word_len && gap > s->cw_unit * 6.5f) {
      event(t, LS_TOOL_CW, s->word);
      s->word_len = 0;
      s->word[0] = 0;
    }
  }
}
static void sample16(ls_audio_tools *t, float x) {
  tool_state *s = &t->s;
  s->samples++;
  s->sub_samples[0] = (s->previous16 + x) * .5f;
  s->sub_samples[1] = x;
  s->previous16 = x;
  fm_tone_process(&s->subtone, s->sub_samples, 2);
  unsigned selection = s->subtone.result.selection;
  if (selection && selection != s->reported) {
    /* The label buffer lives in PSRAM as part of the event slot. */
    ls_tool_event *e = &s->events[s->head];
    e->kind = selection <= FM_CTCSS_COUNT ? LS_TOOL_CTCSS : LS_TOOL_DCS;
    e->time_ms = (uint32_t)(s->samples / 16);
    fm_tone_label((uint16_t)selection, e->text, sizeof(e->text));
    s->head = (s->head + 1) % EVENTS;
    if (s->count < EVENTS)
      s->count++;
    s->total++;
  }
  s->reported = selection;
  int16_t pcm = (int16_t)(fmaxf(-1, fminf(1, x)) * 32767);
  aprs_process(t->aprs, &pcm, 1);
  s->dtmf[s->dtmf_fill++] = x;
  if (s->dtmf_fill == 320) {
    s->dtmf_fill = 0;
    dtmf(t);
  }
  s->cw[s->cw_fill++] = x;
  if (s->cw_fill == 128) {
    s->cw_fill = 0;
    cw(t);
  }
}
void ls_audio_tools_feed(ls_audio_tools *t, const int16_t *pcm, size_t frames,
                         unsigned channels, unsigned rate) {
  if (!t || !pcm || (channels != 1 && channels != 2) || rate < 8000 ||
      rate > 192000)
    return;
  lock(t);
  tool_state *s = &t->s;
  if (s->rate != rate) {
    uint64_t samples=s->samples;
    reset_state(t);
    s->samples=samples;
    s->rate = rate;
    float k = tanf(TOOL_PI * fminf(3400, rate * .4f) / rate),
          d = 1 + 1.41421356237f * k + k * k;
    s->b[0] = k * k / d;
    s->b[1] = 2 * s->b[0];
    s->b[2] = s->b[0];
    s->a[0] = 2 * (k * k - 1) / d;
    s->a[1] = (1 - 1.41421356237f * k + k * k) / d;
  }
  for (size_t i = 0; i < frames; i++) {
    float x = pcm[i * channels] * (1.0f / 32768);
    if (channels == 2)
      x = (x + pcm[i * 2 + 1] * (1.0f / 32768)) * .5f;
    float y = s->b[0] * x + s->z[0];
    s->z[0] = s->b[1] * x - s->a[0] * y + s->z[1];
    s->z[1] = s->b[2] * x - s->a[1] * y;
    s->resample_phase += 16000;
    while (s->resample_phase >= rate) {
      s->resample_phase -= rate;
      float f = 1 - (float)s->resample_phase / 16000;
      sample16(t, s->previous + (y - s->previous) * f);
    }
    s->previous = y;
  }
  unlock(t);
}
unsigned ls_audio_tools_pull(ls_audio_tools *t, const ls_audio_analysis *a,
                             unsigned max_frames) {
  if (!t || !a)
    return 0;
  unsigned total = 0;
  while (total < max_frames) {
    unsigned rate;
    uint32_t old = t->epoch, previous=t->next;
    size_t wanted = max_frames - total;
    if (wanted > 1024)
      wanted = 1024;
    size_t n =
        ls_audio_analysis_read(a, &t->next, &t->epoch, t->input, wanted, &rate);
    if (!n)
      break;
    if (t->epoch != old || t->next-(uint32_t)n!=previous) {
      lock(t);
      reset_state(t);
      t->s.samples=(uint64_t)ls_audio_analysis_origin_ms(a)*16+(uint64_t)(t->next-n)*16000/rate;
      unlock(t);
    }
    ls_audio_tools_feed(t, t->input, n, 2, rate);
    total += (unsigned)n;
  }
  return total;
}
bool ls_audio_tools_snapshot(ls_audio_tools *t, ls_tool_status *status,
                             ls_tool_event *events, unsigned capacity,
                             unsigned *count) {
  if (!t || !status || !count || (capacity && !events) ||
      atomic_flag_test_and_set_explicit(&t->lock, memory_order_acquire))
    return false;
  tool_state *s = &t->s;
  status->wpm = s->cw_measured ? 1200 / s->cw_unit : 0;
  status->events = s->total;
  fm_tone_label((uint16_t)s->subtone.result.selection, status->subtone,
                sizeof(status->subtone));
  snprintf(status->digits, sizeof(status->digits), "%s", s->digits);
  *count = s->count < capacity ? s->count : capacity;
  for (unsigned i = 0; i < *count; i++)
    events[i] = s->events[(s->head + EVENTS - *count + i) % EVENTS];
  unlock(t);
  return true;
}

/* Generator state and its settings also live in PSRAM. A generation counter
   makes a UI edit a single block-boundary change without waiting on audio. */
struct ls_audio_generator {
  atomic_uint generation, kind, start, end, duration;
  atomic_int dbfs;
  unsigned applied, rate, remaining, period, cached_kind;
  float phase, frequency, ratio, amplitude, start_frequency;
};
ls_audio_generator *ls_audio_generator_new(void) {
  ls_audio_generator *g = TOOL_ALLOC(sizeof(*g));
  if (g)
    ls_audio_generator_set(g, LS_GEN_SINE, 1000, 2000, 5000, -24);
  return g;
}
void ls_audio_generator_free(ls_audio_generator *g) { TOOL_FREE(g); }
bool ls_audio_generator_set(ls_audio_generator *g, ls_gen_kind kind,
                            unsigned start, unsigned end, unsigned duration,
                            int dbfs) {
  if (!g || kind > LS_GEN_SWEEP || start < 40 || start > 10000 || end < 40 ||
      end > 10000 || duration < 100 || duration > 30000 || dbfs < -60 ||
      dbfs > -12)
    return false;
  atomic_fetch_add(&g->generation, 1);
  atomic_store(&g->kind, kind);
  atomic_store(&g->start, start);
  atomic_store(&g->end, end);
  atomic_store(&g->duration, duration);
  atomic_store(&g->dbfs, dbfs);
  atomic_fetch_add(&g->generation, 1);
  return true;
}
void ls_audio_generator_render(ls_audio_generator *g, int16_t *pcm,
                               size_t frames, unsigned channels,
                               unsigned rate) {
  if (!g || !pcm || (channels != 1 && channels != 2) || rate < 8000 ||
      rate > 192000)
    return;
  unsigned generation = atomic_load(&g->generation);
  if (generation & 1) {
    memset(pcm, 0, frames * channels * sizeof(*pcm));
    return;
  }
  if (generation != g->applied || rate != g->rate) {
    unsigned kind = atomic_load(&g->kind), start = atomic_load(&g->start),
             end = atomic_load(&g->end), duration = atomic_load(&g->duration);
    int dbfs = atomic_load(&g->dbfs);
    if (atomic_load(&g->generation) != generation) {
      memset(pcm, 0, frames * channels * sizeof(*pcm));
      return;
    }
    g->cached_kind = kind;
    g->rate = rate;
    g->applied = generation;
    g->phase = 0;
    g->period = (unsigned)((uint64_t)duration * rate / 1000);
    g->remaining = g->period;
    g->frequency = g->start_frequency = fminf((float)start, rate * .45f);
    g->ratio =
        powf(fminf((float)end, rate * .45f) / g->frequency, 1.0f / g->period);
    g->amplitude = 32767 * powf(10, dbfs / 20.0f);
  }
  for (size_t i = 0; i < frames; i++) {
    float v = g->cached_kind == LS_GEN_SQUARE ? (g->phase < .5f ? 1 : -1)
                                              : sinf(2 * TOOL_PI * g->phase);
    int16_t sample = (int16_t)lrintf(v * g->amplitude);
    for (unsigned ch = 0; ch < channels; ch++)
      pcm[i * channels + ch] = sample;
    g->phase += g->frequency / rate;
    if (g->phase >= 1)
      g->phase -= floorf(g->phase);
    if (g->cached_kind == LS_GEN_SWEEP) {
      g->frequency *= g->ratio;
      if (!--g->remaining) {
        g->remaining = g->period;
        g->frequency = g->start_frequency;
      }
    }
  }
}
