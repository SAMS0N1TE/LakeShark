#include "ls_audio_view.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "ls_music_backend.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
enum { VIEW_LEVEL, VIEW_BANDS, VIEW_SCAN, VIEW_WATERFALL };
static int view, palette, span_step;
#define PALETTES 5
static const uint8_t RAMP[PALETTES][16] = {
    {TUI_BLACK, TUI_BLUE, TUI_BLUE | TUI_BRIGHT, TUI_CYAN,
     TUI_CYAN | TUI_BRIGHT, TUI_GREEN, TUI_GREEN | TUI_BRIGHT, TUI_YELLOW,
     TUI_YELLOW | TUI_BRIGHT, TUI_RED, TUI_RED | TUI_BRIGHT, TUI_MAGENTA,
     TUI_MAGENTA | TUI_BRIGHT, TUI_WHITE, TUI_WHITE | TUI_BRIGHT,
     TUI_WHITE | TUI_BRIGHT},
    {TUI_BLACK, TUI_RED, TUI_RED, TUI_RED | TUI_BRIGHT, TUI_RED | TUI_BRIGHT,
     TUI_MAGENTA, TUI_MAGENTA | TUI_BRIGHT, TUI_YELLOW, TUI_YELLOW,
     TUI_YELLOW | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT, TUI_WHITE, TUI_WHITE,
     TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT},
    {TUI_BLACK, TUI_MAGENTA, TUI_MAGENTA, TUI_MAGENTA | TUI_BRIGHT,
     TUI_MAGENTA | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT, TUI_BLUE | TUI_BRIGHT,
     TUI_CYAN, TUI_CYAN | TUI_BRIGHT, TUI_CYAN | TUI_BRIGHT,
     TUI_GREEN | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
     TUI_WHITE, TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT},
    {TUI_BLACK, TUI_BLUE, TUI_BLUE, TUI_BLUE | TUI_BRIGHT,
     TUI_BLUE | TUI_BRIGHT, TUI_CYAN, TUI_CYAN, TUI_CYAN | TUI_BRIGHT,
     TUI_CYAN | TUI_BRIGHT, TUI_WHITE, TUI_WHITE, TUI_WHITE,
     TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT, TUI_WHITE | TUI_BRIGHT,
     TUI_WHITE | TUI_BRIGHT},
    {TUI_BLACK, TUI_GREEN, TUI_GREEN, TUI_GREEN, TUI_GREEN, TUI_GREEN,
     TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT,
     TUI_GREEN | TUI_BRIGHT, TUI_GREEN | TUI_BRIGHT, TUI_YELLOW | TUI_BRIGHT,
     TUI_YELLOW | TUI_BRIGHT, TUI_WHITE, TUI_WHITE | TUI_BRIGHT,
     TUI_WHITE | TUI_BRIGHT}};
static uint8_t ramp(int level) {
  return RAMP[palette][level < 0 ? 0 : level > 15 ? 15 : level];
}

/* The scan and the spectrogram keep their history in PSRAM while the app
   is open; internal RAM is too scarce for it. */
