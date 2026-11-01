#include "ls_calls.h"
#include "call_archive.h"
#include "ls_audio_view.h"
#include "ls_glyph.h"
#include "ls_braille.h"
#include "ls_tui.h"
#include "ls_media_widgets.h"
#include "ls_motion.h"
#include "ls_music_backend.h"
#include "ls_picker.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "ls_textpack.h"

static bool opened, replay;
static int selected, top, visible;
static const char *return_receiver;
static tui_rect list_area, progress_area, strip_area;
/* The receiver's live face, when it has one (see ls_calls.h). */
static const ls_calls_live_t *live, *live_hook;
static const char *live_receiver;
static int filter, order, focus = -1, playing_index = -1;
static uint32_t filter_tg;
enum {
  PREV,
  PLAY,
  NEXT,
  QUIETER,
  MUTE,
  LOUDER,
  STOP,
  DELETE,
  OPTIONS,
  BACK,
  FILES,
  KEEP,
  SORT,
  PAGEUP,
  PAGEDOWN,
  ALL,
  P25,
  FM,
  TODAY,
  TG,
  LIVE0,
  LIVE1,
  ACTIONS
};
#ifdef ESP_PLATFORM
#include "esp_attr.h"
static EXT_RAM_BSS_ATTR tui_rect hits[ACTIONS];
static EXT_RAM_BSS_ATTR bool enabled[ACTIONS];
static EXT_RAM_BSS_ATTR char playing_path[CALL_PATH_MAX];
#else
static tui_rect hits[ACTIONS];
static bool enabled[ACTIONS];
static char playing_path[CALL_PATH_MAX];
#endif
static const char *const sorts[] = {"NEWEST", "OLDEST", "SOURCE", "LENGTH",
                                    "SIZE"};
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
static int restore_volume;
#endif
/* Paths are pinned to the rendered rows; an SD refresh cannot redirect a
   tap or a pending delete confirmation to a different call. */
#ifdef ESP_PLATFORM
#include "esp_attr.h"
static EXT_RAM_BSS_ATTR call_entry_t chosen, candidate;
static EXT_RAM_BSS_ATTR call_meta_t playing_meta;
static EXT_RAM_BSS_ATTR call_entry_t rows[CALL_RECENT_MAX];
#else
static call_entry_t chosen, candidate;
static call_meta_t playing_meta;
static call_entry_t rows[CALL_RECENT_MAX];
#endif
static int row_count;
static char note[80];
/* The newest recording on the card, whatever the filter shows. */
static int64_t archive_newest;
/* Where each visible card landed, for a tap to find it again. */
#define CARD_SLOTS 24
#ifdef ESP_PLATFORM
static EXT_RAM_BSS_ATTR tui_rect card_hit[CARD_SLOTS];
static EXT_RAM_BSS_ATTR int card_index[CARD_SLOTS];
#else
static tui_rect card_hit[CARD_SLOTS];
static int card_index[CARD_SLOTS];
#endif
static int card_shown;
/* A recording newer than any seen arrives from the top: the cards below
   make way over a few frames, and the new one says NEW for a while. Only a
   real arrival moves this list. */
static ls_fresh_t arrival;
static int64_t newest_seen;


static int opt_get(const ls_opt_t *o) { return call_archive_option(o->arg); }
static void opt_set(const ls_opt_t *o, int v) {
  call_archive_set_option(o->arg, v);
}
static double opt_num(const ls_opt_t *o) { return call_archive_option(o->arg); }
static void opt_set_num(const ls_opt_t *o, double v) {
  call_archive_set_option(o->arg, (int)v);
}
static const char *opt_text(const ls_opt_t *o) {
  (void)o;
  return call_archive_talkgroups();
}
static void opt_set_text(const ls_opt_t *o, const char *s) {
  (void)o;
  if (!call_archive_set_talkgroups(s))
    LS_SNPUTS(t_ls_calls_use_up_to_32, note, sizeof(note), "Use up to 32 talkgroups, 1..65535, separated by commas");
}
static void browse(const ls_opt_t *o) {
  (void)o;
  if (opened)
    return;
  const char *radio = ls_tui_radio_claimed();
  ls_calls_open(radio && !strcmp(radio, "FM") ? "FM" : "P25");
}
static double utc_offset(const ls_opt_t *o) {
  (void)o;
  return call_archive_option(CALL_OPT_UTC_QUARTERS) / 4.0;
}
static void set_utc_offset(const ls_opt_t *o, double hours) {
  (void)o;
  call_archive_set_option(CALL_OPT_UTC_QUARTERS, (int)(hours * 4));
}
static void show_utc_offset(const ls_opt_t *o, char *out, size_t n) {
  snprintf(out, n, "%+.2f h", utc_offset(o));
}
static const ls_opt_t OPT_CALLS[] = {
    {.label = "Recent calls",
     .kind = LS_OPT_ACTION,
     .act = browse,
     .leaves = true},
    {.label = "Record P25",
     .kind = LS_OPT_TOGGLE,
     .arg = CALL_OPT_P25,
     .get = opt_get,
     .set = opt_set},
    {.label = "Record FM",
     .kind = LS_OPT_TOGGLE,
     .arg = CALL_OPT_FM,
     .get = opt_get,
     .set = opt_set},
    {.label = "Minimum call (ms)",
     .kind = LS_OPT_LEVEL,
     .arg = CALL_OPT_MIN_MS,
     .num = opt_num,
     .set_num = opt_set_num,
     .lo = 0,
     .hi = 10000,
     .step = 100,
     .unit = "milliseconds"},
    {.label = "Keep newest days",
     .kind = LS_OPT_LEVEL,
     .arg = CALL_OPT_DAYS,
     .num = opt_num,
     .set_num = opt_set_num,
     .lo = 0,
     .hi = 365,
     .step = 1,
     .unit = "UTC days; 0 keeps all"},
    {.label = "Only listed talkgroups",
     .kind = LS_OPT_TOGGLE,
     .arg = CALL_OPT_FILTER,
     .get = opt_get,
     .set = opt_set},
    {.label = "Talkgroup list",
     .kind = LS_OPT_TEXT,
     .text = opt_text,
     .set_text = opt_set_text,
     .max = 191},
    {.label = "Local UTC offset",
     .kind = LS_OPT_LEVEL,
     .num = utc_offset,
     .set_num = set_utc_offset,
     .show = show_utc_offset,
     .lo = -12,
     .hi = 14,
     .step = .25,
     .unit = "Hours east of UTC; adjust for DST"},
};
const ls_opt_ctx_t ls_calls_options = {.name = "CALLS",
                                       .job = -1,
                                       .radio = LS_RSEL_NONE,
                                       LS_OPT_ROWS(OPT_CALLS),
                                       .back_to_screen = true};

