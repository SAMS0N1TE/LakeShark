/* Separate player/library views. The player's instrument panel shows one
   of four readings of the audio: stereo peak meters of what is heard, a
   2/3-octave band display, a fine scan of thin bars, and a pixel
   spectrogram for finding what is in a recording. Landscape gives the
   panel most of the width, portrait most of the height. Six analysis layers
   follow those original four views.

   The chrome is the house style: framed tiles on a dithered field in the
   colour of what they do, boxed panels with their title in the border, and
   the transport drawn as pictures when a key is tall enough to carry one. */
#include "../../ls_music_backend.h"
#include "../../ls_audio_view.h"
#include "../../ls_audio_layers.h"
#include "../../ls_music_options.h"
#include "../../ls_icons.h"
#include "../../ls_media_widgets.h"
#include "../../ls_music_transport.h"
#include "../../ls_options.h"
#include "../../ls_picker.h"
#include "../../ls_spectro.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ls_textpack.h"
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
  TAB_WAVE, TAB_SPECTRO, TAB_MELODIC, TAB_SPECTRUM, TAB_SCOPE, TAB_TOOLS,
  MORE,
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
  SEEK_BACK,
  SEEK_FWD,
  LIVE,
  VIEW_PICK, VIEW_PREV, VIEW_NEXT,   /* the one VIEW key and its steps */
  ACTIONS
};
/* The key each control answers to, drawn on it while a keyboard is fitted. */
static const char *const KEYS[ACTIONS] = {
    "B", "P", "N", "-", "M", "+", "L", "I", "W", "1", "2", "3", "4",
    "5", "6", "7", "8", "9", "0", "V", "C", "Z", "E", "D", "R", "ESC", "U", "J", "O", "S", "", "H", "A",
    "[", "]", "", "V", "", ""};
enum { VIEW_LEVEL, VIEW_BANDS, VIEW_SCAN, VIEW_WATERFALL, VIEW_WAVE, VIEW_SPECTRO, VIEW_MELODIC, VIEW_SPECTRUM, VIEW_SCOPE, VIEW_TOOLS, VIEWS };
static const char *const VIEW_NAME[VIEWS] = {"LEVEL", "BANDS", "SCAN",
                                             "WATERFALL", "WAVE", "SPECTRO", "MELODIC", "SPECTRUM", "SCOPE", "TOOLS"};
static const char *const VIEW_MID[VIEWS] = {"LEVEL", "BANDS", "SCAN", "WFALL", "WAVE",
                                            "SPECTRO", "MELODIC", "SPECTRM", "SCOPE", "TOOLS"};
static const char *const VIEW_SHORT[VIEWS] = {"LVL", "BND", "SCN", "WF", "WAV", "SPG", "MEL", "SPC", "SCP", "TLS"};
static const char *const VIEW_TITLE[VIEWS] = {
    " PEAK LEVEL ", " SPECTRUM 2/3 OCTAVE ", " SCAN ", " WATERFALL ", " WAVE ", " SPECTRO ", " MELODIC ", " SPECTRUM ", " SCOPE ", " TOOLS "};
/* Each reading has its colour: its tab, and the frame of its panel. */
static const uint8_t VIEW_HUE[VIEWS] = {TUI_GREEN, TUI_YELLOW, TUI_MAGENTA,
                                        TUI_CYAN, TUI_CYAN, TUI_MAGENTA, TUI_YELLOW, TUI_GREEN, TUI_CYAN, TUI_YELLOW};
/* Screen state that only the UI task touches lives in PSRAM: internal RAM
   is what boot runs out of, and the radio's endpoint needs it. */
EXT_RAM_BSS_ATTR static tui_rect hits[ACTIONS];
EXT_RAM_BSS_ATTR static bool enabled[ACTIONS];
static tui_rect rows_rect, progress_rect;
/* The short track list under the visual in portrait: its rows, and the
   track on its first row. Two cell rows a track, for a finger. */
static tui_rect next_rect;
static int next_top;
#define NEXT_ROW 2
static bool ready, library;
static int selected, current = -1, top, source, focus = -1, row_height = 3,
                     visible_rows;
/* A file FILES handed over, and the name of the one playing from outside
   the music folder: a P25 call, a recording somewhere else on the card. */
EXT_RAM_BSS_ATTR static char handed[416], outside[416];
static bool on_track(void) { return current >= 0 || outside[0]; }
static const char *outside_name(void) {
  const char *slash = strrchr(outside, '/');
  return slash ? slash + 1 : outside;
}
/* Kept across visits and rotations: the panel is a preference. */
static int view = VIEW_LEVEL, palette, span_step;
static unsigned frame;
static ls_audio_layers *layers;
EXT_RAM_BSS_ATTR static char layer_footer[80];
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
/* Move playback. One request per player turn: the player takes one event
   at a time, and a burst of taps on the bar used to fill its queue. The
   analysis views follow again after a seek. */