#define SCAN_MAX 128
#define WF_COLS 128
#define WF_ROWS 96
#define SCAN_TOP_DB (-12.0f)
#define SCAN_FLOOR_DB (-84.0f)
#define WF_TOP_DB (-20.0f)
#define WF_FLOOR_DB (-100.0f)
static const int SPAN_HOPS[3] = {1, 2, 4};
typedef struct {
  float scan[SCAN_MAX], scan_cap[SCAN_MAX], now[SCAN_MAX];
  int64_t scan_until[SCAN_MAX];
  float column[WF_ROWS];
  uint8_t wf[WF_COLS][WF_ROWS];
  int wf_head, wf_filled, scan_n;
  ls_music_cursor_t cursor;
} vis_t;
static vis_t *vis;
static void *vis_alloc(void) {
#ifdef ESP_PLATFORM
  return heap_caps_calloc(1, sizeof(vis_t),
                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return calloc(1, sizeof(vis_t));
#endif
}
static void vis_free(void) {
#ifdef ESP_PLATFORM
  heap_caps_free(vis);
#else
  free(vis);
#endif
  vis = NULL;
}
static const uint8_t white = TUI_ATTR(TUI_WHITE, TUI_BLACK),
                     cyan = TUI_ATTR(TUI_CYAN, TUI_BLACK),
                     ghost = TUI_ATTR(LS_FAINT_FG, TUI_BLACK);

/* Meter segments light at these source levels, dBFS. Finer near the top,
   where program material spends its time. */
#define SEGS 20
static const float SEG[SEGS] = {-40, -34, -28,   -24, -20,   -17, -14,
                                -12, -10, -8,    -7,  -6,    -5,  -4,
                                -3,  -2,  -1.5f, -1,  -0.5f, 0};
static const char *const SEG_NAME[SEGS] = {
    "-40", "-34", "-28", "-24", "-20", "-17", "-14", "-12", "-10", "-8",
    "-7",  "-6",  "-5",  "-4",  "-3",  "-2",  "",    "-1",  "",    "0"};
/* ISO 2/3-octave centres, 63 Hz to 16 kHz. */
#define BANDS 13
#define LO_HZ 63.0f
#define HI_HZ 16000.0f
static const char *const BAND_NAME[BANDS] = {
    "63",   "100",  "160", "250",  "400", "630", "1k",
    "1.6k", "2.5k", "4k",  "6.3k", "10k", "16k"};
#define FLOOR_DB (-60.0f)
#define METER_FALL_DB_S 24.0f
#define BAND_FALL_DB_S 36.0f
#define CAP_FALL_DB_S 18.0f
#define HOLD_US 1500000
#define CAP_US 800000
#define OVER_US 2000000
EXT_RAM_BSS_ATTR static float meter_db[2], hold_db[2], band_db[BANDS],
    cap_db[BANDS];
EXT_RAM_BSS_ATTR static int64_t hold_until[2], over_until[2], cap_until[BANDS];
static int64_t last_us;
static int usable_bands = BANDS;
static void reset_levels(void) {
  for (int ch = 0; ch < 2; ch++)
    meter_db[ch] = hold_db[ch] = FLOOR_DB - 1, over_until[ch] = 0;
  for (int b = 0; b < BANDS; b++)
    band_db[b] = cap_db[b] = FLOOR_DB - 1;
  last_us = 0;
}
static void settle_to(float floor, float *level, float *hold, int64_t *until,
                      float now_db, float fall, float hold_fall,
                      int64_t hold_us, int64_t now, float dt) {
  *level = now_db > *level ? now_db : fmaxf(now_db, *level - fall * dt);
  if (*level < floor - 1)
    *level = floor - 1;
  if (*level >= *hold) {
    *hold = *level;
    *until = now + hold_us;
  } else if (now > *until)
    *hold = fmaxf(*level, *hold - hold_fall * dt);
}
static void settle(float *level, float *hold, int64_t *until, float now_db,
                   float fall, float hold_fall, int64_t hold_us, int64_t now,
                   float dt) {
  settle_to(FLOOR_DB, level, hold, until, now_db, fall, hold_fall, hold_us, now,
            dt);
}
static float scan_top_hz(void) {
  ls_music_format_t f;
  float hi = ls_music_format(&f) ? f.rate_hz * 0.45f : 16000.0f;
  return hi > 20000.0f ? 20000.0f : hi;
}
#define SCAN_LO_HZ 40.0f
/* Thin bars fall fast; their dots hang a moment and drop more slowly. */
static void update_scan(bool playing, int n, int64_t now, float dt) {
  if (!vis || n < 2)
    return;
  if (n > SCAN_MAX)
    n = SCAN_MAX;
  vis->scan_n = n;
  int measured =
      playing ? ls_music_spectrum(SCAN_LO_HZ, scan_top_hz(), n, vis->now) : 0;
  for (int i = 0; i < n; i++)
    settle_to(SCAN_FLOOR_DB, &vis->scan[i], &vis->scan_cap[i],
              &vis->scan_until[i],
              i < measured ? vis->now[i] : SCAN_FLOOR_DB - 1, 60.0f, 24.0f,
              350000, now, dt);
}
static uint8_t wf_level(float db) {
  int l = (int)((db - WF_FLOOR_DB) * 16 / (WF_TOP_DB - WF_FLOOR_DB));
  return (uint8_t)(l < 0 ? 0 : l > 15 ? 15 : l);
}
/* Every column the decoder has produced since the last frame, in order. */
static void update_waterfall(void) {
  if (!vis)
    return;
  for (int n = 0;
       n < 16 && ls_music_spectrogram(&vis->cursor, SPAN_HOPS[span_step],
                                      WF_ROWS, vis->column);
       n++) {
    vis->wf_head = (vis->wf_head + 1) % WF_COLS;
    for (int r = 0; r < WF_ROWS; r++)
      vis->wf[vis->wf_head][r] = wf_level(vis->column[r]);
    if (vis->wf_filled < WF_COLS)
      vis->wf_filled++;
  }
}
/* This frame's clock, for the views that settle while they draw. */
static int64_t frame_now;
static float frame_dt;
static void update_levels(bool playing) {
  int64_t now = esp_timer_get_time();
  float dt = last_us ? (float)(now - last_us) / 1e6f : 0;
  if (dt < 0 || dt > 0.25f)
    dt = 0.25f;
  last_us = now;
  frame_now = now;
  frame_dt = dt;
  for (int ch = 0; ch < 2; ch++) {
    unsigned peak = ls_music_peak(ch);
    float db = peak ? 20.0f * log10f(peak / 32768.0f) : FLOOR_DB - 1;
    if (peak >= 32767)
      over_until[ch] = now + OVER_US;
    settle(&meter_db[ch], &hold_db[ch], &hold_until[ch], db, METER_FALL_DB_S,
           METER_FALL_DB_S, HOLD_US, now, dt);
  }
  float db[BANDS];
  int measured = playing && view == VIEW_BANDS
                     ? ls_music_spectrum(LO_HZ, HI_HZ, BANDS, db)
                     : 0;
  if (measured)
    usable_bands = measured;
  for (int b = 0; b < BANDS; b++)
    settle(&band_db[b], &cap_db[b], &cap_until[b],
           b < measured ? db[b] : FLOOR_DB - 1, BAND_FALL_DB_S, CAP_FALL_DB_S,
           CAP_US, now, dt);
}

static void center(tui_surface *sf, tui_rect r, int y, const char *text,
                   uint8_t attr) {
  int n = (int)strlen(text);
  if (n > r.w - 2)
    n = r.w - 2;
  if (n < 1)
    return;
  char cut[160];
  snprintf(cut, sizeof(cut), "%.*s", n, text);
  tui_put_str(sf, r, r.x + (r.w - n) / 2, y, cut, attr);
}
static uint8_t seg_hue(float db) {
  return db >= -1.0f   ? TUI_RED | TUI_BRIGHT
         : db >= -6.0f ? TUI_YELLOW | TUI_BRIGHT
                       : TUI_CYAN | TUI_BRIGHT;
}
static uint8_t band_hue(float db) {
  return db >= -6.0f    ? TUI_RED | TUI_BRIGHT
         : db >= -18.0f ? TUI_YELLOW | TUI_BRIGHT
                        : TUI_YELLOW;
}
static void db_text(char *out, size_t n, float db) {
  if (db < FLOOR_DB)
    snprintf(out, n, "  -- ");
  else
    snprintf(out, n, "%5.1f", db);
}
/* The segment the held peak sits in, or -1. */
static int hold_segment(float hold) {
  int s = -1;
  while (s + 1 < SEGS && hold >= SEG[s + 1])
    s++;
  return s;
}
static uint8_t seg_attr(int s, float level, int held) {
  if (s == held && level < SEG[s])
    return TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
  return level >= SEG[s] ? TUI_ATTR(seg_hue(SEG[s]), TUI_BLACK) : ghost;
}
static void over_lamp(tui_surface *sf, tui_rect clip, int x, int y, int ch) {
  bool on = esp_timer_get_time() < over_until[ch];
  tui_put_str(sf, clip, x, y, "OVER",
              on ? TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT) : ghost);
}
/* L-R difference of the meters, a notch per 2 dB either side. */
static void balance(tui_surface *sf, tui_rect a, int y) {
  const int side = 6;
  int w = 2 * side + 1, x = a.x + (a.w - (w * 2 + 12)) / 2;
  if (x < a.x + 1)
    x = a.x + 1;
  bool live = meter_db[0] >= FLOOR_DB || meter_db[1] >= FLOOR_DB;
  float diff = meter_db[1] - meter_db[0];
  int pos = (int)lroundf(diff / 2.0f);
  if (pos < -side)
    pos = -side;
  if (pos > side)
    pos = side;
  tui_put_str(sf, a, x, y, "BALANCE  L", LS_ATTR_DIM);
  x += 11;
  for (int i = -side; i <= side; i++) {
    bool mark = live && i == pos;
    tui_put_str(sf, a, x + (i + side) * 2, y,
                mark ? "##"
                : i  ? ". "
                     : "| ",
                mark ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                : i  ? LS_ATTR_DIM
                     : cyan);
  }
  tui_put_str(sf, a, x + w * 2, y, "R", LS_ATTR_DIM);
}

