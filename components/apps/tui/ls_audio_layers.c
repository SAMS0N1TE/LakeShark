#include "ls_audio_layers.h"
#include "ls_pixel_wf.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_cold.h"
#include "ls_textpack.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

struct ls_audio_layers {
  ls_pwf_t wf;
  uint16_t *pixels;
  size_t capacity;
  int w, h, layer, zoom, cw, ch;
  /* MELODIC's lowest note (MIDI), and the axis the live SPECTRO was last
     given, so a new one redraws the history once. */
  int melodic_low;
  float axis_lo, axis_hi;
  int axis_rows, axis_bins;
  bool axis_log;
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
  /* The picture is handed to the present at most every
     LS_LAYER_PICTURE_US: each new one costs the P4 a compare and a copy of
     the whole image (13-16 ms measured), so a picture every 40 ms frame
     left nothing for anything else. In between, the text rows still follow
     every frame and the same picture stays up. */
  int64_t picture_at;
  bool dirty;                 /* a tap, zoom or hold: draw now */
  int drawn_layer, lit;
  tui_rect drawn_plot;
  uint32_t drawn_sig;         /* what the last picture showed */
  /* SPECTRO over the whole track: the bin each pixel row reads, the colour
     of each overview code, and whether `pixels` holds that picture (less
     the playhead column and pinned row, put back from the tables). */
  int16_t *row_bin;
  int row_cap;
  uint16_t lut[256];
  uint32_t table_sig, base_sig;
  bool base_valid;
  int base_x, base_y;
};
/* Ten pictures a second: a trace still moves, and the present's copies
   drop from every frame to one in four at the board's 25 fps. */
#define LS_LAYER_PICTURE_US 100000
static uint32_t mix(uint32_t h, uint32_t v) {
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h;
}
static uint32_t mixf(uint32_t h, float f) {
  uint32_t v;
  memcpy(&v, &f, sizeof(v));
  return mix(h, v);
}
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
  free(p->row_bin);
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
/* One colour map for the live and the whole-track spectrogram: quiet is
   black, then violet, red, orange, yellow and white at the top, so a tone
   stands out of the noise instead of everything reading blue. */
static const ls_pwf_stop_t SPECTRO_STOPS[] = {{0, 0, 0, 0},     {60, 30, 0, 70},
                                              {120, 140, 0, 120}, {175, 235, 60, 20},
                                              {225, 255, 190, 0}, {255, 255, 255, 220}};
#define SPECTRO_STOP_N (int)(sizeof(SPECTRO_STOPS) / sizeof(SPECTRO_STOPS[0]))
static uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
  return (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}
static uint16_t colour(float v) {
  int at = (int)(fminf(1, fmaxf(0, v)) * 255);
  int i = 0;
  while (i + 2 < SPECTRO_STOP_N && at > SPECTRO_STOPS[i + 1].at)
    i++;
  const ls_pwf_stop_t *a = &SPECTRO_STOPS[i], *b = &SPECTRO_STOPS[i + 1];
  int span = b->at - a->at, t = span > 0 ? (at - a->at) * 256 / span : 0;
  if (t < 0)
    t = 0;
  if (t > 256)
    t = 256;
  return rgb565((unsigned)(a->r + ((b->r - a->r) * t >> 8)),
                (unsigned)(a->g + ((b->g - a->g) * t >> 8)),
                (unsigned)(a->b + ((b->b - a->b) * t >> 8)));
}
static float midi_hz(float midi) { return 440 * powf(2, (midi - 69) / 12.0f); }
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
/* SPECTRO over the whole track. It used to work out every pixel from
   scratch every frame - two 64-bit divides, a logarithm's worth of axis and a
   colour-map search each - which took the P4 160 ms a frame: the player ran
   at five frames a second and read as locked up. Now the axis is a table of
   bins per pixel row and the colour map a table per overview code, both
   rebuilt only when the axis or reading changes; the picture is drawn whole
   only when what it shows moves (zoom, a seek past the window, a new
   reading), and otherwise only the playhead column and pinned row are put
   back from the tables and drawn again. */