/* Local dates belong to the operator; archive folders retain their UTC policy.
 */
static bool local_tm(time_t t, struct tm *out) {
  /* The board has no TZ database. A saved offset makes local time explicit,
     including quarter-hour zones, without changing UTC archive retention. */
  return call_utc_tm(
      t + (time_t)call_archive_option(CALL_OPT_UTC_QUARTERS) * 900, out);
}
static bool same_day(int64_t stamp, time_t now) {
  struct tm a, b;
  return stamp > 0 && local_tm((time_t)stamp, &a) && local_tm(now, &b) &&
         a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}
static int compare(const void *a, const void *b) {
  const call_entry_t *x = a, *y = b;
  int c = 0;
  switch (order) {
  case 1:
    c = x->meta.time < y->meta.time ? -1 : x->meta.time > y->meta.time;
    break;
  case 2:
    c = x->meta.talkgroup < y->meta.talkgroup
            ? -1
            : x->meta.talkgroup > y->meta.talkgroup;
    break;
  case 3:
    c = x->duration_ms > y->duration_ms ? -1 : x->duration_ms < y->duration_ms;
    break;
  case 4:
    c = x->bytes > y->bytes ? -1 : x->bytes < y->bytes;
    break;
  default:
    c = x->meta.time > y->meta.time ? -1 : x->meta.time < y->meta.time;
    break;
  }
  return c ? c : strcmp(x->path, y->path);
}
static void load_rows(void) {
  char previous[CALL_PATH_MAX] = "";
  if (selected >= 0 && selected < row_count)
    snprintf(previous, sizeof(previous), "%s", rows[selected].path);
  row_count = 0;
  archive_newest = 0;
  time_t now = time(NULL);
  for (int i = 0; i < call_archive_count() && i < CALL_RECENT_MAX; i++) {
    if (!call_archive_entry(i, &candidate))
      break;
    if (candidate.meta.time > archive_newest)
      archive_newest = candidate.meta.time;
    bool p25 = candidate.meta.rate == 8000 || candidate.meta.talkgroup != 0 ||
               candidate.meta.source != 0;
    if ((filter == 1 && !p25) || (filter == 2 && p25) ||
        (filter == 3 && !same_day(candidate.meta.time, now)) ||
        (filter == 4 && candidate.meta.talkgroup != filter_tg))
      continue;
    ls_media_insert(rows, &row_count, CALL_RECENT_MAX, sizeof(rows[0]),
                    &candidate, compare);
  }
  if (selected >= row_count)
    selected = row_count ? row_count - 1 : 0;
  playing_index = -1;
  for (int i = 0; i < row_count; i++) {
    if (previous[0] && !strcmp(previous, rows[i].path))
      selected = i;
    if (playing_path[0] && !strcmp(playing_path, rows[i].path))
      playing_index = i;
  }
}
void ls_calls_set_live(const char *receiver, const ls_calls_live_t *hook) {
  live_receiver = hook ? receiver : NULL;
  live_hook = hook;
  if (opened && !hook)
    live = NULL;
}
void ls_calls_show_talkgroup(uint32_t talkgroup) {
  filter_tg = talkgroup;
  filter = talkgroup ? 4 : 0;
  selected = top = 0;
  focus = -1;
  if (opened)
    load_rows();
}
void ls_calls_open(const char *receiver) {
  live = live_hook && receiver && live_receiver &&
                 !strcmp(receiver, live_receiver)
             ? live_hook
             : NULL;
  call_archive_init();
  call_archive_refresh();
  return_receiver = receiver;
  opened = true;
  filter = order = 0;
  selected = top = row_count = 0;
  focus = -1;
  note[0] = 0;
  newest_seen = 0;
  playing_path[0] = 0;
  ls_audio_view_open();
}
bool ls_calls_active(void) { return opened; }
static bool stop(bool resume) {
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
  if (replay) {
    ls_music_close();
    if (ls_music_is_open()) {
      snprintf(note, sizeof(note), "%s", ls_music_error());
      return false;
    }
    if (resume)
      ls_tui_radio_want(return_receiver);
  }
#else
  (void)resume;
#endif
  replay = false;
  playing_path[0] = 0;
  playing_index = -1;
  ls_audio_view_reset();
  return true;
}
void ls_calls_leave(void) {
  /* An unopened CALLS view owns no audio buffers to reset or release. */
  if (!opened && !replay) return;
  if (stop(false)) {
    opened = false;
    ls_audio_view_close();
  }
}
static void cancel_delete(void) {}
static void confirm_delete(int index) {
  if (index != 1)
    return;
  if (replay && !strcmp(chosen.path, playing_path) && !stop(true))
    return;
  snprintf(note, sizeof(note), "%s",
           call_archive_delete(chosen.path) ? "Delete queued; refreshing card"
                                            : "Card busy; try again");
}
static void play_selected(void) {
  if (selected < 0 || selected >= row_count)
    return;
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
  if (replay && !strcmp(rows[selected].path, playing_path) &&
      ls_music_state() != LS_MUSIC_STOPPED) {
    if (!ls_music_toggle())
      snprintf(note, sizeof(note), "%s", ls_music_error());
    return;
  }
  ls_tui_radio_want(NULL);
  if (!replay)
    replay = ls_music_open();
  if (!replay || !ls_music_play_path(rows[selected].path)) {
    snprintf(note, sizeof(note), "%s", ls_music_error());
    if (replay)
      stop(true);
    else
      ls_tui_radio_want(return_receiver);
  } else {
    snprintf(playing_path, sizeof(playing_path), "%s", rows[selected].path);
    playing_index = selected;
    playing_meta = rows[selected].meta;
    note[0] = 0;
    ls_audio_view_reset();
  }
#else
  snprintf(note, sizeof(note), "Playback requires the device audio decoder");
#endif
}
#ifdef ESP_PLATFORM
static void return_from_files(void) {
  int saved_filter = filter, saved_order = order;
  ls_calls_open(return_receiver);
  filter = saved_filter;
  order = saved_order;
  load_rows();
  for (int i = 0; i < row_count; i++)
    if (!strcmp(rows[i].path, chosen.path)) {
      selected = i;
      break;
    }
}
#endif
static void action(int id) {
  focus = -1;
  if (id == BACK) {
    if (stop(true)) {
      opened = false;
      ls_audio_view_close();
    }
    return;
  }
  if (id == OPTIONS) {
    ls_opt_open(&ls_calls_options);
    return;
  }
  if (id == STOP) {
    stop(true);
    return;
  }
  if (id == LIVE0 || id == LIVE1) {
    if (live && live->press)
      live->press(id - LIVE0);
    return;
  }
  if (id >= ALL && id <= TG) {
    if (id == TG) {
      if ((!row_count || !rows[selected].meta.talkgroup) && !filter_tg) {
        LS_SNPUTS(t_ls_calls_select_a_p25_talkgroup, note, sizeof(note), "Select a P25 talkgroup first");
        return;
      }
      if (row_count && rows[selected].meta.talkgroup)
        filter_tg = rows[selected].meta.talkgroup;
    }
    filter = id - ALL;
    top = 0;
    load_rows();
    return;
  }
  if (id == SORT) {
    order = (order + 1) % 5;
    load_rows();
    return;
  }
  if (id == PAGEUP || id == PAGEDOWN) {
    selected += (id == PAGEUP ? -visible : visible);
    if (selected < 0)
      selected = 0;
    if (selected >= row_count)
      selected = row_count ? row_count - 1 : 0;
    return;
  }
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
  if (id == QUIETER || id == LOUDER) {
    ls_media_volume_step(id == LOUDER ? 1 : -1);
    return;
  }
  if (id == MUTE) {
    int v = ls_music_volume();
    if (v) {
      restore_volume = v;
      ls_music_volume_step(-v);
    } else if (restore_volume)
      ls_music_volume_step(restore_volume);
    return;
  }
#endif
  if (!row_count)
    return;
  if (id == PREV || id == NEXT) {
    selected = ls_music_step(playing_index >= 0 ? playing_index : selected,
                             row_count, id == PREV ? -1 : 1);
    play_selected();
    return;
  }
  if (id == PLAY) {
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
    if (replay && ls_music_state() != LS_MUSIC_STOPPED) {
      if (!ls_music_toggle())
        snprintf(note, sizeof(note), "%s", ls_music_error());
      return;
    }
#endif
    play_selected();
    return;
  }
  chosen = rows[selected];
  if (id == KEEP) {
    snprintf(note, sizeof(note), "%s",
             call_archive_protect(chosen.path, !chosen.kept)
                 ? "Protection queued; refreshing card"
                 : "Card busy; try again");
  } else if (id == DELETE) {
    ls_picker_open("DELETE CALL?", confirm_delete);
    ls_picker_back(cancel_delete);
    ls_picker_add("CANCEL", "Keep this recording");
    ls_picker_add("DELETE", LS_TEXT(t_ls_calls_remove_wav_and_metadata, "Remove WAV and metadata"));
    ls_picker_select(0);
  } else if (id == FILES) {
#ifdef ESP_PLATFORM
    extern bool ls_scr_files_show_path(const char *path,
                                       void (*on_return)(void));
    /* FILES is deliberately an explicit action; replay always stays here. */
    if (stop(true) && ls_scr_files_show_path(chosen.path, return_from_files)) {
      opened = false;
      ls_audio_view_close();
    }
#else
    snprintf(note, sizeof(note), "FILES: %s", strrchr(chosen.path, '/') + 1);
#endif
  }
}
/* ------------------------------------------------------------ shared art -- */