/* Landscape: two long horizontal bars with the scale between them. */
static void meters_across(tui_surface *sf, tui_rect a) {
  const int label = 3, readout = 13;
  int pitch = (a.w - label - readout) / SEGS;
  if (pitch > 4)
    pitch = 4;
  if (pitch < 1)
    return;
  int span = pitch * SEGS,
      x0 = a.x + label + (a.w - label - readout - span) / 2;
  int thick = (a.h - 7) / 2;
  if (thick > 6)
    thick = 6;
  if (thick < 1)
    thick = 1;
  int block = 2 * thick + 1 + (a.h >= 2 * thick + 6 ? 4 : 0);
  int y0 = a.y + (a.h - block) / 2;
  for (int ch = 0; ch < 2; ch++) {
    int y = y0 + ch * (thick + 1), held = hold_segment(hold_db[ch]);
    for (int s = 0; s < SEGS; s++) {
      uint8_t at = seg_attr(s, meter_db[ch], held);
      for (int yy = 0; yy < thick; yy++)
        for (int c = 0; c < pitch; c++)
          tui_put_char(sf, a, x0 + s * pitch + c, y + yy,
                       c < pitch - 1 || pitch == 1 ? LS_TUI_BLOCK_FULL
                                                   : LS_TUI_BLOCK_LEFT,
                       at);
    }
    tui_put_str(sf, a, x0 - label, y + thick / 2, ch ? "R" : "L", cyan);
    char text[16];
    db_text(text, sizeof(text), meter_db[ch]);
    tui_put_str(sf, a, x0 + span + 2, y + thick / 2, text, white);
    over_lamp(sf, a, x0 + span + 8, y + thick / 2, ch);
  }
  int scale = y0 + thick, last = x0 - label;
  tui_put_str(sf, a, x0 + span + 2, scale, "dBFS", LS_ATTR_DIM);
  for (int s = 0; s < SEGS; s++) {
    int n = (int)strlen(SEG_NAME[s]),
        x = x0 + s * pitch + (pitch - 1) / 2 - (n - 1) / 2;
    if (!n || x <= last)
      continue;
    tui_put_str(sf, a, x, scale, SEG_NAME[s],
                SEG[s] >= -1.0f ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)
                                : LS_ATTR_DIM);
    last = x + n;
  }
  if (block == 2 * thick + 1)
    return;
  int y = y0 + 2 * thick + 2;
  char line[64], l[16], r[16];
  db_text(l, sizeof(l), hold_db[0]);
  db_text(r, sizeof(r), hold_db[1]);
  snprintf(line, sizeof(line), "PEAK HOLD   L %s   R %s", l, r);
  tui_put_str(sf, a, x0, y, line, white);
  balance(sf, a, y + 2);
}

