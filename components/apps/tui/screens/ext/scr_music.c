/* Separate player/library views. The player's instrument panel shows one
   of four readings of the audio: stereo peak meters of what is heard, a
   2/3-octave band display, a fine scan of thin bars, and a pixel
   spectrogram for finding what is in a recording. Landscape gives the
   panel most of the width, portrait most of the height.

   The chrome is the house style: framed tiles on a dithered field in the
   colour of what they do, boxed panels with their title in the border, and
   the transport drawn as pictures when a key is tall enough to carry one. */
#include "../../ls_music_backend.h"
#include "../../ls_audio_view.h"
#include "../../ls_icons.h"
#include "../../ls_media_widgets.h"
#include "../../ls_music_transport.h"
#include "../../ls_options.h"
#include "../../ls_spectro.h"
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
  STOP,
  REPAIR,
  HOLD,
  AXIS,
  ACTIONS
};
/* The key each control answers to, drawn on it while a keyboard is fitted. */
static const char *const KEYS[ACTIONS] = {
    "B", "P", "N", "-", "M", "+", "L", "I", "W", "1", "2", "3", "4",
    "C", "Z", "E", "D", "R", "ESC", "U", "J", "O", "S", "", "H", "A"};
enum { VIEW_LEVEL, VIEW_BANDS, VIEW_SCAN, VIEW_WATERFALL, VIEWS };
static const char *const VIEW_NAME[VIEWS] = {"LEVEL", "BANDS", "SCAN",
                                             "WATERFALL"};
static const char *const VIEW_SHORT[VIEWS] = {"LVL", "BND", "SCN", "WF"};
static const char *const VIEW_TITLE[VIEWS] = {
    " PEAK LEVEL ", " SPECTRUM 2/3 OCTAVE ", " SCAN ", " WATERFALL "};
/* Each reading has its colour: its tab, and the frame of its panel. */
static const uint8_t VIEW_HUE[VIEWS] = {TUI_GREEN, TUI_YELLOW, TUI_MAGENTA,
                                        TUI_CYAN};
/* Screen state that only the UI task touches lives in PSRAM: internal RAM
   is what boot runs out of, and the radio's endpoint needs it. */
EXT_RAM_BSS_ATTR static tui_rect hits[ACTIONS];
EXT_RAM_BSS_ATTR static bool enabled[ACTIONS];
static tui_rect rows_rect, progress_rect;
static bool ready, library;
static int selected, current = -1, top, source, focus = -1, row_height = 3,
                     visible_rows;
/* Kept across visits and rotations: the panel is a preference. */
static int view = VIEW_LEVEL, palette, span_step;
static unsigned frame;
#define PALETTES 5
#define SCAN_FLOOR_DB (-84.0f)
#define SCAN_TOP_DB (-12.0f)
#define SCAN_LO_HZ 40.0f
static const char *const PALETTE_NAME[PALETTES]={"RAINBOW","FIRE","NEON","ICE","PHOSPHOR"};
static const uint8_t PALETTE_HUE[PALETTES]={TUI_MAGENTA,TUI_RED,TUI_MAGENTA,TUI_CYAN,TUI_GREEN};
/* The waterfall's reading, kept like the view: floor and range of the
   colour map, the axis, how far up it reaches, and HOLD. */