static bool spectro_tables(ls_audio_layers *p, const ls_aa_config *c) {
  float top = fminf(20000, ls_audio_overview_rate(p->source) * .5f);
  if (top <= 40)
    return false;
  uint32_t sig = mix(mix(mix(0, (uint32_t)p->h), (uint32_t)c->log_axis), (uint32_t)p->layer);
  sig = mixf(mixf(mixf(mixf(mixf(mixf(sig, p->lo), p->hi), top), c->gain_db), c->floor_db), c->range_db);
  if (p->row_bin && p->row_cap >= p->h && sig == p->table_sig)
    return true;
  if (p->row_cap < p->h) {
    int16_t *rows = psalloc((size_t)p->h * sizeof(int16_t));
    if (!rows)
      return false;
    free(p->row_bin);
    p->row_bin = rows;
    p->row_cap = p->h;
  }
  const float span = logf(top / 40);
  for (int y = 0; y < p->h; y++) {
    float hz = frequency(p, 1 - (float)y / (p->h > 1 ? p->h - 1 : 1));
    p->row_bin[y] = (int16_t)clamp((int)(logf(fmaxf(40, hz) / 40) / span * (LS_OV_BINS - 1)),
                                   0, LS_OV_BINS - 1);
  }
  for (int v = 0; v < 256; v++) {
    float db = v * 120.0f / 255 - 120;
    p->lut[v] = colour((db + c->gain_db - c->floor_db) / c->range_db);
  }
  p->table_sig = sig;
  p->base_valid = false;
  return true;
}
static unsigned spectro_column(const ls_audio_layers *p, int x) {
  uint64_t ms = p->start + (uint64_t)x * p->span / p->w;
  return (unsigned)(ms * LS_OV_COLUMNS / p->duration);
}
/* Columns x0..x1-1 of the picture from the tables: one overview column read
   per distinct column, then a table lookup per pixel. */