uint8_t ls_calls_tg_hue(uint32_t talkgroup) {
  /* Five hues that read on black and are not an alert: one per talkgroup,
     stable across reboots, so a colour comes to mean a talkgroup. */
  /* Blue only ever bright: plain blue is too dark to read on black. */
  static const uint8_t HUES[] = {TUI_CYAN, TUI_GREEN, TUI_MAGENTA, TUI_YELLOW,
                                 TUI_BLUE | TUI_BRIGHT};
  if (!talkgroup)
    return TUI_GREEN;
  return HUES[(talkgroup * 2654435761u >> 16) % 5u];
}
static int big_advance(char c) {
  return c == '.' || c == ':' ? 2 : c == ' ' ? 2 : 4;
}
int ls_calls_big_width(const char *text) {
  int w = 0;
  for (const char *p = text; p && *p; ++p)
    if (ls_glyph_has(*p) || *p == '.' || *p == ':' || *p == ' ')
      w += big_advance(*p);
  return w > 0 ? w - 1 : 0;
}
/* One character as a dot-matrix sign: each stroke of the 3x5 figure is a
   2x2 block of braille dots, three cells wide and three rows tall, with a
   dot of air above and below. Dots, not blocks: it reads like the LED
   sign on a dispatch console and stays in the house's braille idiom. */