static const char *const AXIS_NAME[2]={"LINEAR","LOG"};
static const int TOP_DIV[4]={1,2,4,8};
static int wf_floor=-100, wf_range=80, wf_axis, wf_top;
static bool wf_hold;
static const uint8_t white=TUI_ATTR(TUI_WHITE,TUI_BLACK), cyan=TUI_ATTR(TUI_CYAN,TUI_BLACK), ghost=TUI_ATTR(LS_FAINT_FG,TUI_BLACK);
static uint8_t ramp(int level) { return ls_audio_view_color(palette,level); }
static float scan_top_hz(void) { ls_music_format_t f; float hi=ls_music_format(&f)?f.rate_hz*.45f:16000; return hi>20000?20000:hi; }
static void freq_text(char *out,size_t n,float hz) { if(hz>=1000) snprintf(out,n,"%.1fk",(double)hz/1000); else snprintf(out,n,"%.0f",(double)hz); }
static unsigned rate_now(void) { ls_music_format_t f; return ls_music_format(&f)&&f.rate_hz?f.rate_hz:44100; }
/* Milliseconds of audio in one waterfall pixel column. */
static double span_ms(void) { return ls_audio_view_hop(span_step)*1000.0/rate_now(); }
static void span_text(char *out,size_t n) { snprintf(out,n,"%.1f ms",span_ms()); }
static void push_waterfall(void) {
  ls_audio_wf_opts_t o={(float)wf_floor,(float)wf_range,wf_axis==1,wf_hold,TOP_DIV[wf_top]};
  ls_audio_view_waterfall(&o);
}

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
static void select_recording(void) {
  if(!ready)return;
  const char *path = ls_music_mic_file(), *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  if(!ls_music_scan(0))return;
  source=0;
  current = -1;
  for (int i = 0; i < ls_music_count(); i++)
    if (!strcmp(ls_music_name(i), name))
      selected = i;
}
static void mic_stopped(void) {
  bool recorded=ls_music_mic_state()==LS_MIC_RECORDING;
  ls_music_mic_stop();
  if(recorded && ls_music_mic_state()==LS_MIC_OFF)select_recording();
}
/* OPTIONS: how the player looks and sounds, each row doing what its own
   control does - the view and colour tabs, SPAN, TONE and SOURCE - and the
   waterfall's own reading one level down. */
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
  case SPAN:
    snprintf(out, n, "%.1f ms/PX", span_ms());
    break;
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
enum { WF_O_AXIS = 100, WF_O_TOP, WF_O_FLOOR, WF_O_RANGE, WF_O_HOLD };
static int o_get(const ls_opt_t *o) {
  switch(o->arg){
  case TAB_LEVEL: return view;
  case COLOUR: return palette;
  case WF_O_AXIS: return wf_axis;
  case WF_O_TOP: return wf_top;
  case WF_O_HOLD: return wf_hold;
  default: return span_step;
  }
}
static void o_set(const ls_opt_t *o,int v) {
  switch(o->arg){
  case TAB_LEVEL: view=v; break;
  case COLOUR: palette=v; break;
  case WF_O_AXIS: wf_axis=v&1; break;
  case WF_O_TOP: wf_top=v<0?0:v>3?3:v; break;
  case WF_O_HOLD: wf_hold=v!=0; break;
  default: span_step=v;ls_audio_view_reset(); break;
  }
}
static double o_num(const ls_opt_t *o) {
  return o->arg==WF_O_FLOOR?wf_floor:o->arg==WF_O_RANGE?wf_range:o_get(o);
}
static void o_set_num(const ls_opt_t *o,double v) {
  if(o->arg==WF_O_FLOOR)wf_floor=(int)lround(v<-130?-130:v>-40?-40:v);
  else if(o->arg==WF_O_RANGE)wf_range=(int)lround(v<20?20:v>110?110:v);
  else o_set(o,(int)v);
}
/* How far up the axis reaches, in kHz of the track playing now. */
static void top_show(const ls_opt_t *o, char *out, size_t n) {
  (void)o;
  char k[12];
  ls_spectro_khz(k,sizeof(k),rate_now()/2.0f/TOP_DIV[wf_top]);
  snprintf(out,n,"%s%s",k,wf_top?"":" FULL");
}
static const char *const TOP_NAME[4]={"FULL","1/2","1/4","1/8"};
static const char *const HOLD_NAME[2]={"RUNNING","HELD"};
static const ls_opt_t OPT_WATERFALL[] = {
    {.label="HOLD",.kind=LS_OPT_TOGGLE,.arg=WF_O_HOLD,.names=HOLD_NAME,.get=o_get,.set=o_set},
    {.label="AXIS",.kind=LS_OPT_CYCLE,.arg=WF_O_AXIS,.names=AXIS_NAME,.n=2,.step=1,.get=o_get,.set=o_set},
    {.label="TOP",.kind=LS_OPT_CYCLE,.arg=WF_O_TOP,.names=TOP_NAME,.n=4,.step=1,.get=o_get,.set=o_set,.show=top_show},
    {.label="FLOOR",.kind=LS_OPT_LEVEL,.arg=WF_O_FLOOR,.lo=-130,.hi=-40,.step=5,.unit="dB, the colour map's black",.num=o_num,.set_num=o_set_num},
    {.label="RANGE",.kind=LS_OPT_LEVEL,.arg=WF_O_RANGE,.lo=20,.hi=110,.step=5,.unit="dB from black to white",.num=o_num,.set_num=o_set_num},
};
static const ls_opt_ctx_t CTX_WATERFALL = {.name = "WATERFALL", .job = -1,
                                           .radio = LS_RSEL_NONE,
                                           LS_OPT_ROWS(OPT_WATERFALL)};
#define MUSIC_ROW(l, id)                                                       \
  { .label = (l), .kind = LS_OPT_ACTION, .arg = (id), .act = o_act, .show = o_show }