/* Portrait: two tall columns with the scale between them. */
static void meters_up(tui_surface *sf, tui_rect a) {
  const int mid = 6;
  int bar = (a.w - mid - 4) / 2;
  if (bar > 14)
    bar = 14;
  int rows = a.h - 5, per = rows / SEGS;
  if (bar < 2 || rows < SEGS / 2)
    return;
  if (per < 1)
    per = 1;
  int height = per * SEGS < rows ? per * SEGS : rows;
  int step_num = height, x0 = a.x + (a.w - (2 * bar + mid)) / 2,
      y_top = a.y + 1 + (rows - height) / 2, base = y_top + height - 1;
  for (int ch = 0; ch < 2; ch++) {
    int x = x0 + ch * (bar + mid), held = hold_segment(hold_db[ch]);
    over_lamp(sf, a, x + (bar - 4) / 2, y_top - 1, ch);
    for (int i = 0; i < height; i++) {
      int s = i * SEGS / step_num;
      bool gap = (i + 1) * SEGS / step_num != s || i == height - 1;
      uint8_t at = seg_attr(s, meter_db[ch], held);
      for (int c = 0; c < bar; c++)
        tui_put_char(sf, a, x + c, base - i,
                     gap ? LS_TUI_SEXT(0x3C) : LS_TUI_BLOCK_FULL, at);
    }
    char text[24], db[16];
    db_text(db, sizeof(db), meter_db[ch]);
    snprintf(text, sizeof(text), "%s %s", ch ? "R" : "L", db);
    tui_put_str(sf, a, x + (bar - (int)strlen(text)) / 2, base + 1, text,
                white);
  }
  int last = -1;
  for (int i = height - 1; i >= 0; i--) {
    int s = i * SEGS / step_num;
    if (i + 1 < height && (i + 1) * SEGS / step_num == s)
      continue;
    int y = base - i;
    if (!*SEG_NAME[s] || y <= last)
      continue;
    char text[8];
    snprintf(text, sizeof(text), "%4s", SEG_NAME[s]);
    tui_put_str(sf, a, x0 + bar + 1, y, text,
                SEG[s] >= -1.0f ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK)
                                : LS_ATTR_DIM);
    last = y;
  }
  char l[16], r[16], line[48];
  db_text(l, sizeof(l), hold_db[0]);
  db_text(r, sizeof(r), hold_db[1]);
  snprintf(line, sizeof(line), "PEAK HOLD  L %s  R %s", l, r);
  center(sf, a, base + 2, line, LS_ATTR_DIM);
  balance(sf, a, base + 3);
}

