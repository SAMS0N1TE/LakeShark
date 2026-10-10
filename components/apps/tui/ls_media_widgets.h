#ifndef LS_MEDIA_WIDGETS_H
#define LS_MEDIA_WIDGETS_H
#include "ls_music_backend.h"
#include "ls_music_transport.h"
#include "ls_tui_ui.h"
#include <stdio.h>
#include <string.h>
/* Shared viewport, ordered insertion, clipped columns and selection ink. */
static inline int ls_media_top(int selected, int count, int rows, int top) {
  return ls_music_scroll(selected, count, rows, top);
}
static inline int ls_media_insert(void *rows, int *count, int capacity,
                                  size_t size, const void *row,
                                  int (*cmp)(const void *, const void *)) {
  int lo = 0, hi = *count;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (cmp(row, (char *)rows + mid * size) < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  if (lo >= capacity)
    return -1;
  if (*count < capacity)
    ++*count;
  memmove((char *)rows + (lo + 1) * size, (char *)rows + lo * size,
          (*count - lo - 1) * size);
  memcpy((char *)rows + lo * size, row, size);
  return lo;
}
static inline void ls_media_row_tinted(tui_surface *sf, tui_rect r,
                                       const char *const *column,
                                       const int *width, int count,
                                       bool selected, uint8_t hue) {
  uint8_t ink =
      selected ? TUI_ATTR(TUI_BLACK, TUI_CYAN) : TUI_ATTR(hue, TUI_BLACK);
  tui_fill(sf, r, ' ', ink);
  int x = r.x;
  for (int i = 0; i < count && x < r.x + r.w; i++) {
    int w = width[i];
    if (w > r.x + r.w - x)
      w = r.x + r.w - x;
    tui_rect cell = tui_rect_make(x, r.y, w, r.h);
    tui_put_str(sf, cell, x, r.y, column[i], ink);
    x += width[i] + 1;
  }
}
static inline void ls_media_row(tui_surface *sf, tui_rect r,
                                const char *const *column, const int *width,
                                int count, bool selected) {
  ls_media_row_tinted(sf, r, column, width, count, selected, TUI_WHITE);
}
static inline void ls_media_clock(tui_surface *sf, tui_rect r, uint32_t elapsed,
                                  uint32_t total) {
  char clock[48];
  snprintf(clock, sizeof(clock), "%02u:%02u / %02u:%02u", elapsed / 60000,
           elapsed / 1000 % 60, total / 60000, total / 1000 % 60);
  ls_safe_line(sf, r, r.y, clock, TUI_ATTR(TUI_WHITE, TUI_BLACK));
}
static inline void ls_media_progress_bar(tui_surface *sf, tui_rect r,
                                         uint32_t elapsed, uint32_t total) {
  int width = r.w - 4;
  if (width < 1 || r.h < 2)
    return;
  int filled =
      total
          ? (int)((uint64_t)(elapsed > total ? total : elapsed) * width / total)
          : 0;
  for (int x = 0; x < width; x++)
    tui_put_char(
        sf, r, r.x + 2 + x, r.y + 1, x < filled ? LS_TUI_BLOCK_LOWER : '-',
        x < filled ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_DIM);
}
static inline void ls_media_progress(tui_surface *sf, tui_rect r,
                                     uint32_t elapsed, uint32_t total) {
  ls_media_clock(sf, r, elapsed, total);
  ls_media_progress_bar(sf, r, elapsed, total);
}
static inline void ls_media_volume_step(int direction) {
  int v = ls_music_volume(),
      target = direction > 0 ? (v / 5 + 1) * 5 : ((v + 4) / 5 - 1) * 5;
  if (target < 0)
    target = 0;
  if (target > 100)
    target = 100;
  if (target != v)
    ls_music_volume_step(target - v);
}
typedef void (*ls_media_button_fn)(tui_surface *, void *, int, tui_rect,
                                   const char *, bool, bool);
/* IDs: prev, play, next, quieter, mute, louder. Draw and hit geometry agree. */
static inline void ls_media_transport(tui_surface *sf, tui_rect r, bool playing,
                                      int h, bool can, int volume, float db,
                                      ls_media_button_fn draw, void *ctx) {
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
  draw(sf, ctx, 0, tui_rect_make(r.x, r.y, q - 1, h), "PREV", false, can);
  draw(sf, ctx, 1, tui_rect_make(r.x + q, r.y, r.w - 2 * q, h),
       playing ? "PAUSE" : "PLAY", true, can);
  draw(sf, ctx, 2, tui_rect_make(r.x + r.w - q + 1, r.y, q - 1, h), "NEXT",
       false, can);
  int value = volume;
  if (value < 0)
    value = 0;
  if (value > 100)
    value = 100;
  char vol[48];
  if (value)
    snprintf(vol, sizeof(vol), "VOLUME %d%%  %.1f dB", value, (double)db);
  else
    snprintf(vol, sizeof(vol), "VOLUME 0%%  MUTED");
  ls_safe_line(sf, r, r.y + h, vol, TUI_ATTR(TUI_CYAN, TUI_BLACK));
  /* One segment per five-point step. */
  int gx = r.x + (r.w - 20) / 2;
  for (int i = 0; i < 20; i++)
    tui_put_char(sf, r, gx + i, r.y + h + 1, LS_TUI_BLOCK_LEFT,
                 i < value / 5 ? TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK)
                               : TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
  int y = r.y + h + 3;
  draw(sf, ctx, 3, tui_rect_make(r.x, y, t - 1, h), "VOL -", false, value > 0);
  draw(sf, ctx, 4, tui_rect_make(r.x + t, y, r.w - 2 * t, h),
       value > 0 ? "MUTE" : "UNMUTE", false, true);
  draw(sf, ctx, 5, tui_rect_make(r.x + r.w - t + 1, y, t - 1, h), "VOL +",
       false, value < 100);
}
#endif
