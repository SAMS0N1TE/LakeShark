/* Separate player/library views. The player's instrument panel shows one
   of four readings of the audio: stereo peak meters, a 2/3-octave band
   display, a fine scan of thin bars, and a scrolling spectrogram for finding
   what is in a recording. Landscape gives the panel most of the width,
   portrait most of the height. */
#include "../../ls_music_backend.h"
#include "../../ls_music_transport.h"
#include "../../ls_options.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
enum {
  PREV,
  PLAY,
  NEXT,
  QUIETER,
  SILENCE,
  LOUDER,
  LIBRARY,
  MIC,
  REC,
  TAB_LEVEL,
  TAB_BANDS,
  TAB_SCAN,
  TAB_WATERFALL,
  COLOUR,
  SPAN,
  TONE,
  SOURCE,
  RESCAN,
  BACK,
  PAGEUP,
  PAGEDOWN,
  OPTIONS,
  ACTIONS
};
/* The key each control answers to, drawn on it while a keyboard is fitted. */
static const char *const KEYS[ACTIONS] = {
    "B", "P", "N", "-", "M", "+", "L", "I", "W", "1", "2", "3",
    "4", "C", "Z", "E", "D", "R", "ESC", "U", "J", "O"};
enum { VIEW_LEVEL, VIEW_BANDS, VIEW_SCAN, VIEW_WATERFALL, VIEWS };
static const char *const VIEW_NAME[VIEWS] = {"LEVEL", "BANDS", "SCAN",
                                             "WATERFALL"};
static const char *const VIEW_SHORT[VIEWS] = {"LVL", "BND", "SCN", "WF"};
static const char *const VIEW_TITLE[VIEWS] = {
    " PEAK LEVEL ", " SPECTRUM 2/3 OCTAVE ", " SCAN ", " WATERFALL "};
/* Screen state that only the UI task touches lives in PSRAM: internal RAM
   is what boot runs out of, and the radio's endpoint needs it. */
EXT_RAM_BSS_ATTR static tui_rect hits[ACTIONS];
EXT_RAM_BSS_ATTR static bool enabled[ACTIONS];
static tui_rect rows_rect;
static bool ready, library;
static int selected, current = -1, top, source, focus = -1, row_height = 3,
                     visible_rows, restore_volume = 1;
/* Kept across visits and rotations: the panel is a preference. */
static int view = VIEW_LEVEL, palette, span_step;
static unsigned frame;

