/* The LakeShark Studio window: Studio runs on the PC (app-sdk/lsdk.py
   studio, or the desktop bridge's /studio/), so this says how to open it,
   and its key asks the PC to open it: the board sends "studio: open" on the
   USB feed between two screen updates, and the bridge (host/lsterm/lsweb.py)
   opens /studio/ in its own window. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>

#include "../ls_braille.h"
#include "../ls_rterm.h"
#include "esp_timer.h"

static int64_t s_asked_us;            /* when the key was last pressed, for its note */

static uint8_t on_face(uint8_t fg) { return (uint8_t)((dk_attr(R_FACE) & 0xF0) | (fg & 0x0F)); }
static uint8_t hue(uint8_t light, uint8_t dark) { return on_face(dk_light_face() ? light : dark); }

static tui_rect open_rect(tui_rect c) { return tui_rect_make(c.x + 2, c.y + c.h - 3, 30, 3); }
static tui_rect ok_rect(tui_rect c) { return tui_rect_make(c.x + c.w - 16, c.y + c.h - 3, 14, 3); }

/* A monitor with an app's first lines on it, and its stand. */
static void art(tui_surface *sf, tui_rect clip, int x, int y)
{
    const uint8_t hi = dk_attr(R_HILITE), lo = dk_attr(R_SHADOW);
    ls_box_put(sf, clip, x, y, LS_BOX_ARC_DR, hi);
    dk_hline(sf, clip, x + 1, y, 14, LS_BOX_H, hi);
    ls_box_put(sf, clip, x + 15, y, LS_BOX_ARC_DL, lo);
    for (int r = 1; r <= 5; r++) {
        ls_box_put(sf, clip, x, y + r, LS_BOX_V, hi);
        ls_box_put(sf, clip, x + 15, y + r, LS_BOX_V, lo);
    }
    ls_box_put(sf, clip, x, y + 6, LS_BOX_ARC_UR, hi);
    dk_hline(sf, clip, x + 1, y + 6, 14, LS_BOX_H, lo);
    ls_box_put(sf, clip, x + 15, y + 6, LS_BOX_ARC_UL, lo);
    const uint8_t kw = hue(TUI_BLUE, TUI_CYAN | TUI_BRIGHT), fn = hue(TUI_BLACK, TUI_WHITE | TUI_BRIGHT);
    const uint8_t st = hue(TUI_RED, TUI_YELLOW | TUI_BRIGHT), cm = hue(TUI_GREEN, TUI_GREEN | TUI_BRIGHT);
    dk_text(sf, clip, x + 2, y + 1, "#include", hue(TUI_RED, TUI_RED | TUI_BRIGHT));
    dk_text(sf, clip, x + 2, y + 2, "static", kw);
    dk_text(sf, clip, x + 9, y + 2, "draw", fn);
    dk_text(sf, clip, x + 3, y + 3, "lap_put", fn);
    dk_text(sf, clip, x + 10, y + 3, "(\"hi", st);
    dk_text(sf, clip, x + 2, y + 4, "LAP_APP(..)", kw);
    dk_text(sf, clip, x + 2, y + 5, "/* F5 run */", cm);
    ls_box_put(sf, clip, x + 7, y + 7, LS_BOX_V, lo);
    ls_box_put(sf, clip, x + 8, y + 7, LS_BOX_V, lo);
    dk_hline(sf, clip, x + 4, y + 8, 8, LS_BOX_H, lo);
}

void dk_studio_draw(tui_surface *sf, tui_rect c, tui_rect clip)
{
    art(sf, clip, c.x + 2, c.y + 1);
    const uint8_t tx = dk_attr(R_FACE), strong = dk_attr(R_STRONG), num = dk_attr(R_ACCEL), url = dk_attr(R_INFO);
    const int x = c.x + 21;
    dk_text(sf, clip, x, c.y + 1, "L A K E S H A R K   S T U D I O", hue(TUI_BLUE, TUI_CYAN | TUI_BRIGHT));
    dk_text(sf, clip, x, c.y + 2, "Write apps for this board on your PC.", tx);
    dk_text(sf, clip, x, c.y + 4, "Press OPEN STUDIO. Studio opens in its", strong);
    dk_text(sf, clip, x, c.y + 5, "own window on your PC, next to this one.", strong);
    dk_text(sf, clip, x, c.y + 7, "In Studio: New app, then", tx);
    dk_text(sf, clip, x + 2, c.y + 8, "F7 build   F5 run on the PC", url);
    dk_text(sf, clip, x + 2, c.y + 9, "F8 install on this board", url);
    dk_text(sf, clip, x, c.y + 11, "No window? On the PC run", tx);
    dk_text(sf, clip, x + 2, c.y + 12, "python app-sdk/lsdk.py studio", url);
    dk_hline(sf, clip, c.x + 2, c.y + c.h - 4, c.w - 4, LS_BOX_H, dk_attr(R_SHADOW));
    /* Above the rule, clear of the buttons: between them it ran under OK. */
    const int64_t ago = esp_timer_get_time() - s_asked_us;
    if (s_asked_us && ago < 6000000)
        dk_text(sf, clip, x, c.y + c.h - 5, "Opening Studio on your PC...", dk_attr(R_GOOD));
    dk_button(sf, clip, open_rect(c), "OPEN STUDIO", true, false);
    dk_button(sf, clip, ok_rect(c), "OK", false, false);
}

static void ask(void)
{
    ls_rterm_viewer_line("studio: open");     /* the bridge opens Studio on hearing it */
    s_asked_us = esp_timer_get_time();
    ls_tui_invalidate();
}

bool dk_studio_tap(dk_win_t *w, tui_rect c, int col, int row)
{
    if (tui_rect_contains(open_rect(c), col, row)) { ask(); return true; }
    if (tui_rect_contains(ok_rect(c), col, row)) { ls_desk_close(w->id); return true; }
    return false;
}

bool dk_studio_key(dk_win_t *w, ls_tk_t k, char ch)
{
    if (k == LS_TK_CHAR && (ch | 0x20) == 'o') { ask(); return true; }
    if (k == LS_TK_ENTER || k == LS_TK_ESC) { ls_desk_close(w->id); return true; }
    return false;
}

int dk_open_studio(void) { return dk_open_shell(DK_SHELL_STUDIO, "LakeShark Studio", 66, 19); }
