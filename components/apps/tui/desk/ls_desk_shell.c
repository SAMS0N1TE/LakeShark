/* Shell windows: drawn by the desk itself, so always live. About,
   Display Properties (ls_desk_props.c) and the Shut Down dialog. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>

#include "../ls_braille.h"

/* The OK / Close button of a shell window, bottom right of its client. */
static tui_rect ok_rect(tui_rect c) { return tui_rect_make(c.x + c.w - 16, c.y + c.h - 3, 14, 3); }

static uint8_t on_face(uint8_t fg) { return (uint8_t)((dk_attr(R_FACE) & 0xF0) | (fg & 0x0F)); }
static uint8_t hue(uint8_t light, uint8_t dark) { return on_face(dk_light_face() ? light : dark); }

/* The About window: a fin cutting a sea of signal, the name, what it is. */
static void about(tui_surface *sf, tui_rect c, tui_rect clip)
{
    const uint8_t fin = hue(TUI_BLUE, TUI_CYAN | TUI_BRIGHT), back = hue(TUI_BLACK, TUI_WHITE | TUI_BRIGHT);
    const uint8_t sea = hue(TUI_BLUE, TUI_BLUE | TUI_BRIGHT), foam = hue(TUI_CYAN, TUI_CYAN);
    const uint8_t tx = dk_attr(R_FACE), st = dk_attr(R_STRONG);
    const int fx = c.x + 9, fy = c.y + 1;
    for (int r = 0; r < 6; r++) {
        ls_box_put(sf, clip, fx - r - 1, fy + r, LS_BOX_DIAG_R, fin);
        ls_box_put(sf, clip, fx + 1 + r / 3, fy + r, LS_BOX_DIAG_L, back);
    }
    /* the sea: braille swell, the signal on it */
    static const uint16_t SWELL[12] = {0x2880, 0x28C0, 0x28E0, 0x28F0, 0x28E0, 0x28C0,
                                       0x2880, 0x2840, 0x2844, 0x2846, 0x2844, 0x2840};
    for (int x = 0; x < 16; x++) {
        ls_braille_put(sf, clip, c.x + 1 + x, fy + 6, (int16_t)SWELL[x % 12], sea);
        tui_put_char(sf, clip, c.x + 1 + x, fy + 7, (x & 3) == 1 ? '~' : ' ', foam);
    }
    const int x = c.x + 20;
    static const char NAME[] = "L A K E S H A R K";
    for (int i = 0; NAME[i]; i++)
        if (NAME[i] != ' ') tui_put_char(sf, clip, x + i, c.y + 1, NAME[i], i < 7 ? fin : back);
    dk_text(sf, clip, x, c.y + 2, "D E S K T O P", hue(TUI_RED, TUI_YELLOW | TUI_BRIGHT));
    dk_text(sf, clip, x, c.y + 4, "Windows 95, in characters, on the board.", tx);
    char line[80];
    snprintf(line, sizeof(line), "Firmware %s", ls_desk_version());
    dk_text(sf, clip, x, c.y + 5, line, tx);
    snprintf(line, sizeof(line), "Scheme: %s", ls_desk_theme_name(ls_desk_theme()));
    dk_text(sf, clip, x, c.y + 6, line, tx);
    int open = 0, apps = 0;
    ls_desk_win_info_t wi;
    for (int z = 0; ls_desk_window_at(z, &wi); z++) { open++; apps += wi.app; }
    snprintf(line, sizeof(line), "%d window%s, %d app%s; one app runs live.",
             open, open == 1 ? "" : "s", apps, apps == 1 ? "" : "s");
    dk_text(sf, clip, x, c.y + 7, line, tx);
    dk_hline(sf, clip, c.x + 2, c.y + 9, c.w - 4, LS_BOX_H, dk_attr(R_SHADOW));
    /* Keys, as a short table. */
    static const char *const K[][2] = {
        {"F10", "Start"}, {"F6", "next window"}, {"F11", "full screen"},
        {"F9", "scheme"}, {"Esc", "back, then the window menu"},
    };
    int ky = c.y + 10, kx = c.x + 3;
    for (unsigned i = 0; i < sizeof(K) / sizeof(K[0]); i++) {
        const int col = kx + (int)(i % 3) * 20, row = ky + (int)(i / 3);
        dk_text(sf, clip, col, row, K[i][0], dk_attr(R_ACCEL));
        dk_text(sf, clip, col + 4, row, K[i][1], st);
    }
    dk_button(sf, clip, ok_rect(c), "OK", true, false);
}

/* ---------------------------------------------------------- Shut Down -- */

enum { SD_LEAVE, SD_CLOSE, SD_PARK, SD_LOCK, SD_N };
static const char *const SD_TEXT[SD_N] = {
    "Leave the desktop (the board's own screens)",
    "Close all windows",
    "Park the receiver",
    "Lock the screen",
};
static int s_sd_choice;

static tui_rect sd_row(tui_rect c, int i) { return tui_rect_make(c.x + 16, c.y + 3 + i, c.w - 18, 1); }
static tui_rect sd_yes(tui_rect c) { return tui_rect_make(c.x + c.w - 30, c.y + c.h - 3, 12, 3); }
static tui_rect sd_no(tui_rect c) { return tui_rect_make(c.x + c.w - 16, c.y + c.h - 3, 12, 3); }