static int64_t last_seek_us;
static void seek_to(int64_t ms) {
  if (!on_track() || ls_music_state() == LS_MUSIC_STOPPED)
    return;
  const int64_t now = esp_timer_get_time();
  if (last_seek_us && now - last_seek_us < 120000)
    return;
  const int64_t total = ls_music_duration_ms();
  if (ms < 0)
    ms = 0;
  if (ms > total)
    ms = total;
  if (ls_music_seek_ms((uint32_t)ms)) {
    last_seek_us = now;
    ls_audio_view_reset();
    ls_audio_layers_unpin(layers);
  }
}
static void seek_by(int64_t delta_ms) { seek_to((int64_t)ls_music_position_ms() + delta_ms); }
static void start(int index) {
  if (ready && ls_music_play(index)) {
    selected = current = index;
    outside[0] = 0;
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
    {.label="PLAYBACK",.kind=LS_OPT_MENU,.sub=&ls_music_playback_options},
    {.label="ANALYSIS",.kind=LS_OPT_MENU,.sub=&ls_music_analysis_options},
    {.label="GENERATOR",.kind=LS_OPT_MENU,.sub=&ls_music_generator_options},
    {.label="WATERFALL",.kind=LS_OPT_MENU,.sub=&CTX_WATERFALL},
    MUSIC_ROW("TONE", TONE),
    MUSIC_ROW("SOURCE", SOURCE), MUSIC_ROW("STOP", STOP),
    MUSIC_ROW("REPAIR WAV", REPAIR),
};
#undef MUSIC_ROW
static const ls_opt_ctx_t CTX_MUSIC = {.name = "MUSIC", .job = -1,
                                       .radio = LS_RSEL_NONE,
                                       LS_OPT_ROWS(OPT_MUSIC), .tag = "PLAYER"};
static void view_pick_done(int i) { if (i >= 0 && i < VIEWS) view = i; }
static void act(int id) {
  switch (id) {
  case PREV:
    start(ls_music_step(selected, ls_music_count(), -1));
    break;
  case NEXT:
    start(ls_music_step(selected, ls_music_count(), 1));
    break;
  case PLAY:
    if (on_track() && ls_music_state() != LS_MUSIC_STOPPED)
      ls_music_toggle();
    else if (outside[0])
      ls_music_play_path(outside);
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
    outside[0]=0;
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
  case TAB_WAVE: case TAB_SPECTRO: case TAB_MELODIC: case TAB_SPECTRUM: case TAB_SCOPE: case TAB_TOOLS:
    view = VIEW_LEVEL + (id - TAB_LEVEL);
    break;
  case MORE: /* the next page of four tabs, on a short row */
    view = (view / 4 * 4 + 4) % VIEWS;
    break;
  case VIEW_PREV: view = (view + VIEWS - 1) % VIEWS; break;
  case VIEW_NEXT: view = (view + 1) % VIEWS; break;
  case VIEW_PICK: {
    static const char *const WHAT[VIEWS] = {"peak and hold", "octave bands", "the band scan", "time against frequency",
                                            "the waveform", "spectrogram", "notes heard", "the spectrum", "scope", "tools"};
    ls_picker_open("VISUAL", view_pick_done);
    for (int i = 0; i < VIEWS; i++) ls_picker_add(VIEW_NAME[i], WHAT[i]);
    ls_picker_select(view);
    break;
  }
  case COLOUR:
    palette = (palette + 1) % PALETTES;
    break;
  case SPAN:
    if(view>=VIEW_WAVE){ls_audio_layers_zoom(layers);break;}
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
  case SEEK_BACK:
    seek_by(-5000);
    break;
  case SEEK_FWD:
    seek_by(5000);
    break;
  case LIVE:
    ls_audio_layers_unpin(layers);
    break;
  }
}
/* Play what FILES handed over. In the SD music folder it is that row of the
   list, so NEXT and PREV go on from it; anywhere else it plays on its own. */
static void play_handed(void) {
  char path[sizeof(handed)];
  snprintf(path, sizeof(path), "%s", handed);
  handed[0] = 0;
  if (!ready || !path[0])
    return;
  const char *slash = strrchr(path, '/'), *name = slash ? slash + 1 : path;
  static const char music[] = "/sdcard/music";
  if (slash && (size_t)(slash - path) == sizeof(music) - 1 &&
      !strncmp(path, music, sizeof(music) - 1)) {
    if (source != 0 && ls_music_scan(0))
      source = 0;
    for (int i = 0; source == 0 && i < ls_music_count(); i++)
      if (!strcmp(ls_music_name(i), name)) {
        start(i);
        return;
      }
  }
  if (ls_music_play_path(path)) {
    snprintf(outside, sizeof(outside), "%s", path);
    current = -1;
    library = false;
    focus = -1;
  }
}
static void enter(void) {
  selected = top = 0;
  current = -1;
  outside[0] = 0;
  focus = -1;
  library = false;
  frame = 0;
  /* The player parks the radio. Say so, or the receiver stays parked when
     FM or P25 is next shown: it asks for what it believes is still on. */
  ls_tui_radio_want(NULL);
  ls_audio_view_open();
  layers=ls_audio_layers_new();
  ls_music_options_cursor(layers);
  ready = ls_music_open();
  if (ready)
    ls_music_scan(source);
  play_handed();
}
static void leave(void) {
  ls_music_close();
  ls_audio_view_close();
  ls_audio_layers_free(layers);layers=NULL;
  ls_music_options_cursor(NULL);
  ready = false;
  current = -1;
  outside[0] = 0;
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
  case TAB_WAVE: case TAB_SPECTRO: case TAB_MELODIC: case TAB_SPECTRUM: case TAB_SCOPE: case TAB_TOOLS:
    return VIEW_HUE[id - TAB_LEVEL];
  case SEEK_BACK: case SEEK_FWD: return TUI_CYAN;
  case STOP: return TUI_RED;
  case LIVE: return TUI_GREEN;
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
/* The longest of a view's names that fits a tab with `inner` cells. */
static const char *view_label(int i, int inner) {
  if ((int)strlen(VIEW_NAME[i]) <= inner) return VIEW_NAME[i];
  if ((int)strlen(VIEW_MID[i]) <= inner) return VIEW_MID[i];
  return VIEW_SHORT[i];
}
/* How the ten views' tabs fit across `w` cells in at most `max_rows` rows:
   all ten in one or two rows when each tab can carry a word, else four of
   them and a full-size MORE tile for the next four. Returns the rows used. */
static int tab_rows_for(int w, int max_rows) {
  (void)w; (void)max_rows;
  return 1;
}
static void tabs(tui_surface *sf, tui_rect t, int rows) {
  (void)rows;
  {
    const int side = t.w / 5 < 6 ? 6 : t.w / 5;
    char label[32];
    snprintf(label, sizeof(label), t.w - 2 * side >= 22 ? "VISUAL: %s" : "%s", VIEW_NAME[view]);
    button(sf, VIEW_PREV, tui_rect_make(t.x, t.y, side, 3), "<", false, true);
    button(sf, VIEW_PICK, tui_rect_make(t.x + side, t.y, t.w - 2 * side, 3), label, true, true);
    button(sf, VIEW_NEXT, tui_rect_make(t.x + t.w - side, t.y, side, 3), ">", false, true);
    return;
  }
  if (t.w / VIEWS >= 8) {
    const int per = rows == 2 ? VIEWS / 2 : VIEWS;
    for (int i = 0; i < VIEWS; i++) {
      int j = i % per, row = i / per, tw = t.w / per;
      /* Touching tiles, the last one taking the remainder: every cell of
         the row is somebody's tab. */
      int x = t.x + j * tw, w = j == per - 1 ? t.w - j * tw : tw;
      button(sf, TAB_LEVEL + i, tui_rect_make(x, t.y + 3 * row, w, 3), view_label(i, w - 2),
             i == view, true);
    }
    return;
  }
  /* One short row: this page of four, then MORE. */
  int first = (view / 4) * 4, count = VIEWS - first < 4 ? VIEWS - first : 4;
  int tw = t.w / 5;
  for (int j = 0; j < count; j++) {
    int i = first + j;
    button(sf, TAB_LEVEL + i, tui_rect_make(t.x + j * tw, t.y, tw, 3), view_label(i, tw - 2),
           i == view, true);
  }
  button(sf, MORE, tui_rect_make(t.x + 4 * tw, t.y, t.w - 4 * tw, 3), "MORE >", false, true);
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
  /* Every view has a full-size tab across the top of the panel: one or two
     rows of them, or four and MORE where the panel is short. The format
     lines go under them when the now-playing box does not carry them. */
  const int rows = tab_rows_for(in.w, in.h >= 30 ? 2 : 1);
  tabs(sf, tui_rect_make(in.x, in.y, in.w, 3 * rows), rows);
  int body_y = in.y + 3 * rows;
  if (!boxed) {
    format_line(sf, in, in.x + 1, body_y);
    output_line(sf, in, in.x + 1, body_y + 1);
    body_y += 2;
  }
  tui_rect body = tui_rect_make(in.x, body_y, in.w, in.y + in.h - 1 - body_y);
  if (body.h < 4)
    return;
  tui_rect plot = tui_rect_make(body.x, body.y, body.w, body.h - 3);
  tui_rect controls_row = tui_rect_make(body.x, body.y + body.h - 3, body.w, 3);
  char line[96];
  unsigned rate = rate_now();
  if(view>=VIEW_WAVE){
    ls_audio_layers_draw(layers,sf,body,view-VIEW_WAVE,ls_music_analysis(),ls_music_overview(),ls_music_tools(),ls_music_position_ms(),wf_hold);
    /* A held reading says so, with the way back to following playback. */
    if (ls_audio_layers_pinned(layers) && body.w >= 30)
      button(sf, LIVE, tui_rect_make(body.x + body.w - 10, body.y, 10, 3), "LIVE", false, true);
    LS_SNPRINTF(t_scr_music_v_view_h_s_strip, layer_footer,sizeof(layer_footer),"H %s  Z zoom  strip: seek  %.1fx varispeed",wf_hold?"HELD":"hold",ls_music_speed_get()/1000.0);
    ls_safe_line(sf,in,in.y+in.h-1,layer_footer,LS_ATTR_DIM);
    return;
  }
  switch (view) {
  case VIEW_LEVEL:
    ls_audio_view_draw(sf, body, view, palette, span_step, playing);
    LS_SNPUTS(t_scr_music_output_dbfs_after_volume, line, sizeof(line), "OUTPUT dBFS AFTER VOLUME  <> SOURCE BEFORE IT");
    break;
  case VIEW_BANDS:
    ls_audio_view_draw(sf, body, view, palette, span_step, playing);
    LS_SNPRINTF(t_scr_music_fft_d_hann_1f, line, sizeof(line), "FFT %d HANN  %.1f Hz/BIN  %s",
             LS_MUSIC_FFT_SIZE, (double)rate / LS_MUSIC_FFT_SIZE, source_name());
    break;
  case VIEW_SCAN: {
    ls_audio_view_draw(sf, plot, view, palette, span_step, playing);
    strip(sf, controls_row, false, SCAN_FLOOR_DB, SCAN_TOP_DB);
    char lo[8], hi[8];
    freq_text(lo, sizeof(lo), SCAN_LO_HZ);
    freq_text(hi, sizeof(hi), scan_top_hz());
    LS_SNPRINTF(t_scr_music_s_s_hz_log, line, sizeof(line), "%s - %s Hz LOG  FFT %d  %s", lo, hi,
             LS_MUSIC_FFT_SIZE, source_name());
    break;
  }
  default: {
    push_waterfall();
    ls_audio_view_draw(sf, plot, view, palette, span_step, playing);
    strip(sf, controls_row, true, (float)wf_floor, (float)(wf_floor + wf_range));
    char hi[12];
    ls_spectro_khz(hi, sizeof(hi), rate / 2.0f / TOP_DIV[wf_top]);
    LS_SNPRINTF(t_scr_music_fft_d_1f_hz, line, sizeof(line), "FFT %d  %.1f Hz/BIN  %.0f LINES/S  %s 0-%s",
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
  snprintf(text, n, "%s", outside[0] ? outside_name() : ls_music_name(current >= 0 ? current : selected));
  char *dot = strrchr(text, '.');
  if (dot)
    *dot = 0;
  char *split = strstr(text, " - ");
  *artist = outside[0] ? "FROM FILES" : "SD MUSIC";
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
         : mic == LS_MIC_TONE                  ? "GENERATOR"
         : mic == LS_MIC_LIVE                  ? "LISTENING"
         : playing                             ? "PLAYING"
         : ls_music_state() == LS_MUSIC_PAUSED ? "PAUSED"
                                               : "READY";
}
static uint8_t state_hue(bool playing) {
  ls_music_mic_t mic = ls_music_mic_state();
  return mic == LS_MIC_RECORDING               ? TUI_RED
         : mic == LS_MIC_TONE                  ? TUI_YELLOW
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
  LS_SNPRINTF(t_scr_music_02u_02u_02u_02u, out, n, "%02u:%02u / %02u:%02u", (unsigned)(elapsed_ms / 60000),
           (unsigned)(elapsed_ms / 1000 % 60), (unsigned)(total_ms / 60000),
           (unsigned)(total_ms / 1000 % 60));
}
/* A thick bar to tap: played part bright, a knob where playback is, and
   the rest dim. progress_rect is its row; touch takes a row either side. */
static void seek_bar(tui_surface *sf, tui_rect r, bool show, uint32_t elapsed, uint32_t total) {
  progress_rect = r;
  if (!show || r.w < 4)
    return;
  int knob = total ? (int)((uint64_t)(elapsed > total ? total : elapsed) * (r.w - 1) / total) : 0;
  for (int x = 0; x < r.w; x++) {
    bool done = x < knob;
    tui_put_char(sf, r, r.x + x, r.y, x == knob ? LS_TUI_BLOCK_FULL : done ? LS_TUI_BLOCK_LOWER : '-',
                 x == knob ? TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK)
                 : done    ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                           : TUI_ATTR(TUI_CYAN, TUI_BLACK));
  }
}
/* The roomy form: a box titled with the state, the cassette in its colour,
   the track beside it and the seek bar under both. Twelve rows with the
   keys under it. */
static void info_boxed(tui_surface *sf, tui_rect r, bool playing) {
  int pad = 0;
  for (int y = r.y; y < r.y + 12; y++)
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
  tui_rect box = tui_rect_make(r.x, r.y, r.w, 9);
  char title[24];
  snprintf(title, sizeof(title), " %s ", state_name(playing));
  ls_panel_box(sf, box, title, hue);
  int count = outside[0] ? 0 : ls_music_count();
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
    tui_put_str(sf, text, tx, text.y, mic == LS_MIC_TONE ? "GENERATOR" : "MICROPHONE",
                TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, text, tx, text.y + 1,
                mic == LS_MIC_TONE ? "Tone generator, obeys mute" : mic == LS_MIC_RECORDING ? (name ? name + 1 : path)
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
    elapsed = on_track() ? ls_music_position_ms() / 1000 : 0;
    total = on_track() ? ls_music_duration_ms() / 1000 : 0;
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
  /* The seek bar: the whole width of the box under the cassette. A tap on
     it, the row above or the frame below moves playback there. */
  seek_bar(sf, tui_rect_make(box.x + 2, box.y + 7, box.w - 4, 1), mic == LS_MIC_OFF, elapsed * 1000,
           total * 1000);
  int bw = (r.w - 3) / 4;
  top_keys(sf, r.x + (r.w - (4 * bw + 3)) / 2, r.y + 9, bw, 3);
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
    ls_safe_line(sf, r, r.y + 3, mic == LS_MIC_TONE ? "GENERATOR" : "MICROPHONE", TUI_ATTR(TUI_MAGENTA | TUI_BRIGHT, TUI_BLACK));
    ls_safe_line(sf, r, r.y + 4,
                 mic == LS_MIC_TONE ? "Tone generator, obeys mute" : mic == LS_MIC_RECORDING ? (name ? name + 1 : path)
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
    elapsed = on_track() ? ls_music_position_ms() / 1000 : 0;
    total = on_track() ? ls_music_duration_ms() / 1000 : 0;
  }
  if (*error)
    ls_safe_line(sf, r, r.y + 5, error, TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
  tui_rect clock_rect = tui_rect_make(r.x, r.y + 6, r.w, 1);
  if (mic != LS_MIC_OFF) {
    char clock[48];
    if (mic == LS_MIC_RECORDING)
      snprintf(clock, sizeof(clock), "%02u:%02u RECORDED", elapsed / 60, elapsed % 60);
    else
      snprintf(clock, sizeof(clock), "LIVE");
    ls_safe_line(sf, r, r.y + 6, clock, white);
  } else
    ls_media_clock(sf, clock_rect, elapsed * 1000, total * 1000);
  int count = outside[0] ? 0 : ls_music_count();
  if (count > 0) {
    char n[32];
    int len = snprintf(n, sizeof(n), "TRACK %d/%d",
                       (current >= 0 ? current : selected) + 1, count);
    tui_put_str(sf, r, r.x + r.w - 2 - len, r.y + 6, n,
                TUI_ATTR(hue, TUI_BLACK));
  }
  seek_bar(sf, tui_rect_make(r.x + 2, r.y + 7, r.w - 4, 1), true, elapsed * 1000, total * 1000);
}
/* The width a row of keys from y to y+h-1 can use: clear of the rounded
   corner on those rows only, so the rows above the corner keep the full
   width instead of all of them shrinking to the bottom row's. */
static tui_rect key_row(tui_surface *sf, tui_rect r, int y, int h) {
  int pad = 0, cols = tui_surface_rect(sf).w;
  for (int i = y; i < y + h; i++)
    if (ls_tui_corner_pad(i) > pad)
      pad = ls_tui_corner_pad(i);
  int x0 = r.x < pad ? pad : r.x, x1 = r.x + r.w > cols - pad ? cols - pad : r.x + r.w;
  return tui_rect_make(x0, y, x1 - x0, h);
}
/* Splits a row into n keys one cell apart, the middle one `wide` times as
   wide as the others, and draws them. */
static void key_split(tui_surface *sf, tui_rect row, int n, int wide_at, int wide, const int *ids,
                      const char *const *labels, const bool *on, const bool *can) {
  int units = n - 1 + wide, unit = (row.w - (n - 1)) / units, x = row.x;
  for (int i = 0; i < n; i++) {
    int w = i == n - 1 ? row.x + row.w - x : unit * (i == wide_at ? wide : 1);
    button(sf, ids[i], tui_rect_make(x, row.y, w, row.h), labels[i], on[i], can[i]);
    x += w + 1;
  }
}
/* The player keys, the whole width: PREV, PLAY and NEXT; the volume row; then
   back 5 s, STOP and forward 5 s, the row the corner narrows. h+8 rows. */
/* Rows the keys take out of `avail`: the full set, or the short form. */
static int controls_rows(int h, int avail) { return avail >= h + 8 ? h + 8 : h + 6; }
static void controls(tui_surface *sf, tui_rect r, bool playing, int h) {
  const bool can = ready && ls_music_count() > 0, going = on_track() && ls_music_state() != LS_MUSIC_STOPPED;
  int y = r.y;
  {
    const int ids[3] = {PREV, PLAY, NEXT};
    const char *const labels[3] = {"PREV", playing ? "PAUSE" : "PLAY", "NEXT"};
    const bool on[3] = {false, true, false}, ok[3] = {can, can || outside[0], can};
    key_split(sf, key_row(sf, r, y, h), 3, 1, 2, ids, labels, on, ok);
  }
  y += h + 1;
  /* The volume row, clear of the corner: VOL -, the gauge, VOL +. */
  tui_rect row = key_row(sf, r, y, 3);
  int value = ls_music_volume();
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  if (r.h < h + 8) {
    /* Short: VOL -, MUTE and VOL + in one row, the gauge on the two rows
       under it, no seek row ([ and ] still seek). h+6 rows. */
    const bool muted = ls_music_muted();
    const int ids[3] = {QUIETER, SILENCE, LOUDER};
    const char *const labels[3] = {"VOL -", muted ? "UNMUTE" : "MUTE", "VOL +"};
    const bool on[3] = {false, muted, false}, ok[3] = {value > 0, true, value < 100};
    key_split(sf, row, 3, 1, 1, ids, labels, on, ok);
    tui_rect g = key_row(sf, r, y + 3, 2);
    char vol[24];
    if (value)
      snprintf(vol, sizeof(vol), "%d%% %.1f dB", value, (double)ls_music_volume_db());
    else
      snprintf(vol, sizeof(vol), "MUTED");
    int segs = g.w - (int)strlen(vol) - 2 >= 10 ? 10 : 5, gx = g.x + 1;
    for (int i = 0; i < segs; i++)
      tui_put_char(sf, g, gx + i, g.y, LS_TUI_BLOCK_LEFT,
                   i < value * segs / 100 ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                                          : TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
    tui_put_str(sf, g, gx + segs + 1, g.y, vol, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, g, gx, g.y + 1, "VOLUME", LS_ATTR_DIM);
    return;
  }
  int unit = (row.w - 2) / 4, x = row.x;
  button(sf, QUIETER, tui_rect_make(x, row.y, unit, 3), "VOL -", false, value > 0);
  x += unit + 1;
  tui_rect gauge = tui_rect_make(x, row.y, row.w - 2 * unit - 2, 3);
  button(sf, LOUDER, tui_rect_make(row.x + row.w - unit, row.y, unit, 3), "VOL +", false, value < 100);
  /* The gauge: level and dB, a segment per 5, 10 or 20 points under them,
     and its name. */
  char vol[32];
  if (value)
    snprintf(vol, sizeof(vol), "%d%%  %.1f dB", value, (double)ls_music_volume_db());
  else
    snprintf(vol, sizeof(vol), "0%%  MUTED");
  if ((int)strlen(vol) > gauge.w) {
    if (value)
      snprintf(vol, sizeof(vol), "%.1f dB", (double)ls_music_volume_db());
    else
      snprintf(vol, sizeof(vol), "MUTED");
  }
  int len = (int)strlen(vol);
  tui_put_str(sf, gauge, gauge.x + (gauge.w - len) / 2, gauge.y, vol, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
  int segs = gauge.w - 2 >= 20 ? 20 : gauge.w - 2 >= 10 ? 10 : 5, gx = gauge.x + (gauge.w - segs) / 2;
  for (int i = 0; i < segs; i++)
    tui_put_char(sf, gauge, gx + i, gauge.y + 1, LS_TUI_BLOCK_LEFT,
                 i < value * segs / 100 ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                                        : TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
  if (gauge.w >= 6)
    tui_put_str(sf, gauge, gauge.x + (gauge.w - 6) / 2, gauge.y + 2, "VOLUME", LS_ATTR_DIM);
  /* The row the corner narrows: back 5 s, STOP, forward 5 s and MUTE, the
     labels as long as the keys allow. */
  row = key_row(sf, r, y + 4, 3);
  const bool muted = ls_music_muted();
  /* Too narrow for four keys that can spell UNMUTE: STOP leaves (S, and
     OPTIONS > STOP, still stop). */
  const bool four = (row.w - 3) / 4 - 2 >= 6;
  const int inner = four ? (row.w - 3) / 4 - 2 : (row.w - 2) / 3 - 2;
  const char *back = inner >= 8 ? "<< 5 SEC" : inner >= 5 ? "<< 5s" : "-5s",
             *fwd = inner >= 8 ? "5 SEC >>" : inner >= 5 ? "5s >>" : "+5s";
  const char *mute = muted ? "UNMUTE" : "MUTE";
  const bool stop_ok = going || ls_music_mic_state() != LS_MIC_OFF;
  if (four) {
    const int ids[4] = {SEEK_BACK, STOP, SEEK_FWD, SILENCE};
    const char *const labels[4] = {back, "STOP", fwd, mute};
    const bool on[4] = {false, false, false, muted}, ok[4] = {going, stop_ok, going, true};
    key_split(sf, row, 4, 0, 1, ids, labels, on, ok);
  } else {
    const int ids[3] = {SEEK_BACK, SEEK_FWD, SILENCE};
    const char *const labels[3] = {back, fwd, mute};
    const bool on[3] = {false, false, muted}, ok[3] = {going, going, true};
    key_split(sf, row, 3, 0, 1, ids, labels, on, ok);
  }
}

/* ------------------------------------------------------- the track list -- */

static void draw_library(tui_surface *sf, tui_rect r) {
  int count = ls_music_count();
  ls_safe_line(sf, r, r.y + 1, source ? "FLASH / MUSIC" : "SD / MUSIC",
               TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
  if (ls_tui_keyboard_mode())
    ls_safe_line(sf, tui_rect_make(r.x, r.y, r.w - 13, r.h), r.y + 2,
                 LS_TEXT(t_scr_music_up_down_select_enter, "UP/DOWN select  ENTER play  LEFT/RIGHT prev/next"),
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
    ls_safe_line(sf, rows_rect, rows_rect.y + 1, LS_TEXT(t_scr_music_no_audio_tracks_use, "No audio tracks. Use RESCAN."),
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
/* The tracks around the one playing, in a box between the visual and the
   keys: a tap plays one. The whole list is TRACKS. */
static void up_next(tui_surface *sf, tui_rect r) {
  next_rect = tui_rect_make(0, 0, 0, 0);
  int count = ls_music_count();
  int pad = 0;
  for (int y = r.y; y < r.y + r.h; y++)
    if (ls_tui_corner_pad(y) > pad)
      pad = ls_tui_corner_pad(y);
  if (r.x < pad) {
    r.w -= pad - r.x;
    r.x = pad;
  }
  if (r.x + r.w > tui_surface_rect(sf).w - pad)
    r.w = tui_surface_rect(sf).w - pad - r.x;
  if (count <= 0 || r.w < 20 || r.h < 2 + NEXT_ROW)
    return;
  const int shown = (r.h - 2) / NEXT_ROW, at = current >= 0 ? current : selected;
  int first = at - 1;
  if (first > count - shown)
    first = count - shown;
  if (first < 0)
    first = 0;
  char title[32];
  snprintf(title, sizeof(title), " TRACKS %d/%d ", at + 1, count);
  ls_panel_box(sf, r, title, TUI_CYAN);
  next_rect = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, shown * NEXT_ROW);
  next_top = first;
  for (int i = 0; i < shown && first + i < count; i++) {
    const int index = first + i, y = next_rect.y + i * NEXT_ROW;
    const bool now = index == current;
    tui_rect row = tui_rect_make(next_rect.x, y, next_rect.w, NEXT_ROW);
    if (now)
      ls_fill_dither(sf, tui_rect_make(row.x, y, row.w, 1), LS_DITHER_MEDIUM, TUI_GREEN);
    char num[16], name[96];
    snprintf(num, sizeof(num), "%3d  ", index + 1);
    snprintf(name, sizeof(name), "%s", ls_music_name(index));
    char *dot = strrchr(name, '.');
    if (dot)
      *dot = 0;
    int room = row.w - 12;
    if (room < 1)
      room = 1;
    if ((int)strlen(name) > room)
      name[room] = 0;
    int x = row.x + 1;
    seg(sf, row, &x, y, now ? ">" : " ", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    seg(sf, row, &x, y, num, TUI_ATTR(TUI_YELLOW, TUI_BLACK));
    seg(sf, row, &x, y, name, TUI_ATTR(now ? TUI_WHITE | TUI_BRIGHT : TUI_WHITE, TUI_BLACK));
    if (now)
      tui_put_str(sf, row, row.x + row.w - 5, y, " NOW", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
  }
}
static void draw(tui_surface *sf, tui_rect r) {
  memset(hits, 0, sizeof(hits));
  next_rect = tui_rect_make(0, 0, 0, 0);
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
    int info_h = boxed ? 12 : 8;
    int h = r.h - info_h - 9;
    if (h > 7)
      h = 7;
    if (h < 3)
      h = 3;
    panel(sf, tui_rect_make(r.x, r.y, split - 1, r.h), live, false);
    if (boxed)
      info_boxed(sf, side, playing);
    else
      info_flat(sf, side, playing);
    controls(sf, tui_rect_make(side.x, side.y + side.h - controls_rows(h, side.h - info_h), side.w,
                               controls_rows(h, side.h - info_h)),
             playing, h);
  } else {
    const int h = 5;
    bool boxed = r.w >= 40 && r.h >= 60;
    int info_h = boxed ? 12 : 8;
    if (boxed)
      info_boxed(sf, r, playing);
    else
      info_flat(sf, r, playing);
    int bottom = r.y + r.h - controls_rows(h, r.h - info_h);
    /* A tall screen has room for the tracks around this one between the
       visual and the keys; the visual keeps at least 26 rows. */
    int list_h = 2 + 4 * NEXT_ROW;
    if (outside[0] || ls_music_count() <= 0 || bottom - r.y - info_h - 2 - list_h - 1 < 26)
      list_h = 0;
    panel(sf, tui_rect_make(r.x, r.y + info_h + 1, r.w, bottom - r.y - info_h - 2 - (list_h ? list_h + 1 : 0)),
          live, boxed);
    if (list_h)
      up_next(sf, tui_rect_make(r.x, bottom - 1 - list_h, r.w, list_h));
    controls(sf, tui_rect_make(r.x, bottom, r.w, controls_rows(h, r.h - info_h)), playing, h);
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
  case ']':
    act(ch == '[' ? SEEK_BACK : SEEK_FWD);
    break;
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
    view = (view + 1) % VIEWS;
    break;
  case '1':
  case '2':
  case '3':
  case '4':
  case '5': case '6': case '7': case '8': case '9':
    act(TAB_LEVEL + (ch - '1'));
    break;
  case '0':
    act(TAB_TOOLS);break;
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
  /* The seek bar, with a row of slack above and below: a finger is wider
     than a cell. Taps a little past either end go to the start or end. */
  if (!library && on_track() && progress_rect.w > 1 && y >= progress_rect.y - 1 &&
      y <= progress_rect.y + 1 && x >= progress_rect.x - 2 && x < progress_rect.x + progress_rect.w + 2) {
    int at = x - progress_rect.x;
    if (at < 0) at = 0;
    if (at > progress_rect.w - 1) at = progress_rect.w - 1;
    seek_to((int64_t)at * ls_music_duration_ms() / (progress_rect.w - 1));
    return true;
  }
  /* The analysis views' whole-track strip moves playback too. */
  if (!library && view >= VIEW_WAVE) {
    tui_rect strip = ls_audio_layers_strip(layers);
    if (strip.w > 1 && tui_rect_contains(strip, x, y)) {
      seek_to((int64_t)(x - strip.x) * ls_music_duration_ms() / (strip.w - 1));
      return true;
    }
    if (ls_audio_layers_touch(layers, x, y))
      return true;
  }
  if (!library && next_rect.w > 0 && tui_rect_contains(next_rect, x, y)) {
    int i = next_top + (y - next_rect.y) / NEXT_ROW;
    if (i < ls_music_count())
      start(i);
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
    .hint = "1-0/V view  [ ] seek  C colour  H hold  O options  SPACE play  I mic  W rec  L tracks  +/- vol",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch};

/* FILES' PLAY: open the player on a WAV wherever it is on the card. */
bool ls_scr_music_play_file(const char *path) {
  const int index = ls_tui_screen_index_of(&ls_scr_music);
  if (index < 0 || !path || strlen(path) >= sizeof(handed))
    return false;
  snprintf(handed, sizeof(handed), "%s", path);
  if (ls_tui_screen_current() == index)
    play_handed();
  else
    ls_tui_screen_show(index);
  return true;
}