static const ls_opt_t OPT_MUSIC[] = {
    {.label="VIEW",.kind=LS_OPT_CYCLE,.arg=TAB_LEVEL,.names=VIEW_NAME,.n=VIEWS,.step=1,.get=o_get,.set=o_set},
    {.label="COLOUR",.kind=LS_OPT_CYCLE,.arg=COLOUR,.names=PALETTE_NAME,.n=PALETTES,.step=1,.get=o_get,.set=o_set},
    {.label="SPAN",.kind=LS_OPT_LEVEL,.arg=SPAN,.lo=0,.hi=2,.step=1,.num=o_num,.set_num=o_set_num,.show=o_show},
    {.label="WATERFALL",.kind=LS_OPT_MENU,.sub=&CTX_WATERFALL},
    MUSIC_ROW("TONE", TONE),
    MUSIC_ROW("SOURCE", SOURCE), MUSIC_ROW("STOP", STOP),
    MUSIC_ROW("REPAIR WAV", REPAIR),
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
    ls_music_mute_toggle();
    break;
  case STOP:
    mic_stopped();
    ls_music_stop();
    current=-1;
    break;
  case REPAIR:
    if(ls_music_mic_repair())select_recording();
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
  case HOLD:
    wf_hold = !wf_hold;
    break;
  case AXIS:
    wf_axis ^= 1;
    break;
  case BACK:
    library = false;
    focus = -1;
    break;
  case TONE:
    ls_music_set_warm(!ls_music_warm());
    break;
  case SOURCE:
  case RESCAN: {
    int target=id==SOURCE?1-source:source;
    ready = ls_music_open();
    if (ready && ls_music_scan(target)) {
      source=target;
      selected = top = 0;
      current = -1;
    }
    break;
  }
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

/* Writes text at *x and moves *x past it: for lines in several colours. */
static void seg(tui_surface *sf, tui_rect clip, int *x, int y, const char *text,
                uint8_t attr) {
  tui_put_str(sf, clip, *x, y, text, attr);
  *x += (int)strlen(text);
}

/* --------------------------------------------------------- the pictures -- */

/* A bitmap drawn in sextants, two pixels across and three down a cell:
   '#' is ink. Cells with no ink are a black plate, so the picture reads on
   the dithered field the way the lettering does. */
static void art(tui_surface *sf, tui_rect clip, int x, int y,
                const char *const *rows, int h_px, uint8_t ink) {
  int w_px = (int)strlen(rows[0]);
  for (int cy = 0; cy * 3 < h_px; cy++)
    for (int cx = 0; cx * 2 < w_px; cx++) {
      int bits = 0;
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 2; c++) {
          int px = cx * 2 + c, py = cy * 3 + r;
          if (py < h_px && px < w_px && rows[py][px] == '#')
            bits |= 1 << (r * 2 + c);
        }
      char g = bits == 0x3F ? LS_TUI_BLOCK_FULL : bits ? LS_TUI_SEXT(bits) : ' ';
      tui_put_char(sf, clip, x + cx, y + cy, g, ink);
    }
}
static const char *const ART_PLAY[9] = {"#.......","###.....","#####...","#######.","########","#######.","#####...","###.....","#......."};
static const char *const ART_PAUSE[9] = {"###..###","###..###","###..###","###..###","###..###","###..###","###..###","###..###","###..###"};
static const char *const ART_NEXT[9] = {"#....#....##","##...##...##","###..###..##","####.####.##","############","####.####.##","###..###..##","##...##...##","#....#....##"};
static const char *const ART_PREV[9] = {"##....#....#","##...##...##","##..###..###","##.####.####","############","##.####.####","##..###..###","##...##...##","##....#....#"};
static const char *const ART_QUIETER[9] = {"....#.......","...##.......","#####.......","#####.......","#####..#####","#####.......","#####.......","...##.......","....#......."};
static const char *const ART_LOUDER[9] = {"....#.......","...##.......","#####....#..","#####....#..","#####..#####","#####....#..","#####....#..","...##.......","....#......."};
static const char *const ART_MUTE[9] = {"....#.......","...##.......","#####..#...#","#####...#.#.","#####....#..","#####...#.#.","#####..#...#","...##.......","....#......."};
static const char *const *button_art(int id) {
  switch (id) {
  case PREV: return ART_PREV;
  case PLAY: return ls_music_state() == LS_MUSIC_PLAYING ? ART_PAUSE : ART_PLAY;
  case NEXT: return ART_NEXT;
  case QUIETER: return ART_QUIETER;
  case SILENCE: return ART_MUTE;
  case LOUDER: return ART_LOUDER;
  default: return NULL;
  }
}
/* One cell of the same picture beside a label on a short key. */
static int small_art(int id, char out[3]) {
  switch (id) {
  case PREV: out[0] = out[1] = LS_TUI_SEXT(0x2E); return 2;
  case NEXT: out[0] = out[1] = LS_TUI_SEXT(0x1D); return 2;
  case PLAY:
    if (ls_music_state() == LS_MUSIC_PLAYING) { out[0] = out[1] = LS_TUI_SEXT(0x15); return 2; }
    out[0] = LS_TUI_SEXT(0x1D); return 1;
  default: return 0;
  }
}

/* What a control does decides its colour; the lit form says it is on. */
static uint8_t role_hue(int id) {
  switch (id) {
  case PLAY: case RESCAN: return TUI_GREEN;
  case QUIETER: case LOUDER: case TONE: case HOLD: return TUI_YELLOW;
  case SILENCE: case REC: return TUI_RED;
  case MIC: return TUI_MAGENTA;
  case COLOUR: return PALETTE_HUE[palette];
  case TAB_LEVEL: case TAB_BANDS: case TAB_SCAN: case TAB_WATERFALL:
    return VIEW_HUE[id - TAB_LEVEL];
  default: return TUI_CYAN;
  }
}
/* A framed tile on a dithered field: light when it is there to press,
   heavy when it is on, cyan and heavy with the keyboard's focus on it. */
