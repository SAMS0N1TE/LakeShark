#include "ls_audio_layers.h"
#include "ls_pixel_wf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_cold.h"
#include "ls_textpack.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

struct ls_audio_layers {
  ls_pwf_t wf;
  uint16_t *pixels;
  size_t capacity;
  int w, h, layer, zoom, cw, ch;
  unsigned rate, epoch, fft_size, window;
  tui_rect plot, overview;
  uint32_t serial, time, start, span, duration, last_frames, slice_time;
  float hz, lo, hi;
  bool pinned, slice_pending;
  ls_audio_analysis *analysis;
  ls_audio_overview *source;
  ls_tool_status status;
  ls_tool_event events[12];
  unsigned count;
  ls_aa_track tracks[256];
  char text[224], chunk[224], tick[12];
  int16_t reader[2];
};
static const char *const notes[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};
static void *psalloc(size_t n) {
#ifdef ESP_PLATFORM
  return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return calloc(1, n);
#endif
}
ls_audio_layers *ls_audio_layers_new(void) {
  return psalloc(sizeof(ls_audio_layers));
}
void ls_audio_layers_free(ls_audio_layers *p) {
  if (!p)
    return;
  const uint16_t *shown = NULL;
  if (ls_tui_image_now(NULL, &shown, NULL, NULL, NULL) && shown == p->pixels)
    ls_tui_image(tui_rect_make(0, 0, 0, 0), NULL, 0, 0, 0);
  ls_pwf_free(&p->wf);
  free(p->pixels);
  free(p);
}
static int clamp(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
static int level(float v, int scale) {
  if (!isfinite(v))
    return v > 0 ? scale : 0;
  return clamp((int)(fminf(1, fmaxf(0, v)) * scale), 0, scale);
}
static void dot(ls_audio_layers *p, int x, int y, uint16_t c) {
  if (x >= 0 && x < p->w && y >= 0 && y < p->h)
    p->pixels[(size_t)y * p->w + x] = c;
}
static void line(ls_audio_layers *p, int x0, int y0, int x1, int y1,
                 uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0),
      sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) {
    dot(p, x0, y0, c);
    if (x0 == x1 && y0 == y1)
      break;
    int e2 = 2 * e;
    if (e2 >= dy) {
      e += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      e += dx;
      y0 += sy;
    }
  }
}
static uint16_t colour(float v) {
  v = fminf(1, fmaxf(0, v));
  unsigned r = (unsigned)(31 * fmaxf(0, (v - .45f) / .55f));
  unsigned g = (unsigned)(63 * fminf(1, v * 1.6f));
  unsigned b = (unsigned)(31 * fminf(1, v * 3));
  return (uint16_t)(r << 11 | g << 5 | b);
}
static float frequency(ls_audio_layers *p, float fraction) {
  ls_aa_config c = ls_audio_analysis_config(p->analysis);
  return p->layer == LS_LAYER_MELODIC || c.log_axis
             ? p->lo * powf(p->hi / p->lo, fraction)
             : p->lo + (p->hi - p->lo) * fraction;
}
static int freq_y(ls_audio_layers *p, float hz) {
  ls_aa_config c = ls_audio_analysis_config(p->analysis);
  float f = p->layer == LS_LAYER_MELODIC || c.log_axis
                ? logf(hz / p->lo) / logf(p->hi / p->lo)
                : (hz - p->lo) / (p->hi - p->lo);
  return clamp((int)((1 - f) * (p->h - 1)), 0, p->h - 1);
}
static float db_at(ls_audio_layers *p, float hz, bool hold, bool slice) {
  unsigned n = ls_audio_analysis_bins(p->analysis), rate = p->rate;
  ls_aa_config c = ls_audio_analysis_config(p->analysis);
  if (slice)
    rate = ls_audio_overview_rate(p->source);
  unsigned bin = rate ? (unsigned)(hz * c.fft_size / rate) : n;
  return slice ? ls_audio_overview_slice_db(p->source, bin)
               : ls_audio_analysis_bin_db(p->analysis, bin, hold) - c.gain_db;
}
static void text(tui_surface *sf, tui_rect r, int row, const char *s,
                 uint8_t fg) {
  tui_put_str(sf, r, r.x, r.y + row, s, TUI_ATTR(fg, TUI_BLACK));
}
static void show_image(ls_audio_layers *p, tui_surface *sf) {
  ls_tui_image(p->plot, p->pixels, p->w, p->h, ++p->serial);
  tui_fill(sf, p->plot, LS_TUI_IMAGE_CELL, TUI_ATTR(TUI_WHITE, TUI_BLACK));
}
static void waveform(ls_audio_layers *p, bool cached) {
  int mid = p->h / 2;
  line(p, 0, mid, p->w - 1, mid, 0x18e3);
  for (int x = 0; x < p->w; x++) {
    if (cached) {
      uint64_t ms = p->start + (uint64_t)x * p->span / p->w;
      unsigned b = (unsigned)(ms * LS_OV_WAVE / p->duration);
      int16_t low = 0, high = 0;
      unsigned end =
          (unsigned)((p->start + (uint64_t)(x + 1) * p->span / p->w) *
                     LS_OV_WAVE / p->duration);
      if (end >= LS_OV_WAVE)
        end = LS_OV_WAVE - 1;
      for (; b <= end && b < LS_OV_WAVE; b++) {
        int16_t lo, hi;
        if (ls_audio_overview_wave(p->source, b, &lo, &hi)) {
          if (lo < low)
            low = lo;
          if (hi > high)
            high = hi;
        }
      }
      line(p, x, mid - (int)((float)high * mid / 32768), x,
           mid - (int)((float)low * mid / 32768), 0x07ff);
    } else {
      float v =
          ls_audio_analysis_wave(p->analysis, (unsigned)x, (unsigned)p->w);
      dot(p, x, mid - (int)(v * (mid - 1)), 0x07ff);
    }
  }
}
static void cached_spectro(ls_audio_layers *p, const ls_aa_config *c) {
  float top = fminf(20000, ls_audio_overview_rate(p->source) * .5f);
  if (top <= 40)
    return;
  for (int y = 0; y < p->h; y++) {
    float hz = frequency(p, 1 - (float)y / (p->h - 1));
    unsigned b = (unsigned)clamp(
        (int)(logf(fmaxf(40, hz) / 40) / logf(top / 40) * (LS_OV_BINS - 1)), 0,
        LS_OV_BINS - 1);
    for (int x = 0; x < p->w; x++) {
      uint64_t ms = p->start + (uint64_t)x * p->span / p->w;
      unsigned col = (unsigned)(ms * LS_OV_COLUMNS / p->duration);
      float db =
          ls_audio_overview_spectro(p->source, col, b) * 120.0f / 255 - 120;
      p->pixels[(size_t)y * p->w + x] =
          colour((db + c->gain_db - c->floor_db) / c->range_db);
    }
  }
}
static void live_spectro(ls_audio_layers *p, tui_surface *sf,
                         const ls_aa_config *c, const ls_aa_result *v,
                         bool hold) {
  int bins = (int)ls_audio_analysis_bins(p->analysis);
  if (bins < 2 || !ls_pwf_size(&p->wf, LS_PWF_LEFT, p->w, p->h, bins))
    return;
  static const ls_pwf_stop_t stops[] = {{0, 0, 0, 0},
                                        {64, 0, 50, 140},
                                        {150, 0, 230, 220},
                                        {220, 255, 180, 30},
                                        {255, 255, 255, 255}};
  ls_pwf_colours(&p->wf, stops, 5, 0, 255);
  for (int y = 0; y < p->h; y++) {
    float hz = frequency(p, 1 - (float)y / (p->h - 1));
    int b = clamp(p->rate ? (int)(hz * c->fft_size / p->rate) : 0, 0, bins - 1);
    if (p->wf.first[y] != b || p->wf.last[y] != b) {
      p->wf.first[y] = p->wf.last[y] = (int16_t)b;
      p->wf.redraw = true;
    }
  }
  if (v && !hold && v->frames != p->last_frames) {
    uint8_t *codes = ls_pwf_push(&p->wf);
    for (int b = 0; b < bins; b++)
      codes[b] = (uint8_t)level(
          (ls_audio_analysis_bin_db(p->analysis, b, false) - c->floor_db) /
              c->range_db,
          255);
    p->last_frames = v->frames;
  }
  ls_pwf_mark(&p->wf, freq_y(p, p->hz), 0xffff);
  ls_pwf_compose(&p->wf, 0, 0);
  ls_pwf_show(&p->wf, sf, p->plot);
}
static void scope(ls_audio_layers *p) {
  int mx = p->w / 2, my = p->h / 2;
  line(p, 0, my, p->w - 1, my, 0x18e3);
  line(p, mx, 0, mx, p->h - 1, 0x18e3);
  for (unsigned i = 0; i < 128; i++) {
    float l, r;
    if (ls_audio_analysis_scope(p->analysis, i, &l, &r)) {
      int x = mx + (int)((l - r) * .5f * (mx - 1)),
          y = my - (int)((l + r) * .5f * (my - 1));
      dot(p, x, y, 0x07ff);
    }
  }
}
static void tools_view(ls_audio_layers *p, tui_surface *sf, tui_rect r,
                       ls_audio_tools *t, const ls_aa_result *v) {
  char *s = p->text;
  if (v && v->pitch_hz > 0) {
    float midi = 69 + 12 * log2f(v->pitch_hz / 440);
    int note = (int)lroundf(midi);
    LS_SNPRINTF(t_ls_audio_layers_tuner_1f_hz_s, s, sizeof(p->text), "TUNER %.1f Hz  %s%d  %+.1f cents",
             (double)v->pitch_hz, notes[(note % 12 + 12) % 12], note / 12 - 1,
             (double)((midi - note) * 100));
  } else
    LS_SNPUTS(t_ls_audio_layers_tuner_waiting_for_a, s, sizeof(p->text), "TUNER: waiting for a clear single note");
  text(sf, r, 0, s, TUI_CYAN | TUI_BRIGHT);
  if (t)
    ls_audio_tools_snapshot(t, &p->status, p->events, 12, &p->count);
  LS_SNPRINTF(t_ls_audio_layers_cw_1f_wpm_s, s, sizeof(p->text), "CW %.1f WPM  %s  DTMF %s",
           (double)p->status.wpm, p->status.subtone, p->status.digits);
  text(sf, r, 1, s, TUI_YELLOW);
  text(sf, r, 2, LS_TEXT(t_ls_audio_layers_options_generator_sine_square, "OPTIONS > GENERATOR: sine / square / sweep"), TUI_WHITE);
  if (!t)
    text(sf, r, 4, LS_TEXT(t_ls_audio_layers_decoder_worker_unavailable, "Decoder worker unavailable"), TUI_YELLOW);
  else if (!p->count)
    text(sf, r, 4, LS_TEXT(t_ls_audio_layers_listening_for_dtmf_ctcss, "Listening for DTMF / CTCSS / DCS / CW / APRS"), TUI_WHITE);
  static const char *const kind[] = {"DTMF", "CTCSS", "DCS", "CW", "APRS"};
  int row = 4;
  for (unsigned i = 0; i < p->count && row < r.h; i++) {
    ls_tool_event *e = &p->events[i];
    LS_SNPRINTF(t_ls_audio_layers_02u_02u_03u_s, s, sizeof(p->text), "%02u:%02u.%03u %s %s",
             (unsigned)(e->time_ms / 60000), (unsigned)(e->time_ms / 1000 % 60),
             (unsigned)(e->time_ms % 1000), kind[e->kind], e->text);
    size_t len = strlen(s);
    int width = r.w > 0 ? r.w : 1;
    for (size_t off = 0; off < len && row < r.h; off += (size_t)width) {
      char *chunk = p->chunk;
      snprintf(chunk, sizeof(p->chunk), "%.*s", width, s + off);
      text(sf, r, row++, chunk, TUI_WHITE);
    }
  }
}
void ls_audio_layers_draw(ls_audio_layers *p, tui_surface *sf, tui_rect r,
                          int layer, ls_audio_analysis *a, ls_audio_overview *o,
                          ls_audio_tools *tools, uint32_t position, bool hold) {
  if (!p || !a || r.w < 4 || r.h < 8) {
    text(sf, r, 0, LS_TEXT(t_ls_audio_layers_analysis_unavailable, "Analysis unavailable"), TUI_YELLOW);
    return;
  }
  bool changed = p->analysis != a || p->source != o || p->layer != layer;
  p->analysis = a;
  p->source = o;
  p->layer = layer;
  ls_aa_config c = ls_audio_analysis_config(a);
  uint32_t reader = 0, epoch = 0;

  unsigned rate = 0;
  ls_audio_analysis_read(a, &reader, &epoch, p->reader, 1, &rate);
  if (p->epoch != epoch) {
    p->pinned = false;
    p->count = 0;
    memset(&p->status, 0, sizeof(p->status));
    changed = true;
    p->epoch = epoch;
  }
  p->rate = rate;
  bool config_changed =
      p->fft_size != c.fft_size || p->window != (unsigned)c.window;
  p->fft_size = c.fft_size;
  p->window = c.window;
  if (p->pinned && config_changed)
    ls_audio_overview_slice_request(o, p->time, c.fft_size, c.window);
  if (!hold)
    ls_audio_analysis_process(a, 4);
  const ls_aa_result *v = ls_audio_analysis_result(a);
  p->duration = ls_audio_overview_duration_ms(o);
  bool cached = ls_audio_overview_ready(o) && p->duration;
  if (!cached)
    p->duration = 0;
  if (!p->pinned)
    p->time = position;
  if (p->time > p->duration && cached)
    p->time = p->duration;
  p->lo = layer == LS_LAYER_MELODIC ? 40 : fmaxf(1, c.lo_hz);
  p->hi = layer == LS_LAYER_MELODIC ? 1600 : c.hi_hz;
  unsigned source_rate = ls_audio_overview_rate(o);
  if (!source_rate)
    source_rate = p->rate;
  if (source_rate && p->hi > source_rate * .5f)
    p->hi = source_rate * .5f;
  if (p->hi <= p->lo)
    p->hi = p->lo + 1;
  if (p->hz < p->lo || p->hz > p->hi)
    p->hz = v && v->peak_hz >= p->lo && v->peak_hz<=p->hi ? v->peak_hz : p->lo;
  p->span = cached ? p->duration / (1u << p->zoom) : 0;
  if (cached && !p->span)
    p->span = 1;
  p->start = cached && p->time > p->span / 2 ? p->time - p->span / 2 : 0;
  if (cached && p->start + p->span > p->duration)
    p->start = p->duration - p->span;
  char *s = p->text;
  float db = db_at(p, p->hz, false, false);
  uint32_t slice_ms = 0;
  unsigned slice_bins = 0;
  bool slice = p->pinned &&
               ls_audio_overview_slice_ready(o, &slice_ms, &slice_bins) &&
               slice_ms == p->time && slice_bins == c.fft_size / 2 + 1;
  if (slice)
    db = db_at(p, p->hz, false, true);
  int note = (int)lroundf(69 + 12 * log2f(p->hz / 440));
  float cents = 1200 * log2f(p->hz / (440 * powf(2, (note - 69) / 12.0f)));
  LS_SNPRINTF(t_ls_audio_layers_02u_02u_03u_1f, s, sizeof(p->text), "%02u:%02u.%03u  %.1f Hz",
           (unsigned)(p->time / 60000), (unsigned)(p->time / 1000 % 60),
           (unsigned)(p->time % 1000), (double)p->hz);
  text(sf, r, 0, s, TUI_CYAN | TUI_BRIGHT);
  LS_SNPRINTF(t_ls_audio_layers_s_d_0fc_1f, s, sizeof(p->text), "%s%d %+.0fc  %.1f dBFS %s",
           notes[(note % 12 + 12) % 12], note / 12 - 1, (double)cents,
           (double)db, slice ? "SLICE" : "LIVE");
  text(sf, r, 1, s, TUI_CYAN);
  if (layer == LS_LAYER_TOOLS) {
    p->plot = tui_rect_make(0, 0, 0, 0);
    p->overview = p->plot;
    ls_pwf_release(&p->wf);
    tools_view(p, sf, tui_rect_make(r.x, r.y + 3, r.w, r.h - 3), tools, v);
    return;
  }
  p->plot = tui_rect_make(r.x, r.y + 3, r.w, r.h - 7);
  if (layer == LS_LAYER_SPECTRUM && p->plot.h > 4) {
    float chroma_top=0;
    if(v)for(int i=0;i<12;i++)if(v->chroma[i]>chroma_top)chroma_top=v->chroma[i];
    for (int i = 0; i < 12 && i * 3 < r.w; i++) {
      tui_put_str(sf, r, r.x + i * 3, p->plot.y, notes[i],
                  TUI_ATTR(TUI_YELLOW, TUI_BLACK));
      tui_put_char(sf, r, r.x + i * 3, p->plot.y + 1,
                   LS_TUI_TRACE(v && chroma_top>0 ? level(v->chroma[i]/chroma_top, 7) + 1 : 1),
                   TUI_ATTR(TUI_YELLOW, TUI_BLACK));
    }
    p->plot.y += 2;
    p->plot.h -= 2;
  }
  ls_tui_geometry(NULL, NULL, &p->cw, &p->ch);
  if (p->cw < 1)
    p->cw = 8;
  if (p->ch < 1)
    p->ch = 16;
  p->w = p->plot.w * p->cw;
  p->h = p->plot.h * p->ch;
  size_t need = (size_t)p->w * p->h;
  if (need > p->capacity) {
    uint16_t *mem = psalloc(need * 2);
    if (!mem) {
      text(sf, r, 2, "PSRAM unavailable", TUI_YELLOW);
      return;
    }
    free(p->pixels);
    p->pixels = mem;
    p->capacity = need;
  }
  memset(p->pixels, 0, need * 2);
  if (changed) {
    ls_pwf_clear(&p->wf);
    p->last_frames = 0;
  }
  if (layer != LS_LAYER_SPECTRO && layer != LS_LAYER_MELODIC)
    ls_pwf_release(&p->wf);
  if (layer == LS_LAYER_WAVE)
    waveform(p, cached);
  else if (layer == LS_LAYER_SPECTRUM) {
    int previous = p->h - 1, peak_previous = previous;
    for (int x = 0; x < p->w; x++) {
      float hz = frequency(p, (float)x / (p->w - 1));
      float dbv = db_at(p, hz, false, slice), pk = db_at(p, hz, true, false);
      int y = p->h - 1 -
              level((dbv + c.gain_db - c.floor_db) / c.range_db, p->h - 1);
      int py = p->h - 1 -
               level((pk + c.gain_db - c.floor_db) / c.range_db, p->h - 1);
      if (x) {
        if (!slice)
          line(p, x - 1, peak_previous, x, py, 0xffe0);
        line(p, x - 1, previous, x, y, 0x07ff);
      }
      previous = y;
      peak_previous = py;
    }
  } else if (layer == LS_LAYER_SCOPE)
    scope(p);
  else if (cached)
    cached_spectro(p, &c);
  if ((layer == LS_LAYER_SPECTRO || layer == LS_LAYER_MELODIC) && !cached)
    live_spectro(p, sf, &c, v, hold);
  else {
    if (cached && layer != LS_LAYER_SCOPE && layer != LS_LAYER_SPECTRUM) {
      int x = (int)((uint64_t)(p->time - p->start) * (p->w - 1) / p->span);
      line(p, x, 0, x, p->h - 1, 0xffff);
    }
    if (layer == LS_LAYER_SPECTRO || layer == LS_LAYER_MELODIC) {
      int y = freq_y(p, p->hz);
      line(p, 0, y, p->w - 1, y, 0xffff);
    }
    show_image(p, sf);
  }
  if (layer == LS_LAYER_MELODIC) {
    for (int midi = 36; midi <= 90; midi += 12) {
      float hz = 440 * powf(2, (midi - 69) / 12.0f);
      if (hz < p->lo || hz > p->hi)
        continue;
      char *tick = p->tick;
      snprintf(tick, sizeof(p->tick), "C%d", midi / 12 - 1);
      text(sf, p->plot, freq_y(p, hz) / p->ch, tick, TUI_WHITE);
    }
    /* YIN history follows the live time axis. Cached files retain source
     * detail. */
    if (!cached) {
      unsigned n = ls_audio_analysis_pitch_track(a, p->tracks, 256);
      for (unsigned i = 0; i < n; i++)
        if (p->tracks[i].hz >= p->lo && p->tracks[i].hz <= p->hi) {
          int age = (int)((v->time_ms - p->tracks[i].time_ms) * source_rate /
                          (1000u * c.hop));
          int x = p->plot.x + p->plot.w - 1 - age / p->cw;
          if (x >= p->plot.x)
            tui_put_char(sf, p->plot, x,
                         p->plot.y + freq_y(p, p->tracks[i].hz) / p->ch, '*',
                         TUI_ATTR(TUI_YELLOW, TUI_BLACK));
        }
    }
  }
  if ((layer == LS_LAYER_MELODIC || layer == LS_LAYER_SPECTRO) && !cached &&
      v) {
    unsigned n = ls_audio_analysis_track(a, p->tracks, 256);
    for (unsigned i = 0; i < n; i++)
      if (p->tracks[i].hz >= p->lo && p->tracks[i].hz <= p->hi) {
        int age = (int)((uint64_t)(v->time_ms - p->tracks[i].time_ms) *
                        source_rate / (1000u * c.hop));
        int x = p->plot.x + p->plot.w - 1 - age / p->cw;
        if (x >= p->plot.x)
          tui_put_char(sf, p->plot, x,
                       p->plot.y + freq_y(p, p->tracks[i].hz) / p->ch, '.',
                       TUI_ATTR(TUI_WHITE, TUI_BLACK));
      }
  }
  p->overview = tui_rect_make(r.x, r.y + r.h - 3, r.w, 1);
  for (int x = 0; x < r.w; x++) {
    int16_t lo = 0, hi = 0;
    bool got =
        cached && ls_audio_overview_wave(
                      o, (unsigned)((uint64_t)x * LS_OV_WAVE / r.w), &lo, &hi);
    int level = (int)((hi - lo) * 7.0f / 65535);
    tui_put_char(sf, p->overview, r.x + x, p->overview.y,
                 got ? LS_TUI_TRACE(level + 1) : ' ',
                 TUI_ATTR(TUI_CYAN, TUI_BLUE));
  }
  if (cached)
    tui_put_char(sf, p->overview,
                 r.x + (int)((uint64_t)p->time * (r.w - 1) / p->duration),
                 p->overview.y, '|',
                 TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLUE));
  if (v) {
    LS_SNPRINTF(t_ls_audio_layers_rms_1f_crest_1f, s, sizeof(p->text), "RMS %.1f  crest %.1f  peak~ %.1f dBFS",
             (double)v->rms_db, (double)v->crest_db,
             (double)v->estimated_peak_db);
    text(sf, r, r.h - 2, s, TUI_WHITE);
    LS_SNPRINTF(t_ls_audio_layers_lufs_m_1f_s, s, sizeof(p->text),
             "LUFS M %.1f S %.1f  corr %+.2f  %.0f BPM  key %s%s",
             (double)v->momentary_lufs, (double)v->short_lufs,
             (double)v->correlation, (double)v->bpm,
             v->key_root >= 0 ? notes[v->key_root % 12] : "?",
             v->key_minor ? "m" : "");
    text(sf, r, r.h - 1, s, TUI_YELLOW);
  }
  if (!cached) {
    LS_SNPRINTF(t_ls_audio_layers_live_overview_u_s, s, sizeof(p->text), "LIVE  overview %u%% %s",
             ls_audio_overview_progress(o), ls_audio_overview_error(o));
    text(sf, r, 2, s, TUI_WHITE);
  } else {
    LS_SNPRINTF(t_ls_audio_layers_file_ux_tap_cursor, s, sizeof(p->text), "FILE %ux  tap cursor  Z zoom  O seek",
             1u << p->zoom);
    text(sf, r, 2, s, TUI_WHITE);
  }
}
LS_COLD bool ls_audio_layers_touch(ls_audio_layers *p, int x, int y) {
  if (!p || p->layer == LS_LAYER_TOOLS)
    return false;
  bool overview = tui_rect_contains(p->overview, x, y);
  if (!overview && !tui_rect_contains(p->plot, x, y))
    return false;
  p->pinned = true;
  if (p->duration) {
    unsigned offset = (unsigned)(x - p->plot.x),
             width = (unsigned)(p->plot.w > 1 ? p->plot.w - 1 : 1);
    p->time = overview
                  ? (uint32_t)((uint64_t)offset * p->duration / width)
                  : p->start + (uint32_t)((uint64_t)offset * p->span / width);
    if (p->time > p->duration)
      p->time = p->duration;
    ls_aa_config c = ls_audio_analysis_config(p->analysis);
    ls_audio_overview_slice_request(p->source, p->time, c.fft_size, c.window);
  }
  if (!overview) {
    float f = p->layer == LS_LAYER_SPECTRUM
                  ? (float)(x - p->plot.x) / (p->plot.w - 1)
                  : 1 - (float)(y - p->plot.y) / (p->plot.h > 1 ? p->plot.h - 1 : 1);
    p->hz = frequency(p, f);
  }
  return true;
}
void ls_audio_layers_zoom(ls_audio_layers *p) {
  if (p)
    p->zoom = (p->zoom + 1) % 6;
}
uint32_t ls_audio_layers_time(const ls_audio_layers *p) {
  return p ? p->time : 0;
}
float ls_audio_layers_frequency(const ls_audio_layers *p) {
  return p ? p->hz : 0;
}
