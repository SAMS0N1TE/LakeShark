/* LakeShark Desktop: a Windows 95 style shell drawn in the TUI grid.

   The desk owns the grid while it is on. It draws a desktop, windows, a
   taskbar and menus, and runs the existing router inside the front window
   by drawing it into an off-screen surface the size of the window's client
   area. One app is live at a time, in the front window; the others keep
   their last frame and say they are paused.

   Input reaches the desk first (ls_desk_input, the contract in
   ls_input_ev.h). Until the desktop input side lands, ls_desk_attach() also
   takes today's input: router keys, router taps and the remote finger. */
#ifndef LS_DESK_H
#define LS_DESK_H

#include <stdbool.h>
#include <stdint.h>
#include "../ls_tui.h"
#include "../ls_input_ev.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { LS_DESK_THEME_BAY95 = 0, LS_DESK_THEME_CLASSIC95 = 1, LS_DESK_THEME_HIGH_CONTRAST,
       LS_DESK_THEME_TEAL, LS_DESK_THEME_AMBER, LS_DESK_THEME_NIGHT, LS_DESK_THEME_PHOSPHOR,
       LS_DESK_THEME_ICE, LS_DESK_THEME_SYNTH, LS_DESK_THEME_ARCADE, LS_DESK_THEME_VFD, LS_DESK_THEME_COUNT };

/* On or off. Turning it on attaches the input adapter and puts the router in
   full mode; off gives the grid back to the router as it was. */
void ls_desk_set_enabled(bool on);
bool ls_desk_enabled(void);

void ls_desk_set_theme(int theme);
int  ls_desk_theme(void);
const char *ls_desk_theme_name(int theme);

/* One frame into the grid. The caller presents. UI task only. */
void ls_desk_draw(tui_surface *sf);

/* Open an app (by id, "fm", "p25", "map", "diag"...) in a new compat window,
   or raise its window when it has one. Returns the window id or -1. */
int  ls_desk_open_app(const char *id);
/* Open a tray popup as its click would: "volume", "clock", "radio",
   "mesh", "keyboard", "gps"; right for the right button's menu. */
bool ls_desk_tray_popup(const char *name, bool right);
/* The About window (a shell window drawn by the desk). */
int  ls_desk_open_about(void);
int  ls_desk_open_display(int tab);       /* Display Properties, on a tab (0 Background .. 3 Settings) */
int  ls_desk_open_shutdown(void);
/* The shell's other windows by name: "control" (arg: an applet, -1 the
   panel), "run", "tasks", "bin", "folder". False for a name it has not. */
bool ls_desk_shell_open(const char *name, int arg);
void ls_desk_welcome(void);               /* the splash a new viewer sees */

/* Window manager, for tests and the console. Ids are stable while a window
   lives. */
#define LS_DESK_MAX_WIN 8
typedef enum { LS_DESK_WIN_NORMAL, LS_DESK_WIN_MIN, LS_DESK_WIN_MAX } ls_desk_win_state_t;
typedef struct {
    int id;
    bool app;                 /* a compat app window; false: a shell window */
    int state;                /* ls_desk_win_state_t */
    int x, y, w, h;           /* outer frame, cells */
    int cx, cy, cw, ch;       /* client area, cells */
    bool front;               /* focused, and live */
    bool has_snapshot;
    const char *title;
} ls_desk_win_info_t;
int  ls_desk_window_count(void);
/* An app (by ls_app_t id) has a window on the desk, in front or behind; any
   radio app (a screen that names a receiver) has one. Its services keep
   running for MAP and the other windows that show what they hear. */
bool ls_desk_app_open(const char *id);
bool ls_desk_radio_app_open(void);
/* Back to front: index 0 is the bottom window. */
bool ls_desk_window_at(int z, ls_desk_win_info_t *out);
int  ls_desk_front(void);
void ls_desk_raise(int id);
void ls_desk_move(int id, int x, int y);
void ls_desk_resize(int id, int w, int h);
void ls_desk_minimize(int id);
void ls_desk_maximize(int id);   /* toggles maximize and restore */
void ls_desk_restore(int id);
void ls_desk_close(int id);
void ls_desk_close_all(void);
/* App windows have size presets instead of free sizes: 0 Small (82x28
   outer), 1 Medium (102x34), 2 Large (122x42), 3 Maximized. Each is
   clamped to the work area; a preset that does not fit is not taken (Small
   always is). ls_desk_resize snaps to the smallest preset at least that big.
   ls_desk_size answers the window's preset, -1 for a shell window. */
enum { LS_DESK_SIZE_SMALL, LS_DESK_SIZE_MEDIUM, LS_DESK_SIZE_LARGE, LS_DESK_SIZE_MAX };
int  ls_desk_size(int id);
void ls_desk_set_size(int id, int preset);

/* The desktop's resolution: the grid no longer follows the PC window. The
   owner picks one of ls_desk_res_count() presets (ls_desk_res_at: 100x37,
   120x45, 148x50) and the page scales the cells to fit, letterboxed. Saved
   on the board. ls_desk_res_get answers false while none is held (the grid
   follows the window, as before). ls_desk_res_set(0, 0) lets go; a size
   applies at once where the screen is a terminal (a Nano) and answers false
   for one under 100x30 or where the screen keeps its own grid. For Control
   Panel > Display and the console's 'desk res WxH|auto'. UI task. */
int  ls_desk_res_count(void);
bool ls_desk_res_at(int i, int *cols, int *rows);
bool ls_desk_res_get(int *cols, int *rows);
bool ls_desk_res_set(int cols, int rows);
/* PSRAM blocks the desk holds now (live surface plus snapshots). */
int  ls_desk_alloc_blocks(void);
/* Full screen for the front window (Alt+Enter / F11). */
void ls_desk_fullscreen(bool on);
bool ls_desk_is_fullscreen(void);
/* The front app alone at full frame rate; see ls_desk.c. */
void ls_desk_focus(bool on);
bool ls_desk_focused(void);

/* Menus, for tests: how many are open, and the label of the highlighted
   row of the deepest one. */
int  ls_desk_menu_depth(void);
const char *ls_desk_menu_label(int level, int row);

/* The input adapter for today's paths (router keys and taps, the remote
   finger). Turned on with the desk. */
void ls_desk_attach(bool on);

/* The console: 'desk', 'desk on|off', 'desk theme bay|classic',
   'desk open <app>', 'desk list'. Prints, returns 0 on success. */
int  ls_desk_console(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* LS_DESK_H */