static void button(tui_surface *sf, int id, tui_rect r, const char *label,
                   bool on, bool available) {
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
  const bool sel = id == focus;
  uint8_t hue = role_hue(id);
  int density = LS_DITHER_LIGHT;
  if (!available)
    hue = LS_FAINT_FG;
  else if (sel)
    hue = TUI_CYAN | TUI_BRIGHT, density = LS_DITHER_HEAVY;
  else if (on)
    hue |= TUI_BRIGHT, density = LS_DITHER_HEAVY;
  ls_fill_dither(sf, r, density, hue);
  const bool tall = r.h >= 5;
  const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
  for (int x = r.x + 1; x < r.x + r.w - 1; x++) {
    tui_put_char(sf, r, x, r.y, tall ? '=' : '-', edge);
    tui_put_char(sf, r, x, r.y + r.h - 1, tall ? '=' : '-', edge);
  }
  for (int y = r.y + 1; y < r.y + r.h - 1; y++) {
    tui_put_char(sf, r, r.x, y, '|', edge);
    tui_put_char(sf, r, r.x + r.w - 1, y, '|', edge);
  }
  tui_put_char(sf, r, r.x, r.y, '+', edge);
  tui_put_char(sf, r, r.x + r.w - 1, r.y, '+', edge);
  tui_put_char(sf, r, r.x, r.y + r.h - 1, '+', edge);
  tui_put_char(sf, r, r.x + r.w - 1, r.y + r.h - 1, '+', edge);
  const uint8_t face =
      available ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM;
  const uint8_t ink = TUI_ATTR(available ? (role_hue(id) | TUI_BRIGHT) : LS_FAINT_FG,
                               TUI_BLACK);
  const int inner_y = r.y + 1, inner_h = r.h - 2;
  int label_y = inner_y + (inner_h - 1) / 2;
  const char *const *pic = button_art(id);
  if (pic && inner_h >= 4 && r.w >= 9) {
    /* The picture, then the word under it, centred together. */
    int block = inner_h >= 5 ? 5 : 4, y0 = inner_y + (inner_h - block) / 2;
    int cells = ((int)strlen(pic[0]) + 1) / 2, ax = r.x + (r.w - cells) / 2;
    /* On a black plate a cell wider than the picture, so it reads on a lit
       key as well as a dark one. */
    tui_fill(sf, tui_rect_make(ax - 1, y0, cells + 2, 3), ' ', ink);
    art(sf, r, ax, y0, pic, 9, ink);
    label_y = y0 + block - 1;
  }
  if (label) {
    int n = (int)strlen(label);
    if (n > r.w - 2)
      n = r.w - 2;
    char g[3] = {0};
    int gn = pic && inner_h < 4 ? small_art(id, g) : 0;
    if (gn && n + gn + 1 > r.w - 4)
      gn = 0;
    int lx = r.x + (r.w - (n + (gn ? gn + 1 : 0))) / 2;
    if (gn) {
      if (lx - 1 > r.x) tui_put_char(sf, r, lx - 1, label_y, ' ', ink);
      for (int i = 0; i < gn; i++) tui_put_char(sf, r, lx + i, label_y, g[i], ink);
      lx += gn;
    }
    char cut[48];
    snprintf(cut, sizeof(cut), " %.*s ", n, label);
    /* A space either side on black: the field around it is texture. */
    tui_rect inside = tui_rect_make(r.x + 1, r.y, r.w - 2, r.h);
    tui_put_str(sf, inside, lx - (gn ? 0 : 1), label_y, cut, face);
  }
  if (tall && on && available && r.w >= 7)
    tui_put_str(sf, r, r.x + (r.w - 3) / 2, r.y + r.h - 1, "[*]", edge);
  if (sel)
    tui_put_char(sf, r, r.x, label_y, '>', face);
  if (ls_tui_keyboard_mode() && *KEYS[id]) {
    char badge[8];
    int n = snprintf(badge, sizeof(badge), "[%s]", KEYS[id]);
    if (n + 2 < r.w)
      tui_put_str(sf, r, r.x + r.w - 1 - n, r.y, badge,
                  TUI_ATTR(available ? TUI_YELLOW | TUI_BRIGHT : TUI_WHITE, TUI_BLACK));
  }
}

/* --------------------------------------------------------- the panel -- */