static void spectrum(tui_surface *sf, tui_rect a) {
  const int scale = 4;
  int pitch = (a.w - scale) / BANDS;
  if (pitch > 6)
    pitch = 6;
  if (pitch < 2)
    return;
  bool stagger = pitch < 5;
  int span = pitch * BANDS - 1, x0 = a.x + scale + (a.w - scale - span) / 2;
  int rows = a.h - (stagger ? 2 : 1);
  if (rows < 4)
    return;
  int base = a.y + rows - 1;
  for (int db = 0; db > FLOOR_DB; db -= 12) {
    char text[8];
    snprintf(text, sizeof(text), "%3d", db);
    int y = a.y + (int)(-db * rows / -FLOOR_DB);
    tui_put_str(sf, a, x0 - scale, y, text, LS_ATTR_DIM);
    for (int b = 1; b < BANDS; b++)
      tui_put_char(sf, a, x0 + b * pitch - 1, y, '.', ghost);
  }
  for (int b = 0; b < BANDS; b++) {
    int x = x0 + b * pitch;
    bool measured = b < usable_bands;
    int lit = measured
                  ? (int)lroundf((band_db[b] - FLOOR_DB) / -FLOOR_DB * rows)
                  : 0,
        cap = measured
                  ? (int)lroundf((cap_db[b] - FLOOR_DB) / -FLOOR_DB * rows) - 1
                  : -1;
    for (int i = 0; i < rows; i++) {
      float row_db = FLOOR_DB + (i + 1) * -FLOOR_DB / rows;
      uint8_t at = !measured ? ghost
                   : i < lit ? TUI_ATTR(band_hue(row_db), TUI_BLACK)
                   : i == cap && cap >= lit
                       ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
                       : ghost;
      for (int c = 0; c < pitch - 1; c++)
        tui_put_char(sf, a, x + c, base - i,
                     measured ? LS_TUI_SEXT(0x3C) : LS_TUI_SHADE_25, at);
    }
    int n = (int)strlen(BAND_NAME[b]);
    int lx = x + (pitch - 1) / 2 - (n - 1) / 2;
    tui_put_str(sf, a, lx, base + 1 + (stagger && b % 2), BAND_NAME[b],
                measured ? cyan : ghost);
  }
}

