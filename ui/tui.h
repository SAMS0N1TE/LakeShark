/*
 * tui.h - shared terminal UI surface.
 *
 * Apps render PAGE_MAIN content through their app_t::draw_main callback.
 * The page bar, header, footer, and waterfall/log/settings pages are
 * owned by tui.c. All output is 24-bit truecolour ANSI; box drawing
 * uses raw UTF-8 bytes for the light-line set.
 */
#ifndef TUI_H
#define TUI_H

#include <stdint.h>
#include <stdbool.h>
#include "app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TUI_COLS 128
#define TUI_ROWS 38

void tui_mark_dirty(void);
void tui_draw(void);
void tui_init(void);
void tui_install_log_hook(void);

#define CLS       "\033[2J\033[H"
#define RESET     "\033[0m"
#define BOLD      "\033[1m"
#define DIM       "\033[2m"
#define EL        "\033[K"

#define C_BG_ACT  "\033[48;2;0;0;128m"
#define C_BG_STRP "\033[48;2;16;20;32m"
#define C_BORDER  "\033[38;2;60;100;180m"
#define C_BORD_HI "\033[38;2;100;160;240m"
#define C_LABEL   "\033[38;2;120;140;170m"
#define C_TEXT    "\033[38;2;210;220;235m"
#define C_BRIGHT  "\033[38;2;255;255;255m"
#define C_CYAN    "\033[38;2;80;220;255m"
#define C_GREEN   "\033[38;2;80;255;120m"
#define C_AMBER   "\033[38;2;255;180;0m"
#define C_RED     "\033[38;2;255;70;70m"
#define C_MAGENTA "\033[38;2;220;130;255m"
#define C_BLUE    "\033[38;2;100;170;255m"
#define C_GOLD    "\033[38;2;255;215;80m"
#define C_DIM     "\033[38;2;90;100;120m"

/* Box-drawing - light Unicode set. Stored as UTF-8 hex escapes because
 * raw bytes confuse some Windows toolchains during compile. */
#define HL    "\xe2\x94\x80"
#define VL    "\xe2\x94\x82"
#define TL    "\xe2\x94\x8c"
#define TR    "\xe2\x94\x90"
#define BL    "\xe2\x94\x94"
#define BR    "\xe2\x94\x98"
#define TR_   "\xe2\x94\x9c"
#define TL_   "\xe2\x94\xa4"
#define T_UP  "\xe2\x94\xb4"
#define T_DN  "\xe2\x94\xac"
#define CROSS "\xe2\x94\xbc"

#define TUI_LOG_LINES 200
void tui_log(int color_index, const char *fmt, ...);

void tui_waterfall_push(const uint16_t *mag, int mag_len);
void tui_waterfall_reset(void);

bool settings_handle_key(tui_key_t k);
bool settings_is_editing(void);

void tui_goto(int row, int col);
void tui_hline(int row, int col, int n);
void tui_pad(const char *s, int width);

#ifdef __cplusplus
}
#endif

#endif