static void strip(tui_surface *sf, tui_rect r, bool waterfall, float lo_db,
                  float hi_db) {
  int bw = r.w >= 60 ? 12 : 10;
  int right = r.x + r.w;
  button(sf, COLOUR, tui_rect_make(right - bw, r.y, bw, 3),
         PALETTE_NAME[palette], false, true);
  right -= bw + 1;
  if (waterfall) {
    char label[24];
    span_text(label, sizeof(label));
    button(sf, SPAN, tui_rect_make(right - bw, r.y, bw, 3), label, false, true);
    right -= bw + 1;
    button(sf, HOLD, tui_rect_make(right - 8, r.y, 8, 3), wf_hold ? "HELD" : "HOLD",
           wf_hold, true);
    right -= 9;
    /* AXIS lives in OPTIONS and on A; it gets a key here when there is room
       for it and the colour key both. */
    if (right - r.x >= 18 + 7) {
      button(sf, AXIS, tui_rect_make(right - 7, r.y, 7, 3), wf_axis ? "LOG" : "LIN",
             false, true);
      right -= 8;
    }
  }
  if (right - r.x < 17)
    return;
  for (int i = 0; i < 16; i++)
    tui_put_char(sf, r, r.x + 1 + i, r.y + 1, LS_TUI_BLOCK_FULL,
                 TUI_ATTR(ramp(i), TUI_BLACK));
  char text[16];
  snprintf(text, sizeof(text), "%.0f", (double)lo_db);
  tui_put_str(sf, r, r.x + 1, r.y + 2, text, LS_ATTR_DIM);
  snprintf(text, sizeof(text), "%.0f dB", (double)hi_db);
  tui_put_str(sf, r, r.x + 17 - (int)strlen(text), r.y + 2, text,
              LS_ATTR_DIM);
  tui_put_str(sf, r, r.x + 1, r.y, waterfall ? "COLOUR MAP" : "LEVEL KEY", cyan);
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
/* The format of what is playing, values bright and units dim. */
static void format_line(tui_surface *sf, tui_rect clip, int x, int y) {
  ls_music_format_t f = {0};
  if (!ls_music_format(&f)) {
    tui_put_str(sf, clip, x, y, "NO SIGNAL", ghost);
    return;
  }
  char v[16];
  const uint8_t val = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
  snprintf(v, sizeof(v), "%u.%u", f.rate_hz / 1000, f.rate_hz % 1000 / 100);
  seg(sf, clip, &x, y, v, val);
  seg(sf, clip, &x, y, " kHz  ", LS_ATTR_DIM);
  snprintf(v, sizeof(v), "%u", f.bits);
  seg(sf, clip, &x, y, v, val);
  seg(sf, clip, &x, y, "-BIT  ", LS_ATTR_DIM);
  seg(sf, clip, &x, y, f.channels > 1 ? "STEREO" : "MONO", white);
}
static void output_line(tui_surface *sf, tui_rect clip, int x, int y) {
  ls_music_format_t f = {0};
  ls_music_format(&f);
  seg(sf, clip, &x, y, "OUT ", LS_ATTR_DIM);
  seg(sf, clip, &x, y, f.mono_out ? "MONO" : "STEREO", white);
  seg(sf, clip, &x, y, "  TONE ", LS_ATTR_DIM);
  seg(sf, clip, &x, y, ls_music_warm() ? "WARM" : "FLAT",
      ls_music_warm() ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : white);
}
static void panel(tui_surface *sf, tui_rect p, bool playing, bool boxed) {
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
  ls_panel_box(sf, p, VIEW_TITLE[view], VIEW_HUE[view]);
  tui_rect in = tui_rect_make(p.x + 1, p.y + 1, p.w - 2, p.h - 2);
  /* Tabs beside the format lines when there is room, above them when not;
     with the format in the now-playing box above, the tabs have the row. */
  const int info_w = boxed ? 0 : 25;
  bool beside = boxed || in.w - info_w >= VIEWS * 11;
  int tabs_w = beside ? in.w - info_w : in.w;
  if (tabs_w > VIEWS * 12 && !boxed)
    tabs_w = VIEWS * 12;
  tabs(sf, tui_rect_make(in.x + in.w - tabs_w, in.y, tabs_w, 3));
  if (!boxed) {
    int info_y = beside ? in.y : in.y + 3;
    format_line(sf, in, in.x + 1, info_y);
    output_line(sf, in, in.x + 1, info_y + 1);
  }
  int body_y = beside ? in.y + 3 : in.y + 6;
  tui_rect body = tui_rect_make(in.x, body_y, in.w, in.y + in.h - 1 - body_y);
  tui_rect plot = tui_rect_make(body.x, body.y, body.w, body.h - 3);
  tui_rect controls_row = tui_rect_make(body.x, body.y + body.h - 3, body.w, 3);
  char line[96];
  unsigned rate = rate_now();
  switch (view) {
  case VIEW_LEVEL:
    ls_audio_view_draw(sf, body, view, palette, span_step, playing);
    snprintf(line, sizeof(line), "OUTPUT dBFS AFTER VOLUME  <> SOURCE BEFORE IT");
    break;
  case VIEW_BANDS:
    ls_audio_view_draw(sf, body, view, palette, span_step, playing);
    snprintf(line, sizeof(line), "FFT %d HANN  %.1f Hz/BIN  %s",
             LS_MUSIC_FFT_SIZE, (double)rate / LS_MUSIC_FFT_SIZE, source_name());
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
    push_waterfall();
    ls_audio_view_draw(sf, plot, view, palette, span_step, playing);
    strip(sf, controls_row, true, (float)wf_floor, (float)(wf_floor + wf_range));
    char hi[12];
    ls_spectro_khz(hi, sizeof(hi), rate / 2.0f / TOP_DIV[wf_top]);
    snprintf(line, sizeof(line), "FFT %d  %.1f Hz/BIN  %.0f LINES/S  %s 0-%s",
             LS_MUSIC_FFT_SIZE, (double)rate / LS_MUSIC_FFT_SIZE,
             (double)rate / ls_audio_view_hop(span_step),
             wf_axis ? "LOG" : "LIN", hi);
    break;
  }
  }
  ls_safe_line(sf, in, in.y + in.h - 1, line, LS_ATTR_DIM);
}

/* ------------------------------------------------------ now playing -- */