/* A monitor with the power mark on its screen, the dialog's picture. */
static void sd_art(tui_surface *sf, tui_rect clip, int x, int y)
{
    const uint8_t hi = dk_attr(R_HILITE), lo = dk_attr(R_SHADOW);
    ls_box_put(sf, clip, x, y, LS_BOX_ARC_DR, hi);
    dk_hline(sf, clip, x + 1, y, 9, LS_BOX_H, hi);
    ls_box_put(sf, clip, x + 10, y, LS_BOX_ARC_DL, lo);
    for (int r = 1; r <= 4; r++) {
        ls_box_put(sf, clip, x, y + r, LS_BOX_V, hi);
        ls_box_put(sf, clip, x + 10, y + r, LS_BOX_V, lo);
    }
    ls_box_put(sf, clip, x, y + 5, LS_BOX_ARC_UR, hi);
    dk_hline(sf, clip, x + 1, y + 5, 9, LS_BOX_H, lo);
    ls_box_put(sf, clip, x + 10, y + 5, LS_BOX_ARC_UL, lo);
    const uint8_t screen = TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLUE);
    dk_fill(sf, tui_rect_intersect(clip, tui_rect_make(x + 1, y + 1, 9, 4)), TUI_ATTR(TUI_WHITE, TUI_BLUE));
    /* the power mark: a broken ring and its stroke */
    ls_box_put(sf, clip, x + 5, y + 1, LS_BOX_V, screen);
    ls_box_put(sf, clip, x + 5, y + 2, LS_BOX_V, screen);
    ls_box_put(sf, clip, x + 3, y + 2, LS_BOX_ARC_DR, screen);
    ls_box_put(sf, clip, x + 7, y + 2, LS_BOX_ARC_DL, screen);
    ls_box_put(sf, clip, x + 3, y + 3, LS_BOX_ARC_UR, screen);
    dk_hline(sf, clip, x + 4, y + 3, 3, LS_BOX_H, screen);
    ls_box_put(sf, clip, x + 7, y + 3, LS_BOX_ARC_UL, screen);
    dk_hline(sf, clip, x + 3, y + 6, 5, LS_BOX_H, lo);
}

static void shutdown(tui_surface *sf, tui_rect c, tui_rect clip)
{
    sd_art(sf, clip, c.x + 2, c.y + 1);
    dk_text(sf, clip, c.x + 16, c.y + 1, "Are you sure you want to:", dk_attr(R_STRONG));
    for (int i = 0; i < SD_N; i++) {
        const tui_rect r = sd_row(c, i);
        dk_radio(sf, clip, r.x, r.y, s_sd_choice == i, SD_TEXT[i]);
    }
    dk_button(sf, clip, sd_yes(c), "Yes", true, false);
    dk_button(sf, clip, sd_no(c), "No", false, false);
}

int dk_open_shutdown(void)
{
    s_sd_choice = SD_LEAVE;
    dk_anim_dim();
    return dk_open_shell(DK_SHELL_SHUTDOWN, "Shut Down LakeShark Desktop", 70, 12);
}

static void sd_yes_do(dk_win_t *w)
{
    const int choice = s_sd_choice;
    ls_desk_close(w->id);
    switch (choice) {
    case SD_LEAVE: dk_do(A_DESK_OFF, 0); break;
    case SD_CLOSE: dk_do(A_CLOSE_ALL, 0); break;
    case SD_PARK: dk_do(A_PARK, 0); break;
    case SD_LOCK: dk_do(A_LOCK, 0); break;
    default: break;
    }
}

/* ------------------------------------------------------------- common -- */

void dk_shell_draw(tui_surface *sf, dk_win_t *w, tui_rect client, tui_rect clip, bool front)
{
    (void)front;
    if (w->shell == DK_SHELL_ABOUT) about(sf, client, clip);
    else if (w->shell == DK_SHELL_DISPLAY) dk_props_draw(sf, client, clip);
    else if (w->shell == DK_SHELL_SHUTDOWN) shutdown(sf, client, clip);
    else if (w->shell == DK_SHELL_STUDIO) dk_studio_draw(sf, client, clip);
}

bool dk_shell_tap(dk_win_t *w, tui_rect c, int col, int row)
{
    if (w->shell == DK_SHELL_DISPLAY) return dk_props_tap(c, col, row);
    if (w->shell == DK_SHELL_STUDIO) return dk_studio_tap(w, c, col, row);
    if (w->shell == DK_SHELL_SHUTDOWN) {
        for (int i = 0; i < SD_N; i++)
            if (tui_rect_contains(sd_row(c, i), col, row)) { s_sd_choice = i; return true; }
        if (tui_rect_contains(sd_yes(c), col, row)) { sd_yes_do(w); return true; }
        if (tui_rect_contains(sd_no(c), col, row)) { ls_desk_close(w->id); return true; }
        return false;
    }
    if (tui_rect_contains(ok_rect(c), col, row)) { ls_desk_close(w->id); return true; }
    return false;
}

bool dk_shell_key(dk_win_t *w, ls_tk_t k, char ch)
{
    if (w->shell == DK_SHELL_DISPLAY) return dk_props_key(k, ch);
    if (w->shell == DK_SHELL_STUDIO) return dk_studio_key(w, k, ch);
    if (w->shell == DK_SHELL_SHUTDOWN) {
        if (k == LS_TK_UP) { s_sd_choice = (s_sd_choice + SD_N - 1) % SD_N; return true; }
        if (k == LS_TK_DOWN) { s_sd_choice = (s_sd_choice + 1) % SD_N; return true; }
        if (k == LS_TK_ENTER) { sd_yes_do(w); return true; }
        if (k == LS_TK_ESC) { ls_desk_close(w->id); return true; }
        return true;                                  /* modal: nothing else */
    }
    if (k == LS_TK_ENTER || k == LS_TK_ESC) { ls_desk_close(w->id); return true; }
    return false;
}