/* Colour ramps, sixteen levels from nothing to loudest. */
#define PALETTES 5
static const char *const PALETTE_NAME[PALETTES] = {"RAINBOW", "FIRE", "NEON",
                                                   "ICE", "PHOSPHOR"};
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
  return heap_caps_calloc(1, sizeof(vis_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
static const float SEG[SEGS] = {-40, -34, -28, -24, -20, -17, -14,
                                -12, -10, -8,  -7,  -6,  -5,  -4,
                                -3,  -2,  -1.5f, -1, -0.5f, 0};
static const char *const SEG_NAME[SEGS] = {
    "-40", "-34", "-28", "-24", "-20", "-17", "-14", "-12", "-10", "-8",
    "-7",  "-6",  "-5",  "-4",  "-3",  "-2",  "",    "-1",  "",    "0"};
/* ISO 2/3-octave centres, 63 Hz to 16 kHz. */
#define BANDS 13
#define LO_HZ 63.0f
#define HI_HZ 16000.0f
static const char *const BAND_NAME[BANDS] = {
    "63", "100", "160", "250", "400", "630", "1k",
    "1.6k", "2.5k", "4k", "6.3k", "10k", "16k"};
#define FLOOR_DB (-60.0f)
#define METER_FALL_DB_S 24.0f
#define BAND_FALL_DB_S 36.0f
#define CAP_FALL_DB_S 18.0f
#define HOLD_US 1500000
#define CAP_US 800000
#define OVER_US 2000000
EXT_RAM_BSS_ATTR static float meter_db[2], hold_db[2], band_db[BANDS], cap_db[BANDS];
EXT_RAM_BSS_ATTR static int64_t hold_until[2], over_until[2], cap_until[BANDS];
static int64_t last_us;
static int usable_bands = BANDS;

static void volume_to_step(int direction) {
  int v = ls_music_volume(), target;
  if (direction > 0)
    target = (v / 5 + 1) * 5;
  else
    target = ((v + 4) / 5 - 1) * 5;
  if (target < 0)
    target = 0;
  if (target > 100)
    target = 100;
  if (target != v)
    ls_music_volume_step(target - v);
}
static void start(int index) {
  if (ready && ls_music_play(index)) {
    selected = current = index;
    library = false;
    focus = -1;
  }
}
/* Stop listening. A recording that just ended is rescanned into the SD
   list and selected, so PLAY replays it with the plots. */
static void mic_stopped(void) {
  bool recorded = ls_music_mic_state() == LS_MIC_RECORDING;
  ls_music_mic_stop();
  if (!recorded || !ready || source != 0)
    return;
  const char *path = ls_music_mic_file(), *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  ls_music_scan(source);
  current = -1;
  for (int i = 0; i < ls_music_count(); i++)
    if (!strcmp(ls_music_name(i), name))
      selected = i;
}
/* OPTIONS: how the player looks and sounds, each row doing what its own
   control does - the view and colour tabs, SPAN, TONE and SOURCE. */
static void act(int id);
static void o_act(const ls_opt_t *o) {
  act(o->arg == TAB_LEVEL ? TAB_LEVEL + (view + 1) % VIEWS : o->arg);
}
static void o_show(const ls_opt_t *o, char *out, size_t n) {
  switch (o->arg) {
  case TAB_LEVEL:
    snprintf(out, n, "%s", VIEW_NAME[view]);
    break;
  case COLOUR:
    snprintf(out, n, "%s", PALETTE_NAME[palette]);
    break;
  case SPAN: {
    ls_music_format_t f;
    unsigned rate = ls_music_format(&f) ? f.rate_hz : 44100;
    snprintf(out, n, "%u ms/COL",
             (unsigned)(SPAN_HOPS[span_step] * LS_MUSIC_FFT_SIZE * 1000u / rate));
    break;
  }
  case TONE:
    snprintf(out, n, "%s", ls_music_warm() ? "WARM" : "FLAT");
    break;
  case SOURCE:
    snprintf(out, n, "%s", source ? "FLASH" : "SD");
    break;
  default:
    break;
  }
}
#define MUSIC_ROW(l, id)                                                       \
  { .label = (l), .kind = LS_OPT_ACTION, .arg = (id), .act = o_act, .show = o_show }
static const ls_opt_t OPT_MUSIC[] = {
    MUSIC_ROW("VIEW", TAB_LEVEL), MUSIC_ROW("COLOUR", COLOUR),
    MUSIC_ROW("SPAN", SPAN),      MUSIC_ROW("TONE", TONE),
    MUSIC_ROW("SOURCE", SOURCE),
};
#undef MUSIC_ROW
static const ls_opt_ctx_t CTX_MUSIC = {.name = "MUSIC", .job = -1,
                                       .radio = LS_RSEL_NONE,
                                       LS_OPT_ROWS(OPT_MUSIC), .tag = "PLAYER"};
static void act(int id) {
  switch (id) {
  case PREV:
    start(ls_music_step(selected, ls_music_count(), -1));
    break;
  case NEXT:
    start(ls_music_step(selected, ls_music_count(), 1));
    break;
  case PLAY:
    if (current >= 0 && ls_music_state() != LS_MUSIC_STOPPED)
      ls_music_toggle();
    else
      start(selected);
    break;
  case QUIETER:
    volume_to_step(-1);
    break;
  case LOUDER:
    volume_to_step(1);
    break;
  case SILENCE:
    if (ls_music_volume() > 0) {
      restore_volume = ls_music_volume();
      ls_music_volume_step(-restore_volume);
    } else
      ls_music_volume_step(restore_volume);
    break;
  case MIC:
    if (ls_music_mic_state() != LS_MIC_OFF)
      mic_stopped();
    else
      ls_music_mic_start(false);
    break;
  case REC:
    if (ls_music_mic_state() == LS_MIC_RECORDING)
      mic_stopped();
    else
      ls_music_mic_start(true);
    break;
  case LIBRARY:
    library = true;
    focus = -1;
    break;
  case TAB_LEVEL:
  case TAB_BANDS:
  case TAB_SCAN:
  case TAB_WATERFALL:
    view = VIEW_LEVEL + (id - TAB_LEVEL);
    break;
  case COLOUR:
    palette = (palette + 1) % PALETTES;
    break;
  case SPAN:
    span_step = (span_step + 1) % 3;
    if (vis) {
      vis->wf_filled = 0;
      vis->cursor.started = false;
    }
    break;
  case BACK:
    library = false;
    focus = -1;
    break;
  case TONE:
    ls_music_set_warm(!ls_music_warm());
    break;
  case SOURCE:
    source = 1 - source; /* fall through */
  case RESCAN:
    ready = ls_music_open();
    if (ready)
      ls_music_scan(source);
    selected = top = 0;
    current = -1;
    break;
  case PAGEUP:
    selected -= visible_rows;
    if (selected < 0)
      selected = 0;
    break;
  case PAGEDOWN:
    selected += visible_rows;
    if (selected >= ls_music_count())
      selected = ls_music_count() - 1;
    break;
  case OPTIONS:
    ls_opt_open(&CTX_MUSIC);
    break;
  }
}
static void reset_levels(void) {
  for (int ch = 0; ch < 2; ch++)
    meter_db[ch] = hold_db[ch] = FLOOR_DB - 1, over_until[ch] = 0;
  for (int b = 0; b < BANDS; b++)
    band_db[b] = cap_db[b] = FLOOR_DB - 1;
  last_us = 0;
}
static void enter(void) {
  selected = top = 0;
  current = -1;
  focus = -1;
  library = false;
  frame = 0;
  reset_levels();
  if (!vis)
    vis = vis_alloc();
  if (vis)
    for (int i = 0; i < SCAN_MAX; i++)
      vis->scan[i] = vis->scan_cap[i] = SCAN_FLOOR_DB - 1;
  ready = ls_music_open();
  if (ready)
    ls_music_scan(source);
}
static void leave(void) {
  ls_music_close();
  vis_free();
  ready = false;
  current = -1;
}

/* Fast attack, steady release; the held peak waits, then falls. */
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
  int measured = playing ? ls_music_spectrum(SCAN_LO_HZ, scan_top_hz(), n,
                                             vis->now)
                         : 0;
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
  for (int n = 0; n < 16 && ls_music_spectrogram(&vis->cursor,
                                                  SPAN_HOPS[span_step], WF_ROWS,
                                                  vis->column);
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
static void button(tui_surface *sf, int id, tui_rect r, const char *label,
                   bool primary, bool available) {
  int pad = ls_tui_corner_pad(r.y), bottom = ls_tui_corner_pad(r.y + r.h - 1);
  if (bottom > pad)
    pad = bottom;
  int right = r.x + r.w, cols = tui_surface_rect(sf).w;
  if (r.x < pad)
    r.x = pad;
  if (right > cols - pad)
    right = cols - pad;
  r.w = right - r.x;
  if (r.w < 3 || r.h < 3)
    return;
  hits[id] = r;
  enabled[id] = available;
  uint8_t edge = TUI_ATTR(!available    ? TUI_WHITE
                          : id == focus ? TUI_YELLOW
                                        : TUI_CYAN,
                          TUI_BLACK);
  uint8_t face = primary && available ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : white;
  tui_fill(sf, r, ' ', face);
  for (int x = r.x + 1; x < r.x + r.w - 1; x++) {
    tui_put_char(sf, r, x, r.y, '-', edge);
    tui_put_char(sf, r, x, r.y + r.h - 1, '-', edge);
  }
  for (int y = r.y + 1; y < r.y + r.h - 1; y++) {
    tui_put_char(sf, r, r.x, y, '|', edge);
    tui_put_char(sf, r, r.x + r.w - 1, y, '|', edge);
  }
  tui_put_char(sf, r, r.x, r.y, '.', edge);
  tui_put_char(sf, r, r.x + r.w - 1, r.y, '.', edge);
  tui_put_char(sf, r, r.x, r.y + r.h - 1, '\'', edge);
  tui_put_char(sf, r, r.x + r.w - 1, r.y + r.h - 1, '\'', edge);
  if (label)
    center(sf, r, r.y + r.h / 2, label, available ? face : LS_ATTR_DIM);
  if (ls_tui_keyboard_mode()) {
    char badge[8];
    int n = snprintf(badge, sizeof(badge), "[%s]", KEYS[id]);
    if (n + 2 < r.w)
      tui_put_str(sf, r, r.x + r.w - 1 - n, r.y, badge,
                  TUI_ATTR(available ? TUI_YELLOW : TUI_WHITE, TUI_BLACK));
  }
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
    tui_put_str(sf, a, x + (i + side) * 2, y, mark ? "##" : i ? ". " : "| ",
                mark ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                     : i ? LS_ATTR_DIM : cyan);
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
  int span = pitch * SEGS, x0 = a.x + label + (a.w - label - readout - span) / 2;
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
    int lit = measured ? (int)lroundf((band_db[b] - FLOOR_DB) / -FLOOR_DB *
                                      rows)
                       : 0,
        cap = measured ? (int)lroundf((cap_db[b] - FLOOR_DB) / -FLOOR_DB *
                                      rows) - 1
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
    snprintf(out, n, hz >= 10000.0f || fmodf(hz, 1000.0f) < 1 ? "%.0fk" : "%.1fk",
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
  static const float MARKS[] = {50,   100,  200,  500,  1000,
                                2000, 5000, 10000, 20000};
  float hi = scan_top_hz();
  int last = plot.x - 1;
  for (unsigned m = 0; m < sizeof(MARKS) / sizeof(*MARKS); m++) {
    if (MARKS[m] > hi)
      break;
    char text[8];
    freq_text(text, sizeof(text), MARKS[m]);
    int len = (int)strlen(text);
    int x = plot.x + (int)(logf(MARKS[m] / SCAN_LO_HZ) /
                           logf(hi / SCAN_LO_HZ) * (n - 1) + 0.5f) -
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
  update_waterfall();
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
    float seconds = (float)plot.w * SPAN_HOPS[span_step] * LS_MUSIC_FFT_SIZE /
                    f.rate_hz;
    snprintf(text, sizeof(text), "-%.1fs", (double)seconds);
    tui_put_str(sf, a, plot.x, plot.y + plot.h, text, LS_ATTR_DIM);
  }
  tui_put_str(sf, a, plot.x + plot.w - 3, plot.y + plot.h, "NOW", cyan);
  if (!vis->wf_filled)
    center(sf, plot, plot.y + plot.h / 2, "Waiting for audio", LS_ATTR_DIM);
}
/* The colour key and the controls that belong to the plots, three rows. */
static void strip(tui_surface *sf, tui_rect r, bool with_span, float lo_db,
                  float hi_db) {
  int bw = r.w >= 44 ? 14 : 11;
  int right = r.x + r.w;
  button(sf, COLOUR, tui_rect_make(right - bw, r.y, bw, 3),
         PALETTE_NAME[palette], false, true);
  right -= bw + 1;
  if (with_span) {
    char label[24];
    ls_music_format_t f;
    unsigned rate = ls_music_format(&f) ? f.rate_hz : 44100;
    snprintf(label, sizeof(label), "%u ms/COL",
             (unsigned)(SPAN_HOPS[span_step] * LS_MUSIC_FFT_SIZE * 1000u / rate));
    button(sf, SPAN, tui_rect_make(right - bw, r.y, bw, 3), label, false, true);
    right -= bw + 1;
  }
  if (right - r.x < 18)
    return;
  for (int i = 0; i < 16; i++)
    tui_put_char(sf, r, r.x + 1 + i, r.y + 1, LS_TUI_BLOCK_FULL,
                 TUI_ATTR(ramp(i), TUI_BLACK));
  char text[16];
  snprintf(text, sizeof(text), "%.0f", (double)lo_db);
  tui_put_str(sf, r, r.x + 1, r.y + 2, text, LS_ATTR_DIM);
  snprintf(text, sizeof(text), "%.0f dB", (double)hi_db);
  tui_put_str(sf, r, r.x + 17 - (int)strlen(text) + 3, r.y + 2, text,
              LS_ATTR_DIM);
}
static void tabs(tui_surface *sf, tui_rect t) {
  int tw = t.w / VIEWS;
  bool full = tw - 2 >= 9;
  for (int i = 0; i < VIEWS; i++) {
    int x = t.x + i * tw, w = i == VIEWS - 1 ? t.w - i * tw : tw - 1;
    button(sf, TAB_LEVEL + i, tui_rect_make(x, t.y, w, 3),
           full ? VIEW_NAME[i] : VIEW_SHORT[i], i == view, true);
  }
}
/* What the plots are reading. */
static const char *source_name(void) {
  return ls_music_mic_state() != LS_MIC_OFF ? "MICROPHONE" : "L+R AFTER TONE";
}
static void panel(tui_surface *sf, tui_rect p, bool playing) {
  int pad = ls_tui_corner_pad(p.y);
  if (ls_tui_corner_pad(p.y + p.h - 1) > pad)
    pad = ls_tui_corner_pad(p.y + p.h - 1);
  int cols = tui_surface_rect(sf).w;
  if (p.x < pad) {
    p.w -= pad - p.x;
    p.x = pad;
  }
  if (p.x + p.w > cols - pad)
    p.w = cols - pad - p.x;
  if (p.w < 20 || p.h < 12)
    return;
  ls_panel_box(sf, p, VIEW_TITLE[view], TUI_CYAN);
  tui_rect in = tui_rect_make(p.x + 1, p.y + 1, p.w - 2, p.h - 2);
  /* Tabs beside the format lines when there is room, above them when not. */
  const int info_w = 25;
  bool beside = in.w - info_w >= VIEWS * 11;
  int tabs_w = beside ? in.w - info_w : in.w;
  if (tabs_w > VIEWS * 12)
    tabs_w = VIEWS * 12;
  tabs(sf, tui_rect_make(in.x + in.w - tabs_w, in.y, tabs_w, 3));
  int info_y = beside ? in.y : in.y + 3;
  ls_music_format_t f;
  char line[80];
  if (ls_music_format(&f)) {
    snprintf(line, sizeof(line), "%u.%u kHz %u-BIT %s", f.rate_hz / 1000,
             f.rate_hz % 1000 / 100, f.bits, f.channels > 1 ? "STEREO" : "MONO");
    tui_put_str(sf, in, in.x + 1, info_y, line, white);
  } else
    tui_put_str(sf, in, in.x + 1, info_y, "NO SIGNAL", ghost);
  snprintf(line, sizeof(line), "OUT %s  TONE %s",
           f.mono_out ? "MONO" : "STEREO", ls_music_warm() ? "WARM" : "FLAT");
  tui_put_str(sf, in, in.x + 1, info_y + 1, line, LS_ATTR_DIM);
  int body_y = beside ? in.y + 3 : in.y + 6;
  tui_rect body = tui_rect_make(in.x, body_y, in.w, in.y + in.h - 1 - body_y);
  tui_rect plot = tui_rect_make(body.x, body.y, body.w, body.h - 3);
  tui_rect controls_row = tui_rect_make(body.x, body.y + body.h - 3, body.w, 3);
  unsigned rate = f.rate_hz;
  switch (view) {
  case VIEW_LEVEL:
    if (p.w > p.h * 2)
      meters_across(sf, body);
    else
      meters_up(sf, body);
    snprintf(line, sizeof(line), "Source L/R before downmix and volume");
    break;
  case VIEW_BANDS:
    spectrum(sf, body);
    if (rate)
      snprintf(line, sizeof(line), "FFT %d HANN  %.1f Hz/BIN  %s",
               LS_MUSIC_FFT_SIZE, (double)rate / LS_MUSIC_FFT_SIZE, source_name());
    else
      snprintf(line, sizeof(line), "FFT %d HANN  %s", LS_MUSIC_FFT_SIZE,
               source_name());
    if (usable_bands < BANDS && in.w > 60)
      strncat(line, "  DIM: ABOVE NYQUIST", sizeof(line) - strlen(line) - 1);
    break;
  case VIEW_SCAN: {
    scan_view(sf, plot, playing);
    strip(sf, controls_row, false, SCAN_FLOOR_DB, SCAN_TOP_DB);
    char lo[8], hi[8];
    freq_text(lo, sizeof(lo), SCAN_LO_HZ);
    freq_text(hi, sizeof(hi), scan_top_hz());
    snprintf(line, sizeof(line), "%s - %s Hz LOG  FFT %d  %s", lo, hi,
             LS_MUSIC_FFT_SIZE, source_name());
    break;
  }
  default: {
    waterfall_view(sf, plot);
    strip(sf, controls_row, true, WF_FLOOR_DB, WF_TOP_DB);
    char hi[8];
    freq_text(hi, sizeof(hi), rate ? rate / 2.0f : 22050.0f);
    snprintf(line, sizeof(line), "0 - %s Hz LINEAR  PAUSE TO HOLD THE PICTURE",
             hi);
    break;
  }
  }
  if (!vis && (view == VIEW_SCAN || view == VIEW_WATERFALL))
    ls_safe_line(sf, plot, plot.y + plot.h / 2, "Not enough memory for this view",
                 TUI_ATTR(TUI_RED, TUI_BLACK));
  ls_safe_line(sf, in, in.y + in.h - 1, line, LS_ATTR_DIM);
}

static void title(tui_surface *sf, tui_rect r) {
  char text[256];
  snprintf(text, sizeof(text), "%s",
           ls_music_name(current >= 0 ? current : selected));
  char *dot = strrchr(text, '.');
  if (dot)
    *dot = 0;
  char *split = strstr(text, " - ");
  const char *artist = "SD MUSIC", *song = text;
  if (split) {
    *split = 0;
    artist = text;
    song = split + 3;
  }
  ls_safe_line(sf, r, r.y + 3, artist, cyan);
  int overflow = (int)strlen(song) - (r.w - 2);
  if (overflow > 0 && ls_music_state() == LS_MUSIC_PLAYING) {
    unsigned phase = (frame / 8) % (unsigned)(overflow + 14);
    song += phase > 7 ? (phase - 7 > (unsigned)overflow ? (unsigned)overflow
                                                        : phase - 7)
                      : 0;
  }
  ls_safe_line(sf, r, r.y + 4, *song ? song : "Choose a track", white);
  const char *error = ls_music_error();
  if (*error)
    ls_safe_line(sf, r, r.y + 5, error, TUI_ATTR(TUI_RED, TUI_BLACK));
}
/* Status, title, clock and progress: eight rows. */
static void info(tui_surface *sf, tui_rect r, bool playing) {
  ls_music_mic_t mic = ls_music_mic_state();
  /* OPTIONS makes four where the row is wide enough to spell it; a narrower
     pane keeps three, and its view, colour and span keys are on the panel. */
  const int keys = (r.w - 3) / 4 >= 9 ? 4 : 3;
  int bw = keys == 4 ? 9 : (r.w - 2) / 3 > 10 ? 10 : (r.w - 2) / 3;
  int x = r.x + r.w - keys * bw - (keys - 1);
  ls_safe_line(sf, tui_rect_make(r.x, r.y, x - r.x, r.h), r.y + 1,
               mic == LS_MIC_RECORDING               ? "RECORDING"
               : mic == LS_MIC_LIVE                  ? "LISTENING"
               : playing                             ? "PLAYING"
               : ls_music_state() == LS_MUSIC_PAUSED ? "PAUSED"
                                                     : "READY",
               mic == LS_MIC_RECORDING ? TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT)
                                       : TUI_ATTR(TUI_GREEN, TUI_BLACK));
  /* Keys across the top: listen, record, the track list, and OPTIONS. */
  button(sf, MIC, tui_rect_make(x, r.y, bw, 3), "MIC", mic == LS_MIC_LIVE,
         ready);
  button(sf, REC, tui_rect_make(x + bw + 1, r.y, bw, 3), "REC",
         mic == LS_MIC_RECORDING, ready);
  button(sf, LIBRARY, tui_rect_make(x + 2 * (bw + 1), r.y, bw, 3), "TRACKS",
         false, true);
  if (keys == 4)
    button(sf, OPTIONS, tui_rect_make(x + 3 * (bw + 1), r.y, bw, 3), "OPTIONS",
           false, true);
  unsigned elapsed, total;
  if (mic != LS_MIC_OFF) {
    const char *path = ls_music_mic_file(), *name = strrchr(path, '/');
    ls_safe_line(sf, r, r.y + 3, "MICROPHONE", cyan);
    ls_safe_line(sf, r, r.y + 4,
                 mic == LS_MIC_RECORDING ? (name ? name + 1 : path)
                                         : "Live input, not recorded",
                 white);
    const char *error = ls_music_error();
    if (*error)
      ls_safe_line(sf, r, r.y + 5, error, TUI_ATTR(TUI_RED, TUI_BLACK));
    elapsed = mic == LS_MIC_RECORDING ? ls_music_mic_ms() / 1000 : 0;
    total = 0;
  } else {
    title(sf, r);
    elapsed = current >= 0 ? ls_music_position_ms() / 1000 : 0;
    total = current >= 0 ? ls_music_duration_ms() / 1000 : 0;
  }
  char clock[48];
  if (mic == LS_MIC_RECORDING)
    snprintf(clock, sizeof(clock), "%02u:%02u RECORDED", elapsed / 60,
             elapsed % 60);
  else if (mic == LS_MIC_LIVE)
    snprintf(clock, sizeof(clock), "LIVE");
  else
    snprintf(clock, sizeof(clock), "%02u:%02u / %02u:%02u", elapsed / 60,
             elapsed % 60, total / 60, total % 60);
  ls_safe_line(sf, r, r.y + 6, clock, white);
  int count = ls_music_count();
  if (count > 0) {
    char n[32];
    int len = snprintf(n, sizeof(n), "TRACK %d/%d",
                       (current >= 0 ? current : selected) + 1, count);
    tui_put_str(sf, r, r.x + r.w - 2 - len, r.y + 6, n, LS_ATTR_DIM);
  }
  int width = r.w - 4,
      filled = total ? (int)((uint64_t)elapsed * width / total) : 0;
  for (int x = 0; x < width; x++)
    tui_put_char(sf, r, r.x + 2 + x, r.y + 7,
                 x < filled ? LS_TUI_BLOCK_LOWER : '-',
                 x < filled ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                            : LS_ATTR_DIM);
}
/* Transport, volume readout and volume buttons: 2h+3 rows. */
static void controls(tui_surface *sf, tui_rect r, bool playing, int h) {
  /* Shrink the whole block clear of the rounded corner so every button
     keeps its share, rather than the outermost ones losing their labels. */
  int pad = 0, cols = tui_surface_rect(sf).w;
  for (int y = r.y; y < r.y + r.h; y++)
    if (ls_tui_corner_pad(y) > pad)
      pad = ls_tui_corner_pad(y);
  if (r.x < pad) {
    r.w -= pad - r.x;
    r.x = pad;
  }
  if (r.x + r.w > cols - pad)
    r.w = cols - pad - r.x;
  int q = r.w / 4, t = r.w / 3;
  bool can = ready && ls_music_count() > 0;
  button(sf, PREV, tui_rect_make(r.x, r.y, q - 1, h), "PREV", false, can);
  button(sf, PLAY, tui_rect_make(r.x + q, r.y, r.w - 2 * q, h),
         playing ? "PAUSE" : "PLAY", true, can);
  button(sf, NEXT, tui_rect_make(r.x + r.w - q + 1, r.y, q - 1, h), "NEXT",
         false, can);
  int value = ls_music_volume();
  if (value < 0)
    value = 0;
  if (value > 100)
    value = 100;
  char vol[48];
  if (value)
    snprintf(vol, sizeof(vol), "VOLUME %d%%  %.1f dB", value,
             (double)ls_music_volume_db());
  else
    snprintf(vol, sizeof(vol), "VOLUME 0%%  MUTED");
  center(sf, tui_rect_make(r.x, r.y + h, r.w, 1), r.y + h, vol, cyan);
  /* One segment per five-point step. */
  int gx = r.x + (r.w - 20) / 2;
  for (int i = 0; i < 20; i++)
    tui_put_char(sf, r, gx + i, r.y + h + 1, LS_TUI_BLOCK_LEFT,
                 i < value / 5 ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                               : ghost);
  int y = r.y + h + 3;
  button(sf, QUIETER, tui_rect_make(r.x, y, t - 1, h), "VOL -", false,
         value > 0);
  button(sf, SILENCE, tui_rect_make(r.x + t, y, r.w - 2 * t, h),
         value > 0 ? "MUTE" : "UNMUTE", false, true);
  button(sf, LOUDER, tui_rect_make(r.x + r.w - t + 1, y, t - 1, h), "VOL +",
         false, value < 100);
}
static void draw_library(tui_surface *sf, tui_rect r) {
  ls_safe_line(sf, r, r.y + 1, source ? "FLASH / MUSIC" : "SD / MUSIC", cyan);
  if (ls_tui_keyboard_mode())
    ls_safe_line(sf, tui_rect_make(r.x, r.y, r.w - 13, r.h), r.y + 2,
                 "UP/DOWN select  ENTER play  LEFT/RIGHT prev/next",
                 LS_ATTR_DIM);
  button(sf, BACK, tui_rect_make(r.x + r.w - 12, r.y, 12, 3), "BACK", false,
         true);
  row_height = r.h >= 40 ? 4 : 3;
  rows_rect = tui_rect_make(r.x + 1, r.y + 4, r.w - 2, r.h - 12);
  visible_rows = rows_rect.h / row_height;
  if (visible_rows < 1)
    visible_rows = 1;
  int count = ls_music_count();
  top = ls_music_scroll(selected, count, visible_rows, top);
  if (!count)
    ls_safe_line(sf, rows_rect, rows_rect.y, "No WAV tracks. Use RESCAN.",
                 LS_ATTR_DIM);
  for (int i = 0; i < visible_rows && top + i < count; i++) {
    int index = top + i, y = rows_rect.y + i * row_height;
    tui_rect row = tui_rect_make(rows_rect.x, y, rows_rect.w, row_height - 1);
    uint8_t attr = index == selected ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : white;
    tui_fill(sf, row, ' ', attr);
    ls_safe_line(sf, row, y + 1, ls_music_name(index), attr);
  }
  int y = r.y + r.h - 7, half = r.w / 2;
  button(sf, PAGEUP, tui_rect_make(r.x, y, half - 1, 3), "PREVIOUS PAGE", false,
         top > 0);
  button(sf, PAGEDOWN, tui_rect_make(r.x + half, y, r.w - half, 3), "NEXT PAGE",
         false, top + visible_rows < count);
  int third = r.w / 3;
  y += 4;
  button(sf, TONE, tui_rect_make(r.x, y, third - 1, 3),
         ls_music_warm() ? "TONE WARM" : "TONE FLAT", false, true);
  button(sf, SOURCE, tui_rect_make(r.x + third, y, third - 1, 3),
         source ? "SOURCE FLASH" : "SOURCE SD", false, true);
  button(sf, RESCAN, tui_rect_make(r.x + 2 * third, y, r.w - 2 * third, 3),
         "RESCAN", false, true);
}
static void draw(tui_surface *sf, tui_rect r) {
  memset(hits, 0, sizeof(hits));
  memset(enabled, 0, sizeof(enabled));
  bool playing = ls_music_state() == LS_MUSIC_PLAYING;
  if (playing)
    frame++;
  int count = ls_music_count();
  if (selected >= count)
    selected = count - 1;
  if (selected < 0 && count)
    selected = 0;
  if (library) {
    draw_library(sf, r);
    return;
  }
  bool live = ls_music_live();
  update_levels(live);
  bool wide = r.w > r.h * 2;
  if (wide) {
    int split = r.w * (r.w >= 100 ? 62 : 55) / 100;
    int h = (r.h - 12) / 2;
    if (h > 7)
      h = 7;
    if (h < 3)
      h = 3;
    tui_rect side = tui_rect_make(r.x + split, r.y, r.w - split, r.h);
    panel(sf, tui_rect_make(r.x, r.y, split - 1, r.h), live);
    info(sf, side, playing);
    controls(sf, tui_rect_make(side.x, side.y + side.h - (2 * h + 3), side.w,
                               2 * h + 3),
             playing, h);
  } else {
    const int h = 5;
    info(sf, r, playing);
    int bottom = r.y + r.h - (2 * h + 3);
    panel(sf, tui_rect_make(r.x, r.y + 9, r.w, bottom - r.y - 10), live);
    controls(sf, tui_rect_make(r.x, bottom, r.w, 2 * h + 3), playing, h);
  }
}
static bool key(ls_tk_t key, char ch) {
  if (key == LS_TK_ESC) {
    if (library) {
      library = false;
      focus = -1;
      return true;
    }
    if (focus >= 0) {
      focus = -1;
      return true;
    }
    return false;
  }
  if (key == LS_TK_TAB) {
    for (int i = 0; i < ACTIONS; i++) {
      focus = (focus + 1) % ACTIONS;
      if (enabled[focus])
        break;
    }
    return true;
  }
  if (key == LS_TK_ENTER) {
    if (focus >= 0 && enabled[focus])
      act(focus);
    else if (library)
      start(selected);
    else
      act(PLAY);
    return true;
  }
  if (key == LS_TK_UP || key == LS_TK_DOWN) {
    selected =
        ls_music_step(selected, ls_music_count(), key == LS_TK_UP ? -1 : 1);
    library = true;
    focus = -1;
    return true;
  }
  if (key == LS_TK_LEFT || key == LS_TK_RIGHT) {
    act(key == LS_TK_LEFT ? PREV : NEXT);
    return true;
  }
  if (key == LS_TK_MIC) {
    act(REC);
    return true;
  }
  if (key != LS_TK_CHAR)
    return false;
  if (ch >= 'A' && ch <= 'Z')
    ch += 'a' - 'A';
  switch (ch) {
  case 'p':
  case ' ':
    act(PLAY);
    break;
  case 'b':
    act(PREV);
    break;
  case 'n':
    act(NEXT);
    break;
  case '-':
    act(QUIETER);
    break;
  case '+':
  case '=':
    act(LOUDER);
    break;
  case 'm':
    act(SILENCE);
    break;
  case 'l':
    act(LIBRARY);
    break;
  case 'i':
    act(MIC);
    break;
  case 'w':
    act(REC);
    break;
  case 'v':
    act(TAB_LEVEL + (view + 1) % VIEWS);
    break;
  case '1':
  case '2':
  case '3':
  case '4':
    act(TAB_LEVEL + (ch - '1'));
    break;
  case 'c':
    act(COLOUR);
    break;
  case 'z':
    act(SPAN);
    break;
  case 'e':
    act(TONE);
    break;
  case 'd':
    act(SOURCE);
    break;
  case 'r':
    act(RESCAN);
    break;
  case 'u':
  case 'j':
    if (!library)
      return false;
    act(ch == 'u' ? PAGEUP : PAGEDOWN);
    break;
  case 'o':
    act(OPTIONS);
    break;
  case 's':
    ls_music_stop();
    current = -1;
    break;
  default:
    return false;
  }
  return true;
}
static bool touch(int x, int y) {
  for (int i = 0; i < ACTIONS; i++)
    if (enabled[i] && tui_rect_contains(hits[i], x, y)) {
      act(i);
      return true;
    }
  if (library && tui_rect_contains(rows_rect, x, y)) {
    int i = top + (y - rows_rect.y) / row_height;
    if (i < ls_music_count())
      start(i);
    return true;
  }
  /* Everything this screen does is a drawn control. A tap anywhere else is
     taken here, or the router would turn it into UP/ENTER/DOWN by where it
     landed, and DOWN opens the track list. */
  return true;
}
const ls_tui_screen_t ls_scr_music = {
    .name = "MUSIC",
    .hint = "1-4/V view  C colour  O options  SPACE play  I mic  W rec  L tracks  +/- vol",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch};