static void title_text(char *text, size_t n, const char **artist,
                       const char **song) {
  snprintf(text, n, "%s", ls_music_name(current >= 0 ? current : selected));
  char *dot = strrchr(text, '.');
  if (dot)
    *dot = 0;
  char *split = strstr(text, " - ");
  *artist = "SD MUSIC";
  *song = text;
  if (split) {
    *split = 0;
    *artist = text;
    *song = split + 3;
  }
}
/* A title too long for its line scrolls while it plays. */
static const char *scrolled(const char *song, int width) {
  int overflow = (int)strlen(song) - width;
  if (overflow > 0 && ls_music_state() == LS_MUSIC_PLAYING) {
    unsigned phase = (frame / 8) % (unsigned)(overflow + 14);
    song += phase > 7 ? (phase - 7 > (unsigned)overflow ? (unsigned)overflow
                                                        : phase - 7)
                      : 0;
  }
  return song;
}
static const char *state_name(bool playing) {
  ls_music_mic_t mic = ls_music_mic_state();
  return mic == LS_MIC_RECORDING               ? "RECORDING"
         : mic == LS_MIC_LIVE                  ? "LISTENING"
         : playing                             ? "PLAYING"
         : ls_music_state() == LS_MUSIC_PAUSED ? "PAUSED"
                                               : "READY";
}
static uint8_t state_hue(bool playing) {
  ls_music_mic_t mic = ls_music_mic_state();
  return mic == LS_MIC_RECORDING               ? TUI_RED
         : mic == LS_MIC_LIVE                  ? TUI_MAGENTA
         : playing                             ? TUI_GREEN
         : ls_music_state() == LS_MUSIC_PAUSED ? TUI_YELLOW
                                               : TUI_CYAN;
}
/* Listen, record, the track list and OPTIONS, in a row of tiles. */
static void top_keys(tui_surface *sf, int x, int y, int bw, int h) {
  ls_music_mic_t mic = ls_music_mic_state();
  button(sf, MIC, tui_rect_make(x, y, bw, h), "MIC", mic == LS_MIC_LIVE, ready);
  button(sf, REC, tui_rect_make(x + bw + 1, y, bw, h), "REC",
         mic == LS_MIC_RECORDING, ready);
  button(sf, LIBRARY, tui_rect_make(x + 2 * (bw + 1), y, bw, h),
         bw >= 8 ? "TRACKS" : "LIST", false, true);
  button(sf, OPTIONS, tui_rect_make(x + 3 * (bw + 1), y, bw, h),
         bw >= 9 ? "OPTIONS" : "OPT", false, true);
}
static void clock_text(char *out, size_t n, uint32_t elapsed_ms,
                       uint32_t total_ms) {
  snprintf(out, n, "%02u:%02u / %02u:%02u", (unsigned)(elapsed_ms / 60000),
           (unsigned)(elapsed_ms / 1000 % 60), (unsigned)(total_ms / 60000),
           (unsigned)(total_ms / 1000 % 60));
}
/* The roomy form: a box titled with the state, the cassette in its colour,
   and the track beside it. Eleven rows with the keys under it. */