static void freq_text(char *out, size_t n, float hz) {
  if (hz >= 1000.0f)
    snprintf(out, n,
             hz >= 10000.0f || fmodf(hz, 1000.0f) < 1 ? "%.0fk" : "%.1fk",
             (double)(hz / 1000.0f));
  else
    snprintf(out, n, "%.0f", (double)hz);
}
/* Many thin bars, one per column, coloured by height, each with a dot that
   hangs above it and then falls: the sub-GHz scan's look, over audio. */
static void scan_view(tui_surface *sf, tui_rect a, bool playing) {
  const int scale = 4;
  tui_rect plot = tui_rect_make(a.x + scale, a.y, a.w - scale - 1, a.h - 1);
  if (!vis || plot.w < 8 || plot.h < 4)
    return;
  int n = plot.w > SCAN_MAX ? SCAN_MAX : plot.w;
  update_scan(playing, n, frame_now, frame_dt);
  const float range = SCAN_TOP_DB - SCAN_FLOOR_DB;
  const int rows = plot.h;
  for (int db = (int)SCAN_TOP_DB; db >= (int)SCAN_FLOOR_DB; db -= 12) {
    char text[8];
    snprintf(text, sizeof(text), "%3d", db);
    int y = plot.y + (int)((SCAN_TOP_DB - db) / range * (rows - 1) + 0.5f);
    tui_put_str(sf, a, a.x, y, text, LS_ATTR_DIM);
  }
  for (int i = 0; i < n; i++) {
    float v = (vis->scan[i] - SCAN_FLOOR_DB) / range;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    int eighths = (int)(v * rows * 8 + 0.5f), h = (eighths + 7) / 8;
    float cv = (vis->scan_cap[i] - SCAN_FLOOR_DB) / range;
    cv = cv < 0 ? 0 : cv > 1 ? 1 : cv;
    int cap = (int)(cv * rows + 0.5f);
    for (int from_bottom = 0; from_bottom < rows; from_bottom++) {
      int level = 1 + from_bottom * 14 / (rows > 1 ? rows - 1 : 1);
      char g;
      uint8_t at;
      if (from_bottom < h - 1) {
        g = LS_TUI_TRACE(8);
        at = TUI_ATTR(ramp(level), TUI_BLACK);
      } else if (from_bottom == h - 1) {
        g = LS_TUI_TRACE(eighths - from_bottom * 8);
        at = TUI_ATTR(ramp(level) | TUI_BRIGHT, TUI_BLACK);
      } else if (from_bottom == cap - 1 && cap > h) {
        g = LS_TUI_TRACE(1);
        at = TUI_ATTR(ramp(level + 2) | TUI_BRIGHT, TUI_BLACK);
      } else if (from_bottom == 0) {
        g = LS_TUI_TRACE(1);
        at = TUI_ATTR(ramp(2), TUI_BLACK);
      } else
        continue;
      tui_put_char(sf, plot, plot.x + i, plot.y + rows - 1 - from_bottom, g,
                   at);
    }
  }
  static const float MARKS[] = {50,   100,  200,   500,  1000,
                                2000, 5000, 10000, 20000};
  float hi = scan_top_hz();
  int last = plot.x - 1;
  for (unsigned m = 0; m < sizeof(MARKS) / sizeof(*MARKS); m++) {
    if (MARKS[m] > hi)
      break;
    char text[8];
    freq_text(text, sizeof(text), MARKS[m]);
    int len = (int)strlen(text);
    int x =
        plot.x +
        (int)(logf(MARKS[m] / SCAN_LO_HZ) / logf(hi / SCAN_LO_HZ) * (n - 1) +
              0.5f) -
        (len - 1) / 2;
    if (x <= last)
      continue;
    tui_put_str(sf, a, x, plot.y + rows, text, cyan);
    last = x + len;
  }
}
/* Time runs left to right, newest at the right edge; frequency is linear
   from 0 at the bottom to Nyquist at the top, which is how a message hidden
   in audio is drawn and so how it reads. Each cell carries two slices. */
