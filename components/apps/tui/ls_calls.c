#include "ls_calls.h"
#include "call_archive.h"
#include "ls_audio_view.h"
#include "ls_media_widgets.h"
#include "ls_motion.h"
#include "ls_music_backend.h"
#include "ls_picker.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static bool opened, replay;
static int selected, top, visible, row_height;
static const char *return_receiver;
static tui_rect list_area, progress_area;
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
static const char *const filters[] = {"ALL", "P25", "FM", "TODAY", "TG"};
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
    snprintf(note, sizeof(note),
             "Use up to 32 talkgroups, 1..65535, separated by commas");
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
  time_t now = time(NULL);
  for (int i = 0; i < call_archive_count() && i < CALL_RECENT_MAX; i++) {
    if (!call_archive_entry(i, &candidate))
      break;
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
void ls_calls_open(const char *receiver) {
  call_archive_init();
  call_archive_refresh();
  return_receiver = receiver;
  opened = true;
  filter = order = 0;
  selected = top = row_count = 0;
  focus = -1;
  note[0] = 0;
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
  if (id >= ALL && id <= TG) {
    if (id == TG) {
      if ((!row_count || !rows[selected].meta.talkgroup) && !filter_tg) {
        snprintf(note, sizeof(note), "Select a P25 talkgroup first");
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
    ls_picker_add("DELETE", "Remove WAV and metadata");
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
static void draw_detail(tui_surface *sf, tui_rect r) {
  tui_box(sf, r, "SELECTED CALL", TUI_ATTR(TUI_CYAN, TUI_BLACK));
  if (!row_count) {
    ls_safe_line(sf, r, r.y + 1, "No matching recordings", LS_ATTR_DIM);
    return;
  }
  const call_entry_t *e = &rows[selected];
  char text[128], stamp[40] = "Time unknown";
  struct tm tm;
  if (e->meta.time > 0 && local_tm((time_t)e->meta.time, &tm))
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S local", &tm);
  ls_safe_line(sf, r, r.y + 1, stamp, LS_ATTR_DIM);
  bool p25 = e->meta.rate == 8000 || e->meta.talkgroup || e->meta.source;
  if (p25)
    snprintf(text, sizeof(text), "P25 %.4f MHz TG %lu", e->meta.hz / 1e6,
             (unsigned long)e->meta.talkgroup);
  else
    snprintf(text, sizeof(text), "FM %.4f MHz", e->meta.hz / 1e6);
  ls_safe_line(sf, r, r.y + 2, text, LS_ATTR_DIM);
  if (p25)
    snprintf(text, sizeof(text), "UNIT %lu  %.1fs  %.1fK",
             (unsigned long)e->meta.source, e->duration_ms / 1000.,
             e->bytes / 1024.);
  else
    snprintf(text, sizeof(text), "%.1fs  %.1f KiB  MONO",
             e->duration_ms / 1000., e->bytes / 1024.);
  ls_safe_line(sf, r, r.y + 3, text, LS_ATTR_DIM);
  snprintf(text, sizeof(text), "%u Hz %s %s UTC%+.2f", e->meta.rate,
           e->kept ? "KEEP" : "", e->meta.gps ? "GPS fix" : "No GPS",
           call_archive_option(CALL_OPT_UTC_QUARTERS) / 4.0);
  ls_safe_line(sf, r, r.y + 4, text, LS_ATTR_DIM);
  if (r.h >= 7) {
    if (e->meta.gps)
      snprintf(text, sizeof(text), "GPS %.5f %.5f", e->meta.lat, e->meta.lon);
    else
      snprintf(text, sizeof(text), "GPS: not stored");
    ls_safe_line(sf, r, r.y + 5, text, LS_ATTR_DIM);
  }
}
void ls_calls_draw(tui_surface *sf, tui_rect area) {
  memset(hits, 0, sizeof(hits));
  memset(enabled, 0, sizeof(enabled));
  load_rows();
  call_archive_storage_t card;
  call_archive_storage(&card);
  char text[128];
  bool wide = area.w > area.h * 2;
  int header = 3, chips = 3, actions = wide ? 3 : 6;
  snprintf(text, sizeof(text), "%d CALLS  %.1fM ARCHIVE  %s",
           call_archive_count(), card.archive / 1e6,
           card.paused ? "PAUSED" : "READY");
  ls_safe_line(sf, area, area.y, text,
               TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
  if (card.valid)
    snprintf(text, sizeof(text), "CARD %.2fG FREE%s", card.free / 1e9,
             card.paused ? " / SPACE GUARD" : "");
  else
    snprintf(text, sizeof(text), "CARD UNKNOWN / RECORDING PAUSED");
  ls_safe_line(sf, area, area.y + 1, text, LS_ATTR_DIM);
  ls_safe_line(sf, area, area.y + 2,
               note[0] ? note : "ENTER/tap row play  P pause  [ ] seek",
               LS_ATTR_DIM);
  int cw = area.w / 5;
  for (int i = 0; i < 5; i++) {
    char label[24];
    if (i == 4 && filter_tg)
      snprintf(label, sizeof(label), "TG %lu", (unsigned long)filter_tg);
    else
      snprintf(label, sizeof(label), "%s", filters[i]);
    button(sf, NULL, ALL + i,
           tui_rect_make(area.x + i * cw, area.y + header,
                         i == 4 ? area.w - i * cw : cw - 1, chips),
           label, filter == i,
           i != 4 || filter_tg || (row_count && rows[selected].meta.talkgroup));
  }
  int body_y = area.y + header + chips,
      body_h = area.h - header - chips - actions;
  tui_rect data, media;
  if (wide) {
    int split = area.w * 55 / 100;
    data = tui_rect_make(area.x, body_y, split - 1, body_h);
    media = tui_rect_make(area.x + split, body_y, area.w - split, body_h);
  } else {
    int dh = body_h / 2;
    if (dh < 12)
      dh = 12;
    data = tui_rect_make(area.x, body_y, area.w, dh);
    media = tui_rect_make(area.x, body_y + dh, area.w, body_h - dh);
  }
  int nav_w = data.w / 3;
  button(sf, NULL, SORT, tui_rect_make(data.x, data.y, nav_w, 3), sorts[order],
         false, true);
  button(sf, NULL, PAGEUP, tui_rect_make(data.x + nav_w, data.y, nav_w - 1, 3),
         "PAGE UP", false, selected > 0);
  button(sf, NULL, PAGEDOWN,
         tui_rect_make(data.x + 2 * nav_w, data.y, data.w - 2 * nav_w, 3),
         "PAGE DN", false, selected + 1 < row_count);
  int detail_h = data.h >= 17 ? 7 : data.h >= 14 ? 6 : data.h >= 12 ? 5 : 4;
  list_area = tui_rect_make(data.x, data.y + 3, data.w, data.h - 3 - detail_h);
  tui_box(sf, list_area, "RECORDINGS", TUI_ATTR(TUI_CYAN, TUI_BLACK));
  int widths[] = {12, list_area.w - 29, 5, 5};
  if (widths[1] < 6)
    widths[1] = 6;
  const char *head[] = {"LOCAL TIME", "SOURCE", "DUR", "SIZE"};
  ls_media_row(
      sf, tui_rect_make(list_area.x + 1, list_area.y + 1, list_area.w - 2, 1),
      head, widths, 4, false);
  row_height = list_area.h >= 7 ? 2 : 1;
  visible = (list_area.h - 3) / row_height;
  if (visible < 1)
    visible = 1;
  top = ls_media_top(selected, row_count, visible, top);
  for (int i = top; i < row_count && i < top + visible; i++) {
    const call_entry_t *e = &rows[i];
    char stamp[20] = "--/-- --:--", src[32], seconds[16], size[16];
    struct tm tm;
    if (e->meta.time > 0 && local_tm((time_t)e->meta.time, &tm))
      strftime(stamp, sizeof(stamp), "%m/%d %H:%M", &tm);
    if (e->meta.rate == 8000 || e->meta.talkgroup || e->meta.source)
      snprintf(src, sizeof(src), widths[1] >= 22 ? "TG %lu /U%lu" : "TG %lu",
               (unsigned long)e->meta.talkgroup, (unsigned long)e->meta.source);
    else
      snprintf(src, sizeof(src), "FM %.3f", e->meta.hz / 1e6);
    unsigned sec = e->duration_ms / 1000;
    if (sec < 60)
      snprintf(seconds, sizeof(seconds), "%.1fs", e->duration_ms / 1000.);
    else if (sec < 6000)
      snprintf(seconds, sizeof(seconds), "%02u:%02u", sec / 60, sec % 60);
    else if (sec < 86400)
      snprintf(seconds, sizeof(seconds), "%uh%02u", sec / 3600, sec / 60 % 60);
    else
      snprintf(seconds, sizeof(seconds), "%ud%02u", sec / 86400,
               sec / 3600 % 24);
    if (e->bytes >= 1073741824)
      snprintf(size, sizeof(size), "%.1fG", e->bytes / 1073741824.);
    else if (e->bytes >= 1048576)
      snprintf(size, sizeof(size), "%.1fM", e->bytes / 1048576.);
    else
      snprintf(size, sizeof(size), "%.0fK", e->bytes / 1024.);
    int marks = (e->kept ? 1 : 0) + (e->drops ? 1 : 0);
    if (marks && strlen(src) + marks < sizeof(src)) {
      memmove(src + marks, src, strlen(src) + 1);
      int at = 0;
      if (e->kept)
        src[at++] = '*';
      if (e->drops)
        src[at] = '!';
    }
    const char *cols[] = {stamp, src, seconds, size};
    ls_media_row(sf,
                 tui_rect_make(list_area.x + 1,
                               list_area.y + 2 + (i - top) * row_height,
                               list_area.w - 2, row_height),
                 cols, widths, 4, i == selected);
  }
  snprintf(text, sizeof(text), " %d/%d *KEEP !GAP ",
           row_count ? selected + 1 : 0, row_count);
  tui_put_str(sf, list_area, list_area.x + list_area.w - (int)strlen(text) - 1,
              list_area.y + list_area.h - 1, text, LS_ATTR_DIM);
  if (!row_count)
    ls_safe_line(sf, list_area, list_area.y + 2, "No matching calls",
                 LS_ATTR_DIM);
  draw_detail(
      sf, tui_rect_make(data.x, data.y + data.h - detail_h, data.w, detail_h));
  bool playing = false, paused = false;
  uint32_t pos = 0, total = 0;
  int volume = 0;
  float db = 0;
#if defined(ESP_PLATFORM) || defined(LS_CALLS_HOST_AUDIO)
  playing = replay && ls_music_state() == LS_MUSIC_PLAYING;
  paused = replay && ls_music_state() == LS_MUSIC_PAUSED;
  pos = replay ? ls_music_position_ms() : 0;
  total = replay ? ls_music_duration_ms() : 0;
  volume = ls_music_volume();
  db = ls_music_volume_db();
#endif
  if (!total && row_count)
    total = rows[selected].duration_ms;
  int h = media.h >= 19 && !wide ? 4 : media.h >= 14 ? 3 : 2;
  int transport_h = 2 * h + 3;
  int vh = media.h - transport_h - 2;
  if (vh < 1)
    vh = 1;
  tui_rect visual = tui_rect_make(media.x, media.y, media.w, vh);
  if (replay) {
    const char *state = playing ? "PLAY" : paused ? "PAUSE" : "ENDED";
    if (playing_meta.rate == 8000 || playing_meta.talkgroup ||
        playing_meta.source)
      snprintf(text, sizeof(text), "WATERFALL %s TG %lu", state,
               (unsigned long)playing_meta.talkgroup);
    else
      snprintf(text, sizeof(text), "WATERFALL %s FM %.3f", state,
               playing_meta.hz / 1e6);
  } else
    snprintf(text, sizeof(text), "WATERFALL / READY");
  tui_box(sf, visual, text, TUI_ATTR(TUI_CYAN, TUI_BLACK));
  ls_audio_view_draw(
      sf, tui_rect_make(visual.x + 1, visual.y + 1, visual.w - 2, visual.h - 2),
      3, 0, 0, playing);
  progress_area = tui_rect_make(media.x, media.y + vh, media.w, 2);
  ls_media_progress(sf, progress_area, pos, total);
  if (media.w >= 38)
    tui_put_str(sf, progress_area, progress_area.x + 21, progress_area.y,
                "TAP BAR TO SEEK", LS_ATTR_DIM);
  ls_media_transport(
      sf, tui_rect_make(media.x, media.y + vh + 2, media.w, transport_h),
      playing, h, row_count > 0, volume, db, button, NULL);
  const char *labels[] = {"STOP", "DELETE", "OPTIONS", "BACK", "FILES", "KEEP"};
  int columns = wide ? 6 : 3, aw = area.w / columns;
  for (int i = 0; i < 6; i++)
    button(sf, NULL, i == 5 ? KEEP : STOP + i,
           tui_rect_make(area.x + (i % columns) * aw,
                         area.y + area.h - actions + (i / columns) * 3,
                         (i % columns) == columns - 1
                             ? area.w - (i % columns) * aw
                             : aw - 1,
                         3),
           i == 5 && row_count && rows[selected].kept ? "UNKEEP" : labels[i],
           false,
           i == 0                       ? replay
           : i == 1 || i == 4 || i == 5 ? row_count > 0
                                        : true);
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
    snprintf(note, sizeof(note), "Seek unavailable / decoder busy");
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
  if (tui_rect_contains(list_area, col, row) && row >= list_area.y + 2 &&
      row < list_area.y + list_area.h - 1) {
    int i = top + (row - list_area.y - 2) / row_height;
    if (i < row_count) {
      if (i == selected)
        play_selected();
      else
        selected = i;
      focus = -1;
    }
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