static void info_boxed(tui_surface *sf, tui_rect r, bool playing) {
  int pad = 0;
  for (int y = r.y; y < r.y + 11; y++)
    if (ls_tui_corner_pad(y) > pad)
      pad = ls_tui_corner_pad(y);
  int cols = tui_surface_rect(sf).w;
  if (r.x < pad) {
    r.w -= pad - r.x;
    r.x = pad;
  }
  if (r.x + r.w > cols - pad)
    r.w = cols - pad - r.x;
  const uint8_t hue = state_hue(playing);
  tui_rect box = tui_rect_make(r.x, r.y, r.w, 8);
  char title[24];
  snprintf(title, sizeof(title), " %s ", state_name(playing));
  ls_panel_box(sf, box, title, hue);
  int count = ls_music_count();
  if (count > 0) {
    char n[32];
    int len = snprintf(n, sizeof(n), " TRACK %d/%d ",
                       (current >= 0 ? current : selected) + 1, count);
    tui_put_str(sf, box, box.x + box.w - 2 - len, box.y, n,
                TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
  }
  ls_icon_draw(sf, box, box.x + 2, box.y + 1, LS_ICON_MUSIC,
               TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
  const int tx = box.x + 2 + LS_ICON_COLS + 2, tw = box.x + box.w - 2 - tx;
  tui_rect text = tui_rect_make(tx, box.y + 1, tw, 6);
  ls_music_mic_t mic = ls_music_mic_state();
  const char *error = ls_music_error();
  unsigned elapsed, total;
  if (mic != LS_MIC_OFF) {
    const char *path = ls_music_mic_file(), *name = strrchr(path, '/');
    tui_put_str(sf, text, tx, text.y, "MICROPHONE",
                TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, text, tx, text.y + 1,
                mic == LS_MIC_RECORDING ? (name ? name + 1 : path)
                                        : "Live input, not recorded",
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    elapsed = mic == LS_MIC_RECORDING ? ls_music_mic_ms() / 1000 : 0;
    total = 0;
  } else {
    char name[256];
    const char *artist, *song;
    title_text(name, sizeof(name), &artist, &song);
    tui_put_str(sf, text, tx, text.y, artist, cyan);
    tui_put_str(sf, text, tx, text.y + 1, *song ? scrolled(song, tw) : "Choose a track",
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    elapsed = current >= 0 ? ls_music_position_ms() / 1000 : 0;
    total = current >= 0 ? ls_music_duration_ms() / 1000 : 0;
  }
  format_line(sf, text, tx, text.y + 2);
  if (*error)
    tui_put_str(sf, text, tx, text.y + 3, error, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
  else
    output_line(sf, text, tx, text.y + 3);
  char clock[48];
  if (mic == LS_MIC_RECORDING)
    snprintf(clock, sizeof(clock), "%02u:%02u RECORDED", elapsed / 60, elapsed % 60);
  else if (mic == LS_MIC_LIVE)
    snprintf(clock, sizeof(clock), "LIVE");
  else
    clock_text(clock, sizeof(clock), elapsed * 1000, total * 1000);
  tui_put_str(sf, text, tx, text.y + 4, clock, white);
  /* The seek bar: the touch handler reads it as progress_rect's second row,
     inset two columns at each end. */
  progress_rect = tui_rect_make(tx - 2, text.y + 4, tw + 4, 2);
  if (mic == LS_MIC_OFF)
    ls_media_progress_bar(sf, progress_rect, elapsed * 1000, total * 1000);
  int bw = (r.w - 3) / 4;
  top_keys(sf, r.x + (r.w - (4 * bw + 3)) / 2, r.y + 8, bw, 3);
}
/* The tight form, for a narrow side pane: status, title, clock and
   progress in eight rows, the keys across the top. */
static void info_flat(tui_surface *sf, tui_rect r, bool playing) {
  ls_music_mic_t mic = ls_music_mic_state();
  const int keys = 4;
  int bw = (r.w - 3) / 4;
  if (bw > 9)
    bw = 9;
  int x = r.x + r.w - keys * bw - (keys - 1);
  tui_rect left = tui_rect_make(r.x, r.y, x - r.x, r.h);
  const uint8_t hue = state_hue(playing);
  ls_safe_line(sf, left, r.y + 1, state_name(playing),
               mic == LS_MIC_RECORDING ? TUI_ATTR(TUI_BLACK, TUI_RED | TUI_BRIGHT)
                                       : TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
  top_keys(sf, x, r.y, bw, 3);
  unsigned elapsed, total;
  const char *error = ls_music_error();
  if (mic != LS_MIC_OFF) {
    const char *path = ls_music_mic_file(), *name = strrchr(path, '/');
    ls_safe_line(sf, r, r.y + 3, "MICROPHONE", TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
    ls_safe_line(sf, r, r.y + 4,
                 mic == LS_MIC_RECORDING ? (name ? name + 1 : path)
                                         : "Live input, not recorded",
                 white);
    elapsed = mic == LS_MIC_RECORDING ? ls_music_mic_ms() / 1000 : 0;
    total = 0;
  } else {
    char name[256];
    const char *artist, *song;
    title_text(name, sizeof(name), &artist, &song);
    ls_safe_line(sf, r, r.y + 3, artist, cyan);
    ls_safe_line(sf, r, r.y + 4, *song ? scrolled(song, r.w - 2) : "Choose a track",
                 TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    elapsed = current >= 0 ? ls_music_position_ms() / 1000 : 0;
    total = current >= 0 ? ls_music_duration_ms() / 1000 : 0;
  }
  if (*error)
    ls_safe_line(sf, r, r.y + 5, error, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
  progress_rect = tui_rect_make(r.x, r.y + 6, r.w, 2);
  if (mic != LS_MIC_OFF) {
    char clock[48];
    if (mic == LS_MIC_RECORDING)
      snprintf(clock, sizeof(clock), "%02u:%02u RECORDED", elapsed / 60, elapsed % 60);
    else
      snprintf(clock, sizeof(clock), "LIVE");
    ls_safe_line(sf, r, r.y + 6, clock, white);
  } else
    ls_media_clock(sf, progress_rect, elapsed * 1000, total * 1000);
  int count = ls_music_count();
  if (count > 0) {
    char n[32];
    int len = snprintf(n, sizeof(n), "TRACK %d/%d",
                       (current >= 0 ? current : selected) + 1, count);
    tui_put_str(sf, r, r.x + r.w - 2 - len, r.y + 6, n,
                TUI_ATTR(hue, TUI_BLACK));
  }
  ls_media_progress_bar(sf, progress_rect, elapsed * 1000, total * 1000);
}
/* Transport, volume readout and volume buttons: 2h+3 rows. */
static void transport_button(tui_surface *sf,void *ctx,int id,tui_rect r,const char *label,bool on,bool can) {
  (void)ctx;
  if(id==4){label=ls_music_muted()?"UNMUTE":"MUTE";on=ls_music_muted();}
  button(sf,PREV+id,r,label,on,can);
}
static void controls(tui_surface *sf,tui_rect r,bool playing,int h) {
  ls_media_transport(sf,r,playing,h,ready&&ls_music_count()>0,ls_music_volume(),ls_music_volume_db(),transport_button,NULL);
}

/* ------------------------------------------------------- the track list -- */

static void draw_library(tui_surface *sf, tui_rect r) {
  int count = ls_music_count();
  ls_safe_line(sf, r, r.y + 1, source ? "FLASH / MUSIC" : "SD / MUSIC",
               TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
  if (ls_tui_keyboard_mode())
    ls_safe_line(sf, tui_rect_make(r.x, r.y, r.w - 13, r.h), r.y + 2,
                 "UP/DOWN select  ENTER play  LEFT/RIGHT prev/next",
                 LS_ATTR_DIM);
  else {
    char n[32];
    snprintf(n, sizeof(n), "%d TRACK%s", count, count == 1 ? "" : "S");
    ls_safe_line(sf, tui_rect_make(r.x, r.y, r.w - 13, r.h), r.y + 2, n, LS_ATTR_DIM);
  }
  button(sf, BACK, tui_rect_make(r.x + r.w - 12, r.y, 12, 3), "BACK", false,
         true);
  row_height = r.h >= 40 ? 4 : 3;
  tui_rect box = tui_rect_make(r.x, r.y + 3, r.w, r.h - 10);
  ls_panel_box(sf, box, " TRACKS ", TUI_CYAN);
  rows_rect = tui_rect_make(r.x + 1, r.y + 4, r.w - 2, r.h - 12);
  visible_rows = rows_rect.h / row_height;
  if (visible_rows < 1)
    visible_rows = 1;
  top = ls_media_top(selected, count, visible_rows, top);
  if (count > visible_rows) {
    char n[24];
    int len = snprintf(n, sizeof(n), " %d-%d OF %d ", top + 1,
                       top + visible_rows > count ? count : top + visible_rows, count);
    tui_put_str(sf, box, box.x + box.w - 2 - len, box.y, n, cyan);
  }
  if (!count)
    ls_safe_line(sf, rows_rect, rows_rect.y + 1, "No WAV tracks. Use RESCAN.",
                 LS_ATTR_DIM);
  for (int i = 0; i < visible_rows && top + i < count; i++) {
    int index = top + i, y = rows_rect.y + i * row_height;
    tui_rect row = tui_rect_make(rows_rect.x, y, rows_rect.w, row_height - 1);
    const bool sel = index == selected, now = index == current;
    const int ty = y + (row_height - 1) / 2;
    /* The highlight is centred on the name: three rows when the rows are
       four tall, the name's own row when they are three. */
    if (sel)
      ls_fill_dither(sf, tui_rect_make(row.x, row_height >= 4 ? ty - 1 : ty, row.w,
                                       row_height >= 4 ? 3 : 1),
                     LS_DITHER_MEDIUM, TUI_CYAN);
    char num[8], name[96];
    snprintf(num, sizeof(num), "%3d", index + 1);
    snprintf(name, sizeof(name), "%s", ls_music_name(index));
    char *dot = strrchr(name, '.');
    if (dot)
      *dot = 0;
    const int tag = now ? 6 : 0;
    int nw = row.w - 8 - tag;
    if (nw < 1)
      nw = 1;
    if ((int)strlen(name) > nw)
      name[nw] = 0;
    int x = row.x + 1;
    char lead[2] = {sel ? '>' : ' ', 0};
    seg(sf, row, &x, ty, lead, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    seg(sf, row, &x, ty, num, TUI_ATTR(TUI_YELLOW, TUI_BLACK));
    seg(sf, row, &x, ty, "  ", TUI_ATTR(TUI_WHITE, TUI_BLACK));
    seg(sf, row, &x, ty, name, TUI_ATTR(sel || now ? TUI_WHITE | TUI_BRIGHT : TUI_WHITE, TUI_BLACK));
    seg(sf, row, &x, ty, " ", TUI_ATTR(TUI_WHITE, TUI_BLACK));
    if (now) {
      int tx = row.x + row.w - tag;
      tui_put_char(sf, row, tx, ty, ' ', TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
      tui_put_char(sf, row, tx + 1, ty, LS_TUI_SEXT(0x1D), TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
      tui_put_str(sf, row, tx + 2, ty, "NOW", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    }
  }
  int y = r.y + r.h - 7, half = r.w / 2;
  button(sf, PAGEUP, tui_rect_make(r.x, y, half - 1, 3), "PREVIOUS PAGE", false,
         top > 0);
  button(sf, PAGEDOWN, tui_rect_make(r.x + half, y, r.w - half, 3), "NEXT PAGE",
         false, top + visible_rows < count);
  int third = r.w / 3;
  y += 4;
  button(sf, TONE, tui_rect_make(r.x, y, third - 1, 3),
         ls_music_warm() ? "TONE WARM" : "TONE FLAT", ls_music_warm(), true);
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
    tui_rect side = tui_rect_make(r.x + split, r.y, r.w - split, r.h);
    bool boxed = side.w >= 40 && r.h >= 26;
    int info_h = boxed ? 11 : 8;
    int h = (r.h - info_h - 4) / 2;
    if (h > 7)
      h = 7;
    if (h < 3)
      h = 3;
    panel(sf, tui_rect_make(r.x, r.y, split - 1, r.h), live, false);
    if (boxed)
      info_boxed(sf, side, playing);
    else
      info_flat(sf, side, playing);
    controls(sf, tui_rect_make(side.x, side.y + side.h - (2 * h + 3), side.w,
                               2 * h + 3),
             playing, h);
  } else {
    const int h = 5;
    bool boxed = r.w >= 40 && r.h >= 60;
    int info_h = boxed ? 11 : 8;
    if (boxed)
      info_boxed(sf, r, playing);
    else
      info_flat(sf, r, playing);
    int bottom = r.y + r.h - (2 * h + 3);
    panel(sf, tui_rect_make(r.x, r.y + info_h + 1, r.w, bottom - r.y - info_h - 2),
          live, boxed);
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
  case 'h':
    act(HOLD);
    break;
  case 'a':
    act(AXIS);
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
    act(STOP);
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
    .hint = "1-4/V view  C colour  H hold  A axis  O options  SPACE play  I mic  W rec  L tracks  +/- vol",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch};
