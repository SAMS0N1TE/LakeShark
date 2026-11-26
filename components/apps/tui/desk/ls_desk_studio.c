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

static uint8_t hue(uint8_t light, uint8_t dark) { return dk_fit(dk_light_face() ? light : dark, dk_face_bg(), 45); }

/* One row, bottom right, as every dialog's: Open Studio (the default), Close. */
static bool s_focus_close;            /* Tab moved the ring to Close */
static tui_rect open_rect(tui_rect c) { return tui_rect_make(c.x + c.w - 32, c.y + c.h - 2, 17, 1); }
static tui_rect ok_rect(tui_rect c) { return tui_rect_make(c.x + c.w - 14, c.y + c.h - 2, 12, 1); }

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
    /* The little monitor's screen is black glass in every scheme, its code in an editor's bright colours. */
    dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(x + 1, y + 1, 14, 5)), TUI_ATTR(TUI_WHITE, TUI_BLACK));
    const uint8_t kw = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK), fn = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t st = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK), cm = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    dk_text(sf, clip, x + 2, y + 1, "#include", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
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
    const uint8_t tx = dk_attr(R_FACE), strong = dk_attr(R_STRONG), url = dk_attr(R_INFO);
    const int x = c.x + 21;
    dk_text(sf, clip, x, c.y + 1, "L A K E S H A R K   S T U D I O", hue(TUI_BLUE, TUI_CYAN | TUI_BRIGHT));
    dk_text(sf, clip, x, c.y + 2, "Write apps for this board on your PC.", tx);
    dk_text(sf, clip, x, c.y + 4, "Press Open Studio. Studio opens in its", strong);
    dk_text(sf, clip, x, c.y + 5, "own window on your PC, next to this one.", strong);
    dk_text(sf, clip, x, c.y + 7, "In Studio: New app, then", tx);
    dk_text(sf, clip, x + 2, c.y + 8, "F7 build   F5 run on the PC", url);
    dk_text(sf, clip, x + 2, c.y + 9, "F8 install on this board", url);
    dk_text(sf, clip, x, c.y + 11, "No window? On the PC run", tx);
    {   /* the command in a read-only edit, the way a dialog shows a line to copy */
        static const char CMD[] = "python app-sdk/lsdk.py studio";
        const tui_rect in = dk_well_field(sf, clip, x + 1, c.y + 12, (int)sizeof(CMD) + 2);
        dk_text(sf, tui_rect_intersect(clip, in), x + 3, c.y + 12, CMD, dk_attr(R_WELL));
        (void)url;
    }
    dk_hline(sf, clip, c.x + 2, c.y + c.h - 3, c.w - 4, LS_BOX_H, dk_attr(R_SHADOW));
    /* Left of the buttons, on their row: the note that the PC was asked. */
    const int64_t ago = esp_timer_get_time() - s_asked_us;
    if (s_asked_us && ago < 6000000)
        dk_text(sf, clip, c.x + 2, c.y + c.h - 2, "Opening Studio on your PC...", dk_attr(R_GOOD));
    dk_pushbtn(sf, clip, open_rect(c), "&Open Studio", (s_focus_close ? 0 : DK_BS_DEFAULT | DK_BS_FOCUS));
    dk_pushbtn(sf, clip, ok_rect(c), "&Close", (s_focus_close ? DK_BS_DEFAULT | DK_BS_FOCUS : 0));
}

static void ask(void)
{
    ls_rterm_viewer_line("studio: open");     /* the bridge opens Studio on hearing it */
    s_asked_us = esp_timer_get_time();
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
    if (k == LS_TK_TAB || k == LS_TK_LEFT || k == LS_TK_RIGHT) { s_focus_close = !s_focus_close; return true; }
    if (k == LS_TK_ENTER || (k == LS_TK_CHAR && ch == ' ')) {       /* the button with the ring */
        if (s_focus_close) ls_desk_close(w->id);
        else ask();
        return true;
    }
    if (k == LS_TK_ESC || (k == LS_TK_CHAR && (ch | 0x20) == 'c')) { ls_desk_close(w->id); return true; }
    return false;
}

int dk_open_studio(void)
{
    s_focus_close = false;
    return dk_open_shell(DK_SHELL_STUDIO, "LakeShark Studio", 66, 19);
}