static void waterfall_view(tui_surface *sf, tui_rect a) {
  const int scale = 5;
  tui_rect plot = tui_rect_make(a.x + scale, a.y, a.w - scale - 1, a.h - 1);
  if (!vis || plot.w < 8 || plot.h < 4)
    return;

  if (plot.w > WF_COLS)
    plot.w = WF_COLS;
  const int halves = plot.h * 2;
  for (int c = 0; c < plot.w; c++) {
    int age = plot.w - 1 - c;
    if (age >= vis->wf_filled)
      continue;
    const uint8_t *col = vis->wf[(vis->wf_head - age + WF_COLS) % WF_COLS];
    for (int y = 0; y < plot.h; y++) {
      int level[2];
      for (int half = 0; half < 2; half++) {
        int from_bottom = halves - 1 - (2 * y + half);
        int lo = from_bottom * WF_ROWS / halves,
            hi = (from_bottom + 1) * WF_ROWS / halves;
        if (hi <= lo)
          hi = lo + 1;
        int best = 0;
        for (int r = lo; r < hi && r < WF_ROWS; r++)
          if (col[r] > best)
            best = col[r];
        level[half] = best;
      }
      tui_put_char(sf, plot, plot.x + c, plot.y + y, LS_TUI_BLOCK_UPPER,
                   TUI_ATTR(ramp(level[0]), ramp(level[1])));
    }
  }
  ls_music_format_t f;
  if (ls_music_format(&f)) {
    float nyquist = f.rate_hz / 2.0f;
    for (int i = 0; i <= 4; i++) {
      char text[8];
      freq_text(text, sizeof(text), nyquist * (4 - i) / 4);
      int y = plot.y + i * (plot.h - 1) / 4;
      tui_put_str(sf, a, a.x + scale - 1 - (int)strlen(text), y, text,
                  LS_ATTR_DIM);
    }
    char text[24];
    float seconds =
        (float)plot.w * SPAN_HOPS[span_step] * LS_MUSIC_FFT_SIZE / f.rate_hz;
    snprintf(text, sizeof(text), "-%.1fs", (double)seconds);
    tui_put_str(sf, a, plot.x, plot.y + plot.h, text, LS_ATTR_DIM);
  }
  tui_put_str(sf, a, plot.x + plot.w - 3, plot.y + plot.h, "NOW", cyan);
  if (!vis->wf_filled)
    center(sf, plot, plot.y + plot.h / 2, "Waiting for audio", LS_ATTR_DIM);
}
/* The colour key and the controls that belong to the plots, three rows. */

void ls_audio_view_open(void) {
  reset_levels();
  if (!vis)
    vis = vis_alloc();
  if (vis) {
    memset(vis, 0, sizeof(*vis));
    for (int i = 0; i < SCAN_MAX; i++)
      vis->scan[i] = vis->scan_cap[i] = SCAN_FLOOR_DB - 1;
  }
}
void ls_audio_view_close(void) { vis_free(); }
void ls_audio_view_reset(void) { ls_audio_view_open(); }
uint8_t ls_audio_view_color(int scheme, int level) {
  palette = scheme % PALETTES;
  return ramp(level);
}
void ls_audio_view_draw(tui_surface *sf, tui_rect r, int mode, int scheme,
                        int span, bool live) {
  if (r.w < 12 || r.h < 3)
    return;
  view = mode;
  palette = scheme % PALETTES;
  span_step = span % 3;
  update_levels(live);
  switch (view) {
  case VIEW_LEVEL:
    if (r.w > r.h * 2)
      meters_across(sf, r);
    else
      meters_up(sf, r);
    break;
  case VIEW_BANDS:
    spectrum(sf, r);
    break;
  case VIEW_SCAN:
    scan_view(sf, r, live);
    break;
  default:
    if (live)
      update_waterfall();
    waterfall_view(sf, r);
    break;
  }
  if (!vis && view >= VIEW_SCAN)
    ls_safe_line(sf, r, r.y + r.h / 2, "Not enough PSRAM for this view",
                 LS_ATTR_DIM);
}
