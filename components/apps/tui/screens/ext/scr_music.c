/* Separate player/library views. The player's instrument panel shows one
   of four readings of the audio: stereo peak meters, a 2/3-octave band
   display, a fine scan of thin bars, and a scrolling spectrogram for finding
   what is in a recording. Landscape gives the panel most of the width,
   portrait most of the height. */
#include "../../ls_music_backend.h"
#include "../../ls_audio_view.h"
#include "../../ls_media_widgets.h"
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
static tui_rect rows_rect, progress_rect;
static bool ready, library;
static int selected, current = -1, top, source, focus = -1, row_height = 3,
                     visible_rows, restore_volume = 1;
/* Kept across visits and rotations: the panel is a preference. */
static int view = VIEW_LEVEL, palette, span_step;
static unsigned frame;
#define PALETTES 5
#define WF_FLOOR_DB (-100.0f)
#define WF_TOP_DB (-20.0f)
#define SCAN_FLOOR_DB (-84.0f)
#define SCAN_TOP_DB (-12.0f)
#define SCAN_LO_HZ 40.0f
static const int SPAN_HOPS[3]={1,2,4};
static const char *const PALETTE_NAME[PALETTES]={"RAINBOW","FIRE","NEON","ICE","PHOSPHOR"};
static const uint8_t white=TUI_ATTR(TUI_WHITE,TUI_BLACK), cyan=TUI_ATTR(TUI_CYAN,TUI_BLACK), ghost=TUI_ATTR(LS_FAINT_FG,TUI_BLACK);
static uint8_t ramp(int level) { return ls_audio_view_color(palette,level); }
static float scan_top_hz(void) { ls_music_format_t f; float hi=ls_music_format(&f)?f.rate_hz*.45f:16000; return hi>20000?20000:hi; }
static void freq_text(char *out,size_t n,float hz) { if(hz>=1000) snprintf(out,n,"%.1fk",(double)hz/1000); else snprintf(out,n,"%.0f",(double)hz); }


/* Colour ramps, sixteen levels from nothing to loudest. */

static void volume_to_step(int direction) { ls_media_volume_step(direction); }
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
    ls_audio_view_reset();
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
static void enter(void) {
  selected = top = 0;
  current = -1;
  focus = -1;
  library = false;
  frame = 0;
  ls_audio_view_open();
  ready = ls_music_open();
  if (ready)
    ls_music_scan(source);
}
static void leave(void) {
  ls_music_close();
  ls_audio_view_close();
  ready = false;
  current = -1;
}

/* Fast attack, steady release; the held peak waits, then falls. */
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
    ls_audio_view_draw(sf, body, view, palette, span_step, playing);
    snprintf(line, sizeof(line), "Source L/R before downmix and volume");
    break;
  case VIEW_BANDS:
    ls_audio_view_draw(sf, body, view, palette, span_step, playing);
    if (rate)
      snprintf(line, sizeof(line), "FFT %d HANN  %.1f Hz/BIN  %s",
               LS_MUSIC_FFT_SIZE, (double)rate / LS_MUSIC_FFT_SIZE, source_name());
    else
      snprintf(line, sizeof(line), "FFT %d HANN  %s", LS_MUSIC_FFT_SIZE,
               source_name());

    break;
  case VIEW_SCAN: {
    ls_audio_view_draw(sf, plot, view, palette, span_step, playing);
    strip(sf, controls_row, false, SCAN_FLOOR_DB, SCAN_TOP_DB);
    char lo[8], hi[8];
    freq_text(lo, sizeof(lo), SCAN_LO_HZ);
    freq_text(hi, sizeof(hi), scan_top_hz());
    snprintf(line, sizeof(line), "%s - %s Hz LOG  FFT %d  %s", lo, hi,
             LS_MUSIC_FFT_SIZE, source_name());
    break;
  }
  default: {
    ls_audio_view_draw(sf, plot, view, palette, span_step, playing);
    strip(sf, controls_row, true, WF_FLOOR_DB, WF_TOP_DB);
    char hi[8];
    freq_text(hi, sizeof(hi), rate ? rate / 2.0f : 22050.0f);
    snprintf(line, sizeof(line), "0 - %s Hz LINEAR  PAUSE TO HOLD THE PICTURE",
             hi);
    break;
  }
  }
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
  progress_rect=tui_rect_make(r.x,r.y+6,r.w,2);
  if(mic!=LS_MIC_OFF) {
    char clock[48];if(mic==LS_MIC_RECORDING)snprintf(clock,sizeof(clock),"%02u:%02u RECORDED",elapsed/60,elapsed%60);
    else snprintf(clock,sizeof(clock),"LIVE");
    ls_safe_line(sf,r,r.y+6,clock,white);
  }else ls_media_clock(sf,progress_rect,elapsed*1000,total*1000);
  int count = ls_music_count();
  if (count > 0) {
    char n[32];
    int len = snprintf(n, sizeof(n), "TRACK %d/%d",
                       (current >= 0 ? current : selected) + 1, count);
    tui_put_str(sf, r, r.x + r.w - 2 - len, r.y + 6, n, LS_ATTR_DIM);
  }
  ls_media_progress_bar(sf,progress_rect,elapsed*1000,total*1000);
}
/* Transport, volume readout and volume buttons: 2h+3 rows. */
static void transport_button(tui_surface *sf,void *ctx,int id,tui_rect r,const char *label,bool on,bool can) {
  (void)ctx; button(sf,PREV+id,r,label,on,can);
}
static void controls(tui_surface *sf,tui_rect r,bool playing,int h) {
  ls_media_transport(sf,r,playing,h,ready&&ls_music_count()>0,ls_music_volume(),ls_music_volume_db(),transport_button,NULL);
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
  top = ls_media_top(selected, count, visible_rows, top);
  if (!count)
    ls_safe_line(sf, rows_rect, rows_rect.y, "No WAV tracks. Use RESCAN.",
                 LS_ATTR_DIM);
  for (int i = 0; i < visible_rows && top + i < count; i++) {
    int index = top + i, y = rows_rect.y + i * row_height;
    tui_rect row = tui_rect_make(rows_rect.x, y, rows_rect.w, row_height - 1);
    const char *cols[]={ls_music_name(index)}; int widths[]={row.w-2};
    ls_media_row(sf,tui_rect_make(row.x+1,y+1,row.w-2,1),cols,widths,1,index==selected);
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
  case '[':
  case ']': {
    int64_t target=(int64_t)ls_music_position_ms()+(ch=='['?-5000:5000);
    if(target<0)target=0;
    if(target>ls_music_duration_ms())target=ls_music_duration_ms();
    if(ls_music_seek_ms((uint32_t)target))ls_audio_view_reset();
    break;
  }
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
  if (!library && current>=0 && y==progress_rect.y+1 && x>=progress_rect.x+2 && x<progress_rect.x+progress_rect.w-2) {
    if(ls_music_seek_ms((uint64_t)(x-progress_rect.x-2)*ls_music_duration_ms()/(progress_rect.w-4)))ls_audio_view_reset();
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