static void dot_char(tui_surface *sf, tui_rect clip, int x, int y,
                     const char *const *fig, int cells, uint8_t attr) {
  for (int cy = 0; cy < LS_CALLS_BIG_ROWS; cy++)
    for (int cx = 0; cx < cells; cx++) {
      unsigned bits = 0;
      for (int dy = 0; dy < 4; dy++)
        for (int dx = 0; dx < 2; dx++) {
          const int px = (cx * 2 + dx) / 2, py = (cy * 4 + dy - 1) / 2;
          if (cy * 4 + dy - 1 < 0 || py >= LS_GLYPH_ROWS || px >= LS_GLYPH_COLS)
            continue;
          if (fig[py][px] != 'X')
            continue;
          bits |= dy < 3 ? 1u << (dx * 3 + dy) : 1u << (6 + dx);
        }
      if (bits)
        ls_braille_glyph_put(sf, clip, x + cx, y + cy, (int16_t)(0x2800 | bits), attr);
    }
}
int ls_calls_big_text(tui_surface *sf, tui_rect clip, int x, int y,
                      const char *text, uint8_t attr) {
  static const char *const DOT[LS_GLYPH_ROWS] = {"   ", "   ", "   ", "   ", "X  "};
  static const char *const COLON[LS_GLYPH_ROWS] = {"   ", "X  ", "   ", "X  ", "   "};
  for (const char *p = text; p && *p; ++p) {
    const char c = *p;
    if (c == '.')
      dot_char(sf, clip, x, y, DOT, 1, attr);
    else if (c == ':')
      dot_char(sf, clip, x, y, COLON, 1, attr);
    else if (ls_glyph_has(c))
      dot_char(sf, clip, x, y, ls_glyph_figure(c), 3, attr);
    else if (c != ' ')
      continue;
    x += big_advance(c);
  }
  return x;
}
int ls_calls_chip(tui_surface *sf, tui_rect clip, int x, int y,
                  const char *text, uint8_t hue) {
  const int n = (int)strlen(text);
  tui_rect chip = tui_rect_intersect(tui_rect_make(x, y, n + 2, 1), clip);
  ls_fill_dither(sf, chip, LS_DITHER_HEAVY, hue);
  tui_put_str(sf, clip, x + 1, y, text,
              TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
  return x + n + 3;
}

/* A ruled band with a title, the CALLS frame: '=' in the accent across the
   top, the title set into it. Lighter than a box, so the cards inside it
   read as the content and the frame as the furniture. */
static void rule(tui_surface *sf, tui_rect r, int y, const char *title,
                 uint8_t hue, bool bright) {
  const uint8_t line = TUI_ATTR(hue, TUI_BLACK);
  for (int x = r.x; x < r.x + r.w; x++)
    tui_put_char(sf, r, x, y, '=', line);
  if (title && r.w > (int)strlen(title) + 6) {
    char cap[48];
    snprintf(cap, sizeof(cap), "[ %s ]", title);
    tui_put_str(sf, r, r.x + 2, y, cap,
                TUI_ATTR(bright ? (TUI_WHITE | TUI_BRIGHT) : (hue | TUI_BRIGHT),
                         TUI_BLACK));
  }
}

/* The part of r clear of the camera hole on the TFT: a hole inside r's
   rows takes the side of r it is nearer. Only what the hole touches moves;
   the screen never shifts (an AMOLED has no hole and draws as before). */
static tui_rect clear_of_hole(tui_rect r) {
  tui_rect hole;
  if (!ls_tui_cutout(&hole) || hole.y >= r.y + r.h || hole.y + hole.h <= r.y ||
      hole.x >= r.x + r.w || hole.x + hole.w <= r.x)
    return r;
  if (hole.x - r.x < r.x + r.w - (hole.x + hole.w)) {
    const int nx = hole.x + hole.w + 1;
    r.w -= nx - r.x;
    r.x = nx;
  } else
    r.w = hole.x - 1 - r.x;
  if (r.w < 0)
    r.w = 0;
  return r;
}
static int clear_x(tui_rect r, int y, int rows) {
  return clear_of_hole(tui_rect_make(r.x, y, r.w, rows)).x;
}

/* ----------------------------------------------------------------- keys -- */

static void button(tui_surface *sf, void *ctx, int id, tui_rect r,
                   const char *label, bool on, bool can) {
  (void)ctx;
  hits[id] = r;
  enabled[id] = can;
  tui_box(sf, r, NULL, TUI_ATTR(can ? TUI_CYAN : LS_FAINT_FG, TUI_BLACK));
  uint8_t ink =
      can ? TUI_ATTR(on ? TUI_BLACK : TUI_WHITE, on ? TUI_CYAN : TUI_BLACK)
          : LS_ATTR_FAINT;
  tui_rect inner = tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2);
  if (inner.h < 1)
    inner = tui_rect_make(r.x + 1, r.y, r.w - 2, 1);
  tui_fill(sf, inner, ' ', ink);
  tui_put_str(sf, inner,
              inner.x + (inner.w - (int)strlen(label) > 0
                             ? (inner.w - (int)strlen(label)) / 2
                             : 0),
              inner.y + inner.h / 2, label, ink);
  if (focus == id)
    tui_put_char(sf, r, r.x, r.y, '>',
                 TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
}

/* A row of keys, each as wide as its label needs and the spare shared out,
   on one line when they fit and two when they do not. Returns rows used. */
typedef struct {
  int id;
  const char *label;
  bool on, can;
} calls_key_t;
/* A key is a thumb wide at least: eight columns, or its label and frame. */
static int key_min(const calls_key_t *k) {
  const int w = (int)strlen(k->label) + 2;
  return w < 8 ? 8 : w;
}
static int key_need(const calls_key_t *k, int n, int gap) {
  int need = gap * (n - 1);
  for (int i = 0; i < n; i++)
    need += key_min(&k[i]);
  return need;
}
static int key_rows_needed(int w, const calls_key_t *k, int n) {
  return key_need(k, n, -1) <= w ? 3 : 6;
}
static void key_row(tui_surface *sf, tui_rect r, const calls_key_t *k, int n) {
  if (n <= 0)
    return;
  if (r.h <= 3)
    r = clear_of_hole(r);
  if (r.h >= 6 && key_rows_needed(r.w, k, n) == 6) {
    const int first = (n + 1) / 2;
    key_row(sf, tui_rect_make(r.x, r.y, r.w, 3), k, first);
    key_row(sf, tui_rect_make(r.x, r.y + 3, r.w, 3), k + first, n - first);
    return;
  }
  /* Keys stand apart when there is a column to spare between each, and
     share their borders like the top bar's when there is not. */
  const int gap = key_need(k, n, 1) <= r.w ? 1 : -1;
  int spare = r.w - key_need(k, n, gap);
  if (spare < 0)
    spare = 0;
  int x = r.x;
  for (int i = 0; i < n; i++) {
    int w = key_min(&k[i]) + spare / n + (i < spare % n ? 1 : 0);
    if (i == n - 1)
      w = r.x + r.w - x;
    button(sf, NULL, k[i].id, tui_rect_make(x, r.y, w, 3), k[i].label, k[i].on,
           k[i].can);
    x += w + gap;
  }
}

/* --------------------------------------------------------------- header -- */

static void draw_header(tui_surface *sf, tui_rect r, const char *title,
                        uint8_t hue) {
  call_archive_storage_t card;
  call_archive_storage(&card);
  char stats[64], space[64];
  LS_SNPRINTF(t_ls_calls_d_calls_1fm_archive, stats, sizeof(stats), "%d CALLS  %.1fM ARCHIVE  %s",
           call_archive_count(), card.archive / 1e6,
           card.paused ? "PAUSED" : "READY");
  if (card.valid)
    snprintf(space, sizeof(space), "CARD %.2fG FREE%s", card.free / 1e9,
             card.paused ? " / SPACE GUARD" : "");
  else
    LS_SNPUTS(t_ls_calls_card_unknown_recording_paused, space, sizeof(space), "CARD UNKNOWN / RECORDING PAUSED");
  const int sw = (int)strlen(stats) > (int)strlen(space) ? (int)strlen(stats)
                                                         : (int)strlen(space);
  const int sx = r.x + r.w - sw - 1;
  /* The emblem: a handset under its own signal, in the receiver's hue. */
  const uint8_t ink = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
  int x = r.x + 1;
  if (sx - x >= 6) {
    tui_put_str(sf, r, x, r.y, "((|))", ink);
    tui_put_str(sf, r, x, r.y + 1, " _|_ ", TUI_ATTR(hue, TUI_BLACK));
    x += 6;
  }
  if (sx - x >= (int)strlen(title) + 2) {
    tui_put_str(sf, r, x, r.y, title, ink);
    for (int i = 0; i < (int)strlen(title); i++)
      tui_put_char(sf, r, x + i, r.y + 1, '=', TUI_ATTR(hue, TUI_BLACK));
  }
  tui_put_str(sf, r, sx, r.y, stats, TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
  tui_put_str(sf, r, sx, r.y + 1, note[0] ? note : space,
              note[0] ? TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
              : card.paused || !card.valid
                  ? TUI_ATTR(TUI_YELLOW, TUI_BLACK)
                  : LS_ATTR_DIM);
}

/* ------------------------------------------------------------ one call -- */

static bool is_p25(const call_meta_t *m) {
  return m->rate == 8000 || m->talkgroup || m->source;
}
static const char *tg_alias(uint32_t tg) {
  return tg && live && live->alias ? live->alias(tg) : NULL;
}
static void duration_text(char *out, size_t n, uint32_t ms) {
  unsigned sec = ms / 1000;
  if (sec < 60)
    snprintf(out, n, "%.1fs", ms / 1000.);
  else if (sec < 6000)
    snprintf(out, n, "%u:%02u", sec / 60, sec % 60);
  else if (sec < 86400)
    snprintf(out, n, "%uh%02u", sec / 3600, sec / 60 % 60);
  else
    snprintf(out, n, "%ud%02u", sec / 86400, sec / 3600 % 24);
}
static void size_text(char *out, size_t n, uint32_t bytes) {
  if (bytes >= 1073741824)
    snprintf(out, n, "%.1fG", bytes / 1073741824.);
  else if (bytes >= 1048576)
    snprintf(out, n, "%.1fM", bytes / 1048576.);
  else
    snprintf(out, n, "%.0fK", bytes / 1024.);
}
/* What a call is called on a card: its talkgroup, or the FM channel. */
static void call_name(const call_meta_t *m, char *out, size_t n) {
  if (is_p25(m))
    snprintf(out, n, "TG %lu", (unsigned long)m->talkgroup);
  else
    snprintf(out, n, "FM %.3f", m->hz / 1e6);
}

/* THE STAGE without a live receiver, and during a replay: the one call this
   screen is about, large, with the player under it. */
/* Rule, the headline and its air, two lines of detail, the audio picture
   while replaying, the progress bar and the transport. */
static int stage_rows(bool big, int audio, int h) {
  return 1 + (big ? LS_CALLS_BIG_ROWS + 2 : 0) + 2 + audio + 2 + (2 * h + 3);
}
static void draw_stage(tui_surface *sf, tui_rect r, bool replaying,
                       const call_entry_t *e, bool big, int audio, int h) {
  const uint8_t hue = e ? (is_p25(&e->meta) ? ls_calls_tg_hue(e->meta.talkgroup)
                                            : TUI_GREEN)
                        : TUI_CYAN;
  bool playing = false, paused = false;
  uint32_t pos = 0, total = e ? e->duration_ms : 0;
  int volume = 0;
  float db = 0;
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
  playing = replay && ls_music_state() == LS_MUSIC_PLAYING;
  paused = replay && ls_music_state() == LS_MUSIC_PAUSED;
  pos = replay ? ls_music_position_ms() : 0;
  if (replay && ls_music_duration_ms())
    total = ls_music_duration_ms();
  volume = ls_music_volume();
  db = ls_music_volume_db();
#endif
  char title[40];
  if (replaying)
    snprintf(title, sizeof(title), "%s", playing ? "REPLAY" : paused ? "PAUSED" : "ENDED");
  else if (row_count)
    snprintf(title, sizeof(title), "CALL %d OF %d", selected + 1, row_count);
  else
    snprintf(title, sizeof(title), "NO CALLS");
  int y = r.y;
  rule(sf, r, y, title, hue, replaying && playing);
  if (e) {
    /* What the call is, as chips on the rule: a P25 call or FM, kept,
       and whether the recording has a gap in it. */
    const char *chips[3];
    uint8_t chip_hue[3];
    int n = 0;
    chips[n] = is_p25(&e->meta) ? "P25" : "FM";
    chip_hue[n++] = is_p25(&e->meta) ? TUI_BLUE : TUI_GREEN;
    if (e->kept) {
      chips[n] = "KEEP";
      chip_hue[n++] = TUI_YELLOW;
    }
    if (e->drops) {
      chips[n] = "GAP";
      chip_hue[n++] = TUI_RED;
    }
    int width = 0;
    for (int i = 0; i < n; i++)
      width += (int)strlen(chips[i]) + 3;
    int cx = r.x + r.w - width - 1;
    if (cx > r.x + (int)strlen(title) + 8)
      for (int i = 0; i < n; i++)
        cx = ls_calls_chip(sf, r, cx, y, chips[i], chip_hue[i]);
  }
  y++;
  char text[96], name[32];
  if (!e) {
    ls_safe_line(sf, r, y + 1,
                 filter ? "Nothing matches this filter" : "No recordings yet",
                 LS_ATTR_DIM);
  } else {
    call_name(&e->meta, name, sizeof(name));
    const char *alias = is_p25(&e->meta) ? tg_alias(e->meta.talkgroup) : NULL;
    bool shouted_alias = false;
    if (big) {
      /* The alias when it fits, else the talkgroup's number: the thing a
         listener says out loud. */
      char shout[24];
      if (alias && ls_calls_big_width(alias) <= r.w - 4) {
        snprintf(shout, sizeof(shout), "%s", alias);
        shouted_alias = true;
      } else if (is_p25(&e->meta))
        snprintf(shout, sizeof(shout), "TG %lu", (unsigned long)e->meta.talkgroup);
      else
        snprintf(shout, sizeof(shout), "%.3f", e->meta.hz / 1e6);
      ls_calls_big_text(sf, r, clear_x(r, y + 1, LS_CALLS_BIG_ROWS) + 2, y + 1, shout,
                        TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
      y += LS_CALLS_BIG_ROWS + 2;
    }
    char stamp[40] = "time unknown";
    struct tm tm;
    if (e->meta.time > 0 && local_tm((time_t)e->meta.time, &tm))
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S local", &tm);
    /* Whatever the headline did not already say. */
    int x = clear_x(r, y, 1) + 2;
    const bool named = big && !shouted_alias;
    if (!named || shouted_alias) {
      tui_put_str(sf, r, x, y, name, TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
      x += (int)strlen(name) + 2;
    }
    if (alias && !shouted_alias && x + (int)strlen(alias) < r.x + r.w - 1) {
      tui_put_str(sf, r, x, y, alias, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
      x += (int)strlen(alias) + 2;
    }
    if (is_p25(&e->meta) && e->meta.source)
      snprintf(text, sizeof(text), "UNIT %lu", (unsigned long)e->meta.source);
    else
      snprintf(text, sizeof(text), "%s  %u Hz", is_p25(&e->meta) ? "P25" : "FM MONO",
               e->meta.rate);
    if (x + (int)strlen(text) < r.x + r.w - 1)
      tui_put_str(sf, r, x, y, text, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    y++;
    char dur[16], size[16];
    duration_text(dur, sizeof(dur), e->duration_ms);
    size_text(size, sizeof(size), e->bytes);
    LS_SNPRINTF(t_ls_calls_s_4f_mhz_s, text, sizeof(text), "%s  %.4f MHz  %s  %s", stamp, e->meta.hz / 1e6,
             dur, size);
    if ((int)strlen(text) > r.w - 3)
      snprintf(text, sizeof(text), "%s  %s", stamp, dur);
    tui_put_str(sf, r, clear_x(r, y, 1) + 2, y, text, LS_ATTR_DIM);
    y++;
  }
  if (audio > 0) {
    /* What is being heard, from the decoded audio itself. */
    tui_rect view = clear_of_hole(tui_rect_make(r.x + 1, y, r.w - 2, audio));
    ls_audio_view_draw(sf, view, 3, 0, 0, playing);
    y += audio;
  }
  /* Centred, so a tap's place on the bar is its place in the call. */
  progress_area = clear_of_hole(tui_rect_make(r.x + 1, y, r.w - 2, 2));
  ls_media_progress(sf, progress_area, pos, total);
  if (replaying && r.w >= 38)
    tui_put_str(sf, progress_area, progress_area.x + 21, progress_area.y,
                "TAP BAR TO SEEK", LS_ATTR_DIM);
  y += 2;
  ls_media_transport(sf, clear_of_hole(tui_rect_make(r.x, y, r.w, 2 * h + 3)), playing, h,
                     row_count > 0, volume, db, button, NULL);
}

/* ----------------------------------------------------------- the cards -- */

/* One recording, two rows: who and how long, then when and where. The
   colour down the left edge is the talkgroup's, the bar under the length is
   the call's length against the longest in view. Selected adds the third row
   the old detail box carried. */
static bool expand_selected;
static int card_height(int index) {
  return index == selected && expand_selected ? 4 : 3;
}
static void draw_card(tui_surface *sf, tui_rect clip, int y,
                      const call_entry_t *e, int index, uint32_t longest,
                      bool fresh) {
  const bool sel = index == selected, p25 = is_p25(&e->meta),
             tall = sel && expand_selected;
  const uint8_t hue = p25 ? ls_calls_tg_hue(e->meta.talkgroup) : TUI_GREEN;
  /* A card the camera hole crosses gives up that side, all its rows. */
  const tui_rect around =
      clear_of_hole(tui_rect_make(clip.x, y, clip.w, tall ? 3 : 2));
  clip = tui_rect_make(around.x, clip.y, around.w, clip.h);
  const int x0 = clip.x, w = clip.w;
  for (int k = 0; k < (tall ? 3 : 2); k++)
    tui_put_char(sf, clip, x0, y + k, LS_TUI_BLOCK_LEFT,
                 TUI_ATTR(hue | (sel || fresh ? TUI_BRIGHT : 0), TUI_BLACK));
  tui_rect line = tui_rect_make(x0 + 1, y, w - 1, 1);
  const uint8_t base = sel ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : TUI_ATTR(TUI_WHITE, TUI_BLACK);
  tui_fill(sf, line, ' ', base);
  char name[32], dur[16], text[96];
  call_name(&e->meta, name, sizeof(name));
  duration_text(dur, sizeof(dur), e->duration_ms);
  int x = x0 + 2;
  if (index == playing_index) {
    tui_put_str(sf, line, x, y, ">", sel ? base : TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    x += 2;
  }
  tui_put_str(sf, line, x, y, name, sel ? base : TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
  x += (int)strlen(name) + 2;
  const char *alias = p25 ? tg_alias(e->meta.talkgroup) : NULL;
  int right = x0 + w - (int)strlen(dur) - 1;
  if (alias) {
    tui_rect room = tui_rect_make(x, y, right - x - 1, 1);
    tui_put_str(sf, room, x, y, alias, sel ? base : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
  }
  tui_put_str(sf, line, right, y, dur,
              sel ? base : TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
  /* Second row: unit and clock on the left, the length bar on the right. */
  char stamp[16] = "--:--";
  struct tm tm;
  if (e->meta.time > 0 && local_tm((time_t)e->meta.time, &tm))
    strftime(stamp, sizeof(stamp), same_day(e->meta.time, time(NULL)) ? "%H:%M:%S" : "%m/%d %H:%M", &tm);
  if (p25)
    snprintf(text, sizeof(text), "%s  U%lu", stamp, (unsigned long)e->meta.source);
  else
    snprintf(text, sizeof(text), "%s  MONO", stamp);
  tui_put_str(sf, clip, x0 + 2, y + 1, text, LS_ATTR_DIM);
  int tags = x0 + w - 1;
  if (e->drops) {
    tags -= 3;
    tui_put_str(sf, clip, tags, y + 1, "GAP", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
    tags--;
  }
  if (e->kept) {
    tags -= 4;
    tui_put_str(sf, clip, tags, y + 1, "KEEP", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
    tags--;
  }
  if (fresh) {
    tags -= 3;
    tui_put_str(sf, clip, tags, y + 1, "NEW", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
    tags--;
  }
  const int bx = x0 + 2 + (int)strlen(text) + 2, bw = tags - bx - 1;
  if (bw >= 6) {
    int fill = longest ? (int)((uint64_t)e->duration_ms * bw / longest) : 0;
    if (fill < 1)
      fill = 1;
    for (int i = 0; i < bw; i++)
      tui_put_char(sf, clip, bx + i, y + 1, i < fill ? '=' : '-',
                   i < fill ? TUI_ATTR(hue | (sel ? TUI_BRIGHT : 0), TUI_BLACK)
                            : LS_ATTR_FAINT);
  }
  if (tall) {
    char full[40] = "time unknown";
    if (e->meta.time > 0 && local_tm((time_t)e->meta.time, &tm))
      strftime(full, sizeof(full), "%Y-%m-%d %H:%M:%S local", &tm);
    char size[16];
    size_text(size, sizeof(size), e->bytes);
    snprintf(text, sizeof(text), "%s  %.4f  %s  %s", full, e->meta.hz / 1e6, size,
             e->meta.gps ? "GPS" : "");
    tui_put_str(sf, clip, x0 + 2, y + 2, text, TUI_ATTR(TUI_CYAN, TUI_BLACK));
  }
}


static void draw_list(tui_surface *sf, tui_rect r) {
  list_area = r;
  card_shown = 0;
  const int64_t newest = archive_newest;
  uint32_t longest = 0;
  for (int i = 0; i < row_count; i++)
    if (rows[i].duration_ms > longest)
      longest = rows[i].duration_ms;
  if (newest_seen && newest > newest_seen)
    ls_fresh_bump(&arrival);
  if (newest > newest_seen)
    newest_seen = newest;
  const bool fresh = arrival.seeded && ls_fresh_level(&arrival, 8000) > 0;
  const uint8_t slide = arrival.seeded ? ls_fresh_level(&arrival, 450) : 0;
  char title[48];
  snprintf(title, sizeof(title), "RECENT %d/%d", row_count ? selected + 1 : 0, row_count);
  rule(sf, r, r.y, title, TUI_CYAN, false);
  if (r.w > 40) {
    char by[16];
    snprintf(by, sizeof(by), " %s ", sorts[order]);
    tui_put_str(sf, r, r.x + r.w - (int)strlen(by) - 2, r.y, by,
                TUI_ATTR(TUI_CYAN, TUI_BLACK));
  }
  tui_rect body = tui_rect_make(r.x, r.y + 1, r.w, r.h - 1);
  if (!row_count) {
    ls_safe_line(sf, body, body.y + 1,
                 filter ? "No matching calls" : "No calls recorded yet", LS_ATTR_DIM);
    ls_safe_line(sf, body, body.y + 2,
                 filter ? "Tap ALL to see every call"
                        : "Calls save here as they are heard",
                 LS_ATTR_FAINT);
    visible = 1;
    return;
  }
  /* How many cards fit with the selection in view, then where to start. */
  int fit = 0, used = 0;
  while (fit < row_count && used + 3 <= body.h) {
    used += 3;
    fit++;
  }
  if (expand_selected && used + 1 > body.h && fit > 1)
    fit--; /* room for the selected card's third row */
  visible = fit < 1 ? 1 : fit;
  top = ls_media_top(selected, row_count, visible, top);
  /* More than fits: a rail down the right edge pages, with a thumb
     between its keys for where in the list this is. */
  if (row_count > visible && body.w >= 40 && body.h >= 8) {
    tui_rect rail = tui_rect_make(body.x + body.w - 8, body.y, 8, body.h);
    body.w -= 9;
    button(sf, NULL, PAGEUP, tui_rect_make(rail.x, rail.y, 8, 3), "^ UP", false,
           selected > 0);
    button(sf, NULL, PAGEDOWN, tui_rect_make(rail.x, rail.y + rail.h - 3, 8, 3),
           "v DN", false, selected + 1 < row_count);
    const int track = rail.h - 6, tx = rail.x + 3;
    if (track > 0) {
      int t0 = top * track / row_count,
          t1 = (top + visible) * track / row_count;
      if (t1 <= t0)
        t1 = t0 + 1;
      tui_rect hole;
      const bool has_hole = ls_tui_cutout(&hole);
      for (int i = 0; i < track; i++) {
        const bool on = i >= t0 && i < t1;
        if (has_hole && tui_rect_contains(hole, tx, rail.y + 3 + i))
          continue;
        /* One dot column: dotted where the list is, solid where the view is. */
        const int16_t cell = (int16_t)(0x2800 | (on ? 0x47 : 0x41));
        ls_braille_put(sf, rail, tx + 1, rail.y + 3 + i, cell,
                       on ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_FAINT);
      }
    }
  }
  const int shift = order == 0 && top == 0 && slide ? 3 * slide / 255 : 0;
  int y = body.y - shift;
  for (int i = top; i < row_count && y + 2 <= body.y + body.h; i++) {
    const int ch = card_height(i);
    draw_card(sf, body, y, &rows[i], i, longest,
              fresh && rows[i].meta.time == newest_seen && order == 0);
    if (card_shown < CARD_SLOTS && y >= body.y) {
      card_hit[card_shown] = tui_rect_make(body.x, y, body.w, ch - 1);
      card_index[card_shown++] = i;
    }
    y += ch;
  }
}

/* ---------------------------------------------------------------- draw -- */

void ls_calls_draw(tui_surface *sf, tui_rect area) {
  memset(hits, 0, sizeof(hits));
  memset(enabled, 0, sizeof(enabled));
  strip_area = tui_rect_make(0, -1, 0, 0);
  list_area = progress_area = tui_rect_make(0, -1, 0, 0);
  load_rows();
  const bool wide = area.w > area.h * 2;
  const char *title = live && live->title ? live->title : "CALLS";
  const uint8_t hue = live ? live->hue : TUI_CYAN;
  draw_header(sf, tui_rect_make(area.x, area.y, area.w, 2), title, hue);
  tui_rect body = tui_rect_make(area.x, area.y + 2, area.w, area.h - 2);
  const call_entry_t *sel = row_count ? &rows[selected] : NULL;
  /* The replayed call, pinned: the list may move under it. */
  const call_entry_t *shown = sel;
  if (replay && playing_index >= 0)
    shown = &rows[playing_index];

  /* Left (or top): what this screen is about now. Right (or below): the
     filters, the recordings and the actions on them. */
  tui_rect stage, side;
  if (wide) {
    int lw = area.w * 46 / 100;
    if (lw < 30)
      lw = 30;
    if (lw > 60)
      lw = 60;
    stage = tui_rect_make(body.x, body.y, lw, body.h);
    side = tui_rect_make(body.x + lw + 1, body.y, body.w - lw - 1, body.h);
  } else {
    stage = side = body;
  }

  /* The actions under everything on the side. */
  calls_key_t act[6] = {
      {STOP, "STOP", false, replay},
      {KEEP, row_count && rows[selected].kept ? "UNKEEP" : "KEEP", false, row_count > 0},
      {DELETE, "DELETE", false, row_count > 0},
      {FILES, "FILES", false, row_count > 0},
      {OPTIONS, "OPTIONS", false, true},
      {BACK, "BACK", false, true},
  };
  const int act_h = key_rows_needed(side.w, act, 6);
  /* Filters, sort and paging: one row where they fit, else two. */
  char tg_label[16];
  if (filter_tg)
    snprintf(tg_label, sizeof(tg_label), "TG %lu", (unsigned long)filter_tg);
  else
    snprintf(tg_label, sizeof(tg_label), "TG");
  calls_key_t chip[6] = {
      {ALL, "ALL", filter == 0, true},
      {P25, "P25", filter == 1, true},
      {FM, "FM", filter == 2, true},
      {TODAY, "TODAY", filter == 3, true},
      {TG, tg_label, filter == 4,
       filter_tg || (row_count && rows[selected].meta.talkgroup)},
      {SORT, sorts[order], false, true},
  };
  const int chips_h = key_rows_needed(side.w, chip, 6);

  const int h = 3;
  bool live_stage = live && live->card_draw && !replay;
  int live_card = live_stage && live->card_rows ? live->card_rows(stage.w) : 0;
  if (live_card <= 0)
    live_stage = false;
  /* With a live card on stage, the selected recording's detail moves into
     its card in the list. */
  expand_selected = live_stage;
  int stage_h, strip_h = 0, audio = 0;
  bool big = true;
  if (live_stage) {
    stage_h = live_card + 3;
  } else {
    /* What the side must keep below the stage in portrait: the chips, a
       few cards and the actions. */
    const int room = wide ? stage.h : stage.h - act_h - chips_h - 10;
    stage_h = stage_rows(true, 0, h);
    if (stage_h > room) {
      big = false;
      stage_h = stage_rows(false, 0, h);
    }
    if (replay) {
      audio = room - stage_h - (wide ? 0 : 8);
      if (audio > 10)
        audio = 10;
      if (audio < 4)
        audio = 0;
      stage_h += audio;
    }
  }
  if (wide && stage_h > stage.h)
    stage_h = stage.h;

  /* Draw the stage. */
  tui_rect st = tui_rect_make(stage.x, stage.y, stage.w, stage_h);
  if (live_stage) {
    live->card_draw(sf, tui_rect_make(st.x, st.y, st.w, live_card));
    ls_calls_live_key_t lk[2];
    int n = live->keys ? live->keys(lk) : 0;
    if (n > 2)
      n = 2;
    calls_key_t k[3];
    int m = 0;
    for (int i = 0; i < n; i++)
      k[m++] = (calls_key_t){LIVE0 + i, lk[i].label, lk[i].on, lk[i].can};
    k[m++] = (calls_key_t){PLAY, "PLAY", false, row_count > 0};
    key_row(sf, tui_rect_make(st.x, st.y + live_card, st.w, 3), k, m);
  } else {
    draw_stage(sf, st, replay, shown, big, audio, h);
  }

  /* The talkgroup strip: under the stage, in whatever the stage left. */
  int y = st.y + st.h;
  if (wide) {
    if (live && live->strip_draw && live->strip_rows && !replay) {
      strip_h = live->strip_rows(stage.w, stage.y + stage.h - y);
      if (strip_h > 0) {
        strip_area = tui_rect_make(stage.x, y, stage.w, strip_h);
        live->strip_draw(sf, strip_area);
      }
    }
  }

  /* The side: chips, list, actions. */
  int sy = wide ? side.y : y;
  const int bottom = side.y + side.h;
  key_row(sf, tui_rect_make(side.x, bottom - act_h, side.w, act_h), act, 6);
  int list_h = bottom - act_h - sy - chips_h;
  if (!wide && live && live->strip_draw && live->strip_rows && !replay) {
    int most = list_h - 10;
    if (most > 0) {
      strip_h = live->strip_rows(side.w, most);
      if (strip_h > 0) {
        strip_area = tui_rect_make(side.x, sy, side.w, strip_h);
        live->strip_draw(sf, strip_area);
        sy += strip_h;
        list_h -= strip_h;
      }
    }
  }
  key_row(sf, tui_rect_make(side.x, sy, side.w, chips_h), chip, 6);
  sy += chips_h;
  if (list_h >= 3)
    draw_list(sf, tui_rect_make(side.x, sy, side.w, list_h));
}

static void seek(int delta) {
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
  if (!replay)
    return;
  int64_t target = (int64_t)ls_music_position_ms() + delta;
  uint32_t total = ls_music_duration_ms();
  if (target < 0)
    target = 0;
  if (target > total)
    target = total;
  if (!ls_music_seek_ms((uint32_t)target))
    LS_SNPUTS(t_ls_calls_seek_unavailable_decoder_busy, note, sizeof(note), "Seek unavailable / decoder busy");
  else
    ls_audio_view_reset();
#else
  (void)delta;
#endif
}
bool ls_calls_key(ls_tk_t key, char ch) {
  if (key >= LS_TK_F1)
    return false;
  if (key == LS_TK_ESC || key == LS_TK_BACKSPACE) {
    action(BACK);
    return true;
  }
  if (key == LS_TK_TAB) {
    for (int i = 0; i < ACTIONS; i++) {
      focus = (focus + 1) % ACTIONS;
      if (enabled[focus])
        break;
    }
    return true;
  }
  if (key == LS_TK_UP || key == LS_TK_DOWN) {
    focus = -1;
    if (key == LS_TK_UP && selected > 0)
      --selected;
    if (key == LS_TK_DOWN && selected + 1 < row_count)
      ++selected;
    return true;
  }
  if (key == LS_TK_LEFT || key == LS_TK_RIGHT) {
    action(key == LS_TK_LEFT ? PREV : NEXT);
    return true;
  }
  if (key == LS_TK_ENTER) {
    if (focus >= 0)
      action(focus);
    else
      play_selected();
    return true;
  }
  if (key != LS_TK_CHAR)
    return false;
  switch (tolower((unsigned char)ch)) {
  case 'p':
  case ' ':
    action(PLAY);
    break;
  case 'n':
    action(NEXT);
    break;
  case ',':
    action(PREV);
    break;
  case 's':
    action(STOP);
    break;
  case 'd':
    action(DELETE);
    break;
  case 'o':
    action(OPTIONS);
    break;
  case 'b':
    action(BACK);
    break;
  case 'k':
    action(KEEP);
    break;
  case 'f':
    action(FILES);
    break;
  case 't':
    action(SORT);
    break;
  case 'u':
    action(PAGEUP);
    break;
  case 'j':
    action(PAGEDOWN);
    break;
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
    action(ALL + ch - '1');
    break;
  case '-':
  case '<':
    action(QUIETER);
    break;
  case '+':
  case '=':
  case '>':
    action(LOUDER);
    break;
  case 'm':
    action(MUTE);
    break;
  case '[':
    seek(-5000);
    break;
  case ']':
    seek(5000);
    break;
  case 'r':
    call_archive_refresh();
    break;
  case 'h':
    action(LIVE0);
    break;
  case 'l':
    action(LIVE1);
    break;
  default:
    return false;
  }
  return true;
}
bool ls_calls_touch(int col, int row) {
  for (int i = 0; i < ACTIONS; i++)
    if (enabled[i] && tui_rect_contains(hits[i], col, row)) {
      action(i);
      return true;
    }
  if (live && live->strip_hit && tui_rect_contains(strip_area, col, row)) {
    const uint32_t tg = live->strip_hit(col, row);
    if (tg)
      ls_calls_show_talkgroup(tg == filter_tg && filter == 4 ? 0 : tg);
    return true;
  }
  for (int k = 0; k < card_shown; k++)
    if (tui_rect_contains(card_hit[k], col, row)) {
      const int i = card_index[k];
      if (i == selected)
        play_selected();
      else
        selected = i;
      focus = -1;
      return true;
    }
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
  if (replay && (row == progress_area.y || row == progress_area.y + 1) &&
      col >= progress_area.x + 2 &&
      col < progress_area.x + progress_area.w - 2) {
    uint32_t target = (uint64_t)(col - progress_area.x - 2) *
                      ls_music_duration_ms() / (progress_area.w - 4);
    if (ls_music_seek_ms(target))
      ls_audio_view_reset();
  }
#endif
  return true;
}