static void spectro_columns(ls_audio_layers *p, int x0, int x1) {
  uint8_t codes[LS_OV_BINS];
  unsigned last = UINT32_MAX;
  for (int x = x0 < 0 ? 0 : x0; x < x1 && x < p->w; x++) {
    unsigned col = spectro_column(p, x);
    if (col != last) {
      for (unsigned b = 0; b < LS_OV_BINS; b++)
        codes[b] = ls_audio_overview_spectro(p->source, col, b);
      last = col;
    }
    uint16_t *px = p->pixels + x;
    for (int y = 0; y < p->h; y++, px += p->w)
      *px = p->lut[codes[p->row_bin[y]]];
  }
}
static void spectro_row(ls_audio_layers *p, int y) {
  if (y < 0 || y >= p->h)
    return;
  uint16_t *row = p->pixels + (size_t)y * p->w;
  const unsigned bin = (unsigned)p->row_bin[y];
  for (int x = 0; x < p->w; x++)
    row[x] = p->lut[ls_audio_overview_spectro(p->source, spectro_column(p, x), bin)];
}
static void cached_spectro(ls_audio_layers *p, const ls_aa_config *c) {
  if (!spectro_tables(p, c)) {
    memset(p->pixels, 0, (size_t)p->w * p->h * 2);
    p->base_valid = false;
    return;
  }
  uint32_t sig = mix(mix(mix(mix(p->table_sig, p->start), p->span), (uint32_t)p->w),
                     (uint32_t)(uintptr_t)p->source);
  if (p->base_valid && sig == p->base_sig) {
    /* The same picture: only what was drawn over it goes back. */
    if (p->base_x >= 0)
      spectro_columns(p, p->base_x, p->base_x + 1);
    spectro_row(p, p->base_y);
  } else
    spectro_columns(p, 0, p->w);
  p->base_sig = sig;
  p->base_valid = true;
  p->base_x = p->base_y = -1;
}
static void live_spectro(ls_audio_layers *p, tui_surface *sf,
                         const ls_aa_config *c, const ls_aa_result *v,
                         bool hold) {
  int bins = (int)ls_audio_analysis_bins(p->analysis);
  if (bins < 3 || !p->rate || !ls_pwf_size(&p->wf, LS_PWF_LEFT, p->w, p->h, bins))
    return;
  ls_pwf_colours(&p->wf, SPECTRO_STOPS, SPECTRO_STOP_N, 0, 255);
  /* Each pixel row reads the bins its band of frequencies covers: blended
     between the two nearest where a bin is taller than a pixel (the low end
     of a log axis), the loudest of them where it is shorter. Rebuilt only
     when the axis moves. */
  if (p->axis_lo != p->lo || p->axis_hi != p->hi || p->axis_rows != p->h ||
      p->axis_bins != bins || p->axis_log != c->log_axis || !p->wf.lerp) {
    const float per_bin = (float)c->fft_size / p->rate;
    for (int y = 0; y < p->h; y++) {
      float top = frequency(p, 1 - ((float)y - .5f) / (p->h - 1)),
            bottom = frequency(p, 1 - ((float)y + .5f) / (p->h - 1)),
            centre = frequency(p, 1 - (float)y / (p->h - 1));
      float b0 = bottom * per_bin, b1 = top * per_bin;
      if (b1 - b0 < 1.0f) {
        float at = fmaxf(0, centre * per_bin);
        int a = clamp((int)at, 0, bins - 2);
        p->wf.first[y] = (int16_t)a;
        p->wf.last[y] = (int16_t)clamp((int)((at - a) * 256), 0, 256);
      } else {
        int a = clamp((int)b0, 0, bins - 1), z = clamp((int)b1, a, bins - 1);
        p->wf.first[y] = (int16_t)a;
        p->wf.last[y] = (int16_t)(-(z + 1));
      }
    }
    p->wf.lerp = true;
    ls_pwf_axis_changed(&p->wf);
    p->axis_lo = p->lo;
    p->axis_hi = p->hi;
    p->axis_rows = p->h;
    p->axis_bins = bins;
    p->axis_log = c->log_axis;
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
  /* The inspected frequency only while a tap holds it: a marker left at
     whatever the loudest bin was on entry reads as part of the signal. */
  ls_pwf_mark(&p->wf, p->pinned ? freq_y(p, p->hz) : -1, 0xffff);
  ls_pwf_compose(&p->wf, 0, 0);
  ls_pwf_show(&p->wf, sf, p->plot);
}
/* Frequency labels in the cells left of the picture, never over it: a
   label cell over image cells blanks the picture behind it. */
static void freq_gutter(ls_audio_layers *p, tui_surface *sf, tui_rect g) {
  if (g.w < 3)
    return;
  static const float log_ticks[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
  float ticks[24];
  int n = 0;
  ls_aa_config c = ls_audio_analysis_config(p->analysis);
  if (c.log_axis) {
    for (unsigned i = 0; i < sizeof(log_ticks) / sizeof(log_ticks[0]); i++)
      ticks[n++] = log_ticks[i];
  } else {
    float span = p->hi - p->lo, step = span > 12000 ? 2000 : span > 6000 ? 1000 : span > 2500 ? 500 : 200;
    for (float f = ceilf(p->lo / step) * step; f <= p->hi && n < 24; f += step)
      ticks[n++] = f;
  }
  int last_row = -2;
  for (int i = n - 1; i >= 0; i--) {
    if (ticks[i] < p->lo || ticks[i] > p->hi)
      continue;
    int row = g.y + freq_y(p, ticks[i]) / p->ch;
    if (row - last_row < 2 || row >= g.y + g.h)
      continue;
    char k[8];
    if (ticks[i] >= 1000)
      snprintf(k, sizeof(k), fmodf(ticks[i], 1000) ? "%.1fk" : "%.0fk", (double)(ticks[i] / 1000));
    else
      snprintf(k, sizeof(k), "%.0f", (double)ticks[i]);
    int len = (int)strlen(k);
    tui_put_str(sf, g, g.x + g.w - 1 - len, row, k, TUI_ATTR(TUI_CYAN, TUI_BLACK));
    tui_put_char(sf, g, g.x + g.w - 1, row, '-', TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    last_row = row;
  }
}
/* MELODIC: a piano roll. Three octaves that follow the tune, a keyboard
   down the left edge with the sounding key lit, a row per semitone (the
   black keys darker, each C ruled) and the detected pitch as a line, the
   newest at the right. Drawn whole each frame from the analysis' pitch
   history; nothing here reads the FFT, which is too coarse at bass notes. */
#define MELODIC_SPAN 36
static bool black_key(int midi) {
  int k = (midi % 12 + 12) % 12;
  return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
}
static int melodic(ls_audio_layers *p, const ls_aa_result *v, const ls_aa_config *c,
                   unsigned source_rate) {
  unsigned n = ls_audio_analysis_pitch_track(p->analysis, p->tracks, 256);
  /* Follow the tune: recentre on the recent notes when they leave the
     middle of the range, in whole octaves so the rows stay put. */
  int lo_note = 999, hi_note = -999;
  for (unsigned i = n > 96 ? n - 96 : 0; i < n; i++)
    if (p->tracks[i].hz >= 30 && p->tracks[i].hz <= 4200) {
      int m = (int)lroundf(69 + 12 * log2f(p->tracks[i].hz / 440));
      if (m < lo_note)
        lo_note = m;
      if (m > hi_note)
        hi_note = m;
    }
  if (!p->melodic_low)
    p->melodic_low = 36;
  if (hi_note >= lo_note &&
      (lo_note < p->melodic_low + 2 || hi_note > p->melodic_low + MELODIC_SPAN - 2)) {
    int mid = (lo_note + hi_note) / 2;
    p->melodic_low = clamp((mid - MELODIC_SPAN / 2 + 6) / 12 * 12, 24, 96);
  }
  p->lo = midi_hz(p->melodic_low - .5f);
  p->hi = midi_hz(p->melodic_low + MELODIC_SPAN - .5f);
  const int key_w = p->w >= 120 ? 18 : 12;
  /* The rows: semitone bands, black keys darker, a rule under each C. */
  for (int m = p->melodic_low; m < p->melodic_low + MELODIC_SPAN; m++) {
    int y0 = freq_y(p, midi_hz(m + .5f)), y1 = freq_y(p, midi_hz(m - .5f));
    uint16_t band = black_key(m) ? 0x0841 : 0x10a3;
    for (int y = y0; y <= y1 && y < p->h; y++) {
      uint16_t *row = p->pixels + (size_t)y * p->w;
      uint16_t key = black_key(m) ? 0x2104 : 0xbdf7;
      for (int x = 0; x < key_w; x++)
        row[x] = black_key(m) ? (x < key_w * 2 / 3 ? key : 0xbdf7) : key;
      for (int x = key_w; x < p->w; x++)
        row[x] = band;
    }
    if ((m % 12 + 12) % 12 == 0)
      line(p, key_w, y1, p->w - 1, y1, 0x4a69);
    line(p, 0, y1, key_w - 1, y1, 0x4208);
  }
  /* A rule every second, moving with the picture. */
  const float hop_ms = source_rate ? c->hop * 1000.0f / source_rate : 11.6f;
  const float ms_per_px = hop_ms * 256 / (float)(p->w - key_w);
  const uint32_t now_ms = v ? v->time_ms : 0;
  for (uint32_t s = now_ms / 1000 * 1000;; s -= 1000) {
    int x = p->w - 1 - (int)((now_ms - s) / ms_per_px);
    if (x < key_w)
      break;
    line(p, x, 0, x, p->h - 1, 0x2945);
    if (s < 1000)
      break;
  }
  /* The tune: a thick line through the voiced points, broken across
     silence and jumps, the newest point a bright block. */
  int px = -1, py = -1, lit = -1;
  for (unsigned i = 0; i < n; i++) {
    const ls_aa_track *t = &p->tracks[i];
    int x = p->w - 1 - (int)((now_ms - t->time_ms) / ms_per_px);
    bool voiced = t->hz >= p->lo && t->hz <= p->hi;
    if (!voiced || x < key_w) {
      px = -1;
      continue;
    }
    int y = freq_y(p, t->hz);
    uint16_t ink = 0xffe0;
    if (px >= 0 && abs(y - py) < p->h / 12)
      for (int d = -1; d <= 1; d++)
        line(p, px, py + d, x, y + d, ink);
    else
      for (int d = -1; d <= 1; d++)
        line(p, x - 1, y + d, x + 1, y + d, ink);
    px = x;
    py = y;
    /* Sounding now: the last point, and recent. */
    if (i + 1 == n && now_ms - t->time_ms < 250)
      lit = (int)lroundf(69 + 12 * log2f(t->hz / 440));
  }
  if (px >= 0 && lit >= 0)
    for (int d = -3; d <= 3; d++)
      line(p, px - 3, py + d, px, py + d, 0xffff);
  /* The sounding key on the keyboard. */
  if (lit >= p->melodic_low && lit < p->melodic_low + MELODIC_SPAN) {
    int y0 = freq_y(p, midi_hz(lit + .5f)), y1 = freq_y(p, midi_hz(lit - .5f));
    int w = black_key(lit) ? key_w * 2 / 3 : key_w;
    for (int y = y0 + 1; y < y1 && y < p->h; y++)
      for (int x = 0; x < w; x++)
        p->pixels[(size_t)y * p->w + x] = 0xfd20;
  }
  return lit;
}
/* Note names beside the roll, one per C and the sounding note. */
static void note_gutter(ls_audio_layers *p, tui_surface *sf, tui_rect g, int lit) {
  for (int m = p->melodic_low; m < p->melodic_low + MELODIC_SPAN; m += 12) {
    int row = g.y + freq_y(p, midi_hz((float)m)) / p->ch;
    char k[16];
    snprintf(k, sizeof(k), "C%d", m / 12 - 1);
    tui_put_str(sf, g, g.x + g.w - (int)strlen(k), row, k, TUI_ATTR(TUI_WHITE, TUI_BLACK));
  }
  if (lit >= p->melodic_low && lit < p->melodic_low + MELODIC_SPAN) {
    int row = g.y + freq_y(p, midi_hz((float)lit)) / p->ch;
    char k[16];
    snprintf(k, sizeof(k), "%s%d", notes[(lit % 12 + 12) % 12], lit / 12 - 1);
    tui_fill(sf, tui_rect_make(g.x, row, g.w, 1), ' ', TUI_ATTR(TUI_BLACK, TUI_YELLOW));
    tui_put_str(sf, g, g.x + g.w - (int)strlen(k), row, k,
                TUI_ATTR(TUI_BLACK, TUI_YELLOW | TUI_BRIGHT));
  }
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
  const bool roll = layer == LS_LAYER_MELODIC;
  if (roll) {
    if (!p->melodic_low)
      p->melodic_low = 36;
    p->lo = midi_hz(p->melodic_low - .5f);
    p->hi = midi_hz(p->melodic_low + MELODIC_SPAN - .5f);
  } else {
    p->lo = fmaxf(1, c.lo_hz);
    p->hi = c.hi_hz;
  }
  unsigned source_rate = ls_audio_overview_rate(o);
  if (!source_rate)
    source_rate = p->rate;
  if (!roll && source_rate && p->hi > source_rate * .5f)
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
  if (roll) {
    /* What is sounding, in the roll's own words. */
    if (v && v->pitch_hz > 0) {
      float midi = 69 + 12 * log2f(v->pitch_hz / 440);
      int n = (int)lroundf(midi);
      LS_SNPRINTF(t_ls_audio_layers_note_s_d_1f_hz_0f, s, sizeof(p->text), "NOTE %s%d  %.1f Hz  %+.0f cents",
                  notes[(n % 12 + 12) % 12], n / 12 - 1, (double)v->pitch_hz,
                  (double)((midi - n) * 100));
      text(sf, r, 0, s, TUI_YELLOW | TUI_BRIGHT);
    } else
      text(sf, r, 0, LS_TEXT(t_ls_audio_layers_note_none, "NOTE --  no clear single note"), TUI_YELLOW);
    if (v && v->key_root >= 0)
      LS_SNPRINTF(t_ls_audio_layers_key_s_s, s, sizeof(p->text), "KEY %s %s",
                  notes[v->key_root % 12], v->key_minor ? "MINOR" : "MAJOR");
    else
      LS_SNPUTS(t_ls_audio_layers_key_listening, s, sizeof(p->text), "KEY listening");
    text(sf, r, 1, s, TUI_CYAN);
  } else {
    int note = (int)lroundf(69 + 12 * log2f(p->hz / 440));
    float cents = 1200 * log2f(p->hz / (440 * powf(2, (note - 69) / 12.0f)));
    LS_SNPRINTF(t_ls_audio_layers_02u_02u_03u_1f, s, sizeof(p->text), "%02u:%02u.%03u  %.1f Hz",
             (unsigned)(p->time / 60000), (unsigned)(p->time / 1000 % 60),
             (unsigned)(p->time % 1000), (double)p->hz);
    text(sf, r, 0, s, TUI_CYAN | TUI_BRIGHT);
    LS_SNPRINTF(t_ls_audio_layers_s_d_0fc_1f, s, sizeof(p->text), "%s%d %+.0fc  %.1f dBFS %s",
             notes[(note % 12 + 12) % 12], note / 12 - 1, (double)cents,
             (double)db, slice ? "SLICE" : p->pinned ? "PINNED" : "LIVE");
    text(sf, r, 1, s, TUI_CYAN);
  }
  if (layer == LS_LAYER_TOOLS) {
    p->plot = tui_rect_make(0, 0, 0, 0);
    p->overview = p->plot;
    ls_pwf_release(&p->wf);
    tools_view(p, sf, tui_rect_make(r.x, r.y + 3, r.w, r.h - 3), tools, v);
    return;
  }
  /* SPECTRO and MELODIC keep their scale in a gutter of text cells. */
  const int gutter = layer == LS_LAYER_SPECTRO ? 5 : roll ? 4 : 0;
  p->plot = tui_rect_make(r.x + gutter, r.y + 3, r.w - gutter, r.h - 8);
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
  if (p->plot.w < 4 || p->plot.h < 2)
    return;
  ls_tui_geometry(NULL, NULL, &p->cw, &p->ch);
  if (p->cw < 1)
    p->cw = 8;
  if (p->ch < 1)
    p->ch = 16;
  p->w = p->plot.w * p->cw;
  p->h = p->plot.h * p->ch;
  size_t need = (size_t)p->w * p->h;
  bool replaced = false;
  if (need > p->capacity) {
    uint16_t *mem = psalloc(need * 2);
    if (!mem) {
      text(sf, r, 2, "PSRAM unavailable", TUI_YELLOW);
      return;
    }
    free(p->pixels);
    p->pixels = mem;
    p->capacity = need;
    replaced = true;
  }
  if (changed) {
    ls_pwf_clear(&p->wf);
    p->last_frames = 0;
  }
  const bool live_wf = layer == LS_LAYER_SPECTRO && !cached;
  if (!live_wf)
    ls_pwf_release(&p->wf);
  /* Whether to draw a new picture this frame. Over the whole track WAVE and
     SPECTRO change only when what they show moves, so they draw when that
     does; the live traces draw on the picture clock. A tap, a zoom, a new
     view or a new size draws at once. */
  const int64_t now = esp_timer_get_time();
  const bool playhead = cached && !roll && layer != LS_LAYER_SCOPE && layer != LS_LAYER_SPECTRUM;
  const int head_x = playhead ? (int)((uint64_t)(p->time - p->start) * (p->w - 1) / p->span) : -1;
  const int pin_y = layer == LS_LAYER_SPECTRO && p->pinned ? freq_y(p, p->hz) : -1;
  const bool still = cached && (layer == LS_LAYER_WAVE || layer == LS_LAYER_SPECTRO);
  uint32_t sig = 0;
  if (still) {
    sig = mix(mix(mix(mix(mix(mix(0, p->start), p->span), (uint32_t)head_x), (uint32_t)pin_y),
                  (uint32_t)layer), (uint32_t)c.log_axis);
    sig = mixf(mixf(mixf(mixf(mixf(sig, c.gain_db), c.floor_db), c.range_db), p->lo), p->hi);
  }
  const bool must = changed || replaced || p->dirty || layer != p->drawn_layer ||
                    memcmp(&p->plot, &p->drawn_plot, sizeof(p->plot)) || !p->serial;
  const bool due = now - p->picture_at >= LS_LAYER_PICTURE_US || now < p->picture_at;
  const bool fresh = must || (still ? due && sig != p->drawn_sig : due);
  if (replaced || layer != LS_LAYER_SPECTRO || !cached)
    p->base_valid = false;
  if (live_wf)
    live_spectro(p, sf, &c, v, hold);
  else if (fresh) {
    int lit = -1;
    if (!(layer == LS_LAYER_SPECTRO && cached))
      memset(p->pixels, 0, need * 2);
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
    else if (roll)
      lit = melodic(p, v, &c, source_rate);
    else if (cached)
      cached_spectro(p, &c);
    if (head_x >= 0)
      line(p, head_x, 0, head_x, p->h - 1, 0xffff);
    if (pin_y >= 0)
      line(p, 0, pin_y, p->w - 1, pin_y, 0xffff);
    if (p->base_valid) {
      p->base_x = head_x;
      p->base_y = pin_y;
    }
    p->lit = lit;
    p->serial++;
    p->picture_at = now;
    p->drawn_sig = sig;
    p->dirty = false;
  }
  p->drawn_layer = layer;
  p->drawn_plot = p->plot;
  if (!live_wf) {
    /* The same serial when nothing was drawn: the present has nothing to do. */
    ls_tui_image(p->plot, p->pixels, p->w, p->h, p->serial);
    tui_fill(sf, p->plot, LS_TUI_IMAGE_CELL, TUI_ATTR(TUI_WHITE, TUI_BLACK));
  }
  const int lit = roll ? p->lit : -1;
  if (layer == LS_LAYER_SPECTRO)
    freq_gutter(p, sf, tui_rect_make(r.x, p->plot.y, gutter, p->plot.h));
  else if (roll)
    note_gutter(p, sf, tui_rect_make(r.x, p->plot.y, gutter, p->plot.h), lit);
  /* The whole track in two rows under the plot: its loudness, then where
     playback is. A tap here moves playback (the screen does that). */
  p->overview = tui_rect_make(r.x, r.y + r.h - 4, r.w, 2);
  const uint32_t whole = cached ? p->duration : 0;
  const int knob = whole ? (int)((uint64_t)(p->time > whole ? whole : p->time) * (r.w - 1) / whole) : -1;
  for (int x = 0; x < r.w; x++) {
    int16_t lo = 0, hi = 0;
    bool got =
        cached && ls_audio_overview_wave(
                      o, (unsigned)((uint64_t)x * LS_OV_WAVE / r.w), &lo, &hi);
    int level = (int)((hi - lo) * 7.0f / 65535);
    tui_put_char(sf, p->overview, r.x + x, p->overview.y,
                 got ? LS_TUI_TRACE(level + 1) : ' ',
                 TUI_ATTR(x <= knob ? TUI_CYAN | TUI_BRIGHT : TUI_CYAN, TUI_BLUE));
    tui_put_char(sf, p->overview, r.x + x, p->overview.y + 1,
                 x == knob ? '#' : x < knob ? '=' : '-',
                 x == knob ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLUE)
                 : x < knob ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLUE)
                            : TUI_ATTR(TUI_CYAN, TUI_BLUE));
  }
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
  /* Row 2 says how to read the picture. */
  if (roll)
    text(sf, r, 2, LS_TEXT(t_ls_audio_layers_roll_legend, "PITCH OVER TIME  newest at right  1 line = 1 s"), TUI_WHITE);
  else if (layer == LS_LAYER_SPECTRO && !cached) {
    LS_SNPRINTF(t_ls_audio_layers_spectro_live_legend_u, s, sizeof(p->text), "LIVE  newest at right  bright = loud  overview %u%%",
                ls_audio_overview_progress(o));
    text(sf, r, 2, s, TUI_WHITE);
  } else if (!cached) {
    LS_SNPRINTF(t_ls_audio_layers_live_overview_u_s, s, sizeof(p->text), "LIVE  overview %u%% %s",
             ls_audio_overview_progress(o), ls_audio_overview_error(o));
    text(sf, r, 2, s, TUI_WHITE);
  } else {
    LS_SNPRINTF(t_ls_audio_layers_file_ux_tap_inspect, s, sizeof(p->text), "WHOLE TRACK %ux  tap: inspect  Z zoom",
             1u << p->zoom);
    text(sf, r, 2, s, TUI_WHITE);
  }
}
tui_rect ls_audio_layers_strip(const ls_audio_layers *p) {
  return p ? p->overview : tui_rect_make(0, 0, 0, 0);
}
bool ls_audio_layers_pinned(const ls_audio_layers *p) { return p && p->pinned; }
void ls_audio_layers_unpin(ls_audio_layers *p) {
  if (p) {
    p->pinned = false;
    p->dirty = true;
  }
}
LS_COLD bool ls_audio_layers_touch(ls_audio_layers *p, int x, int y) {
  if (!p || p->layer == LS_LAYER_TOOLS)
    return false;
  /* The whole-track strip moves playback; the screen owns that. A tap on
     the plot holds that instant and frequency for reading (LIVE lets go).
     The piano roll has nothing to inspect: it always follows. */
  const bool overview = false;
  if (!tui_rect_contains(p->plot, x, y))
    return false;
  if (p->layer == LS_LAYER_MELODIC)
    return true;
  p->pinned = true;
  p->dirty = true;
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
  if (p) {
    p->zoom = (p->zoom + 1) % 6;
    p->dirty = true;
  }
}
uint32_t ls_audio_layers_time(const ls_audio_layers *p) {
  return p ? p->time : 0;
}
float ls_audio_layers_frequency(const ls_audio_layers *p) {
  return p ? p->hz : 0;
}
