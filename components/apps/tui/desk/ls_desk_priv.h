/* The desk's insides, shared by its files. Nothing outside desk/ includes it. */
#ifndef LS_DESK_PRIV_H
#define LS_DESK_PRIV_H

#include <stdbool.h>
#include <stdint.h>
#include "ls_desk.h"
#include "../ls_box.h"
#include "../ls_app.h"

/* ------------------------------------------------------------- roles -- */

typedef enum {
    R_DESK,        /* desktop ground */
    R_DESK_TEXT,   /* icon labels and the wordmark on the ground */
    R_FACE,        /* window and menu faces, labels (TEXT) */
    R_STRONG,      /* values, focused labels, the clock */
    R_HILITE,      /* top and left bevel lines */
    R_SHADOW,      /* bottom and right bevel lines, separators: lines only */
    R_TITLE_ON,
    R_TITLE_OFF,
    R_SELECT,
    R_SELECT_DIM,
    R_KEY,         /* small push keys */
    R_KEY_DOWN,    /* the front window's taskbar button, pressed */
    R_ACCEL,       /* the accelerator letter (no underline bit exists) */
    R_CLOSE,       /* the close key's cross */
    R_TIP,
    R_GOOD, R_WARN, R_BAD, R_INFO, R_MESH,
    R_DISABLED,
    R_STRIPE,      /* the Start menu's stripe */
    R_TASKKEY,     /* a raised taskbar key's face */
    R_TASKHOVER,   /* a taskbar key under the pointer */
    R_TRAY,        /* the tray's sunken panel */
    R_MENU,        /* menu faces */
    R_COUNT
} dk_role_t;

uint8_t dk_attr(int role);

/* Display Properties: a scheme, the owner's colours over it per item, the
   background, and the effects. */
enum { DK_IT_DESKTOP, DK_IT_TITLE_ON, DK_IT_TITLE_OFF, DK_IT_MENU, DK_IT_SELECT, DK_IT_WINDOW,
       DK_IT_BUTTON, DK_IT_TIP, DK_IT_COUNT };
enum { DK_FIT_FILL, DK_FIT_TILE, DK_FIT_CENTRE, DK_FIT_STRETCH, DK_FIT_COUNT };
typedef struct {
    uint8_t scheme, mask;                     /* mask: which items carry their own colours */
    uint8_t fg[DK_IT_COUNT], bg[DK_IT_COUNT];
    uint32_t wall;                            /* a picture or the mark / the sky (see dk_wall) */
    uint8_t pattern, fit;                     /* a pattern under it; how a picture fits */
    bool live_drag, anim, labels;             /* contents while dragging; animations; label backings */
} dk_prefs_t;
int  dk_scheme_count(void);
const char *dk_scheme_name(int i);
const char *dk_scheme_palette(int i);   /* the TUI theme's name */
const char *dk_item_name(int it);
const dk_prefs_t *dk_prefs(void);
void dk_prefs_set(const dk_prefs_t *p);   /* takes effect at once; ls_desk_apply_prefs saves */
void dk_item_colors(const dk_prefs_t *p, int item, uint8_t *fg, uint8_t *bg);
uint8_t dk_attr_with(const dk_prefs_t *p, int role);   /* for previews */
bool dk_light_face(void);
bool dk_light_desk(void);
/* A hue (TUI colour) as foreground on the face. */
uint8_t dk_hue(uint8_t hue);
uint8_t dk_face_bg(void);

/* ----------------------------------------------------------- widgets -- */

/* UTF-8 with box and shape codepoints; returns cells written. */
int  dk_text(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t attr);
/* The first letter in the accelerator colour. */
int  dk_text_accel(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t attr, uint8_t accel);
int  dk_text_len(const char *s);
void dk_fill(tui_surface *sf, tui_rect r, uint8_t attr);
void dk_hline(tui_surface *sf, tui_rect clip, int x, int y, int len, int cp, uint8_t attr);
void dk_vline(tui_surface *sf, tui_rect clip, int x, int y, int len, int cp, uint8_t attr);
/* Frames sit on the outer edges of r's border cells: fill all of r with the
   face, then frame it; the border cells keep that ground. */
void dk_frame(tui_surface *sf, tui_rect clip, tui_rect r, bool raised);
tui_rect dk_inner(tui_rect r);
void dk_edge_put(tui_surface *sf, tui_rect clip, int x, int y, int cp, int role);
void dk_frame_double(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t attr);
void dk_frame_dashed(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t attr);
void dk_frame_march(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t attr, unsigned phase);

/* ---------------------------------------------------------- animation -- */

/* Event-driven only: each starts on an action or a change of state, runs
   a fraction of a second, and stops. Off in Effects: none start. */
void dk_anim_zoom(tui_rect from, tui_rect to);
void dk_anim_icon(int icon);
void dk_anim_key(int win, int key);
void dk_anim_start(void);
void dk_anim_splash(void);
void dk_anim_dim(void);
bool dk_anim_active(void);
int  dk_anim_icon_progress(int icon);        /* 0..1000; 1000 when not running */
int  dk_anim_key_progress(int win, int key);
int  dk_anim_start_progress(void);
int  dk_anim_dim_progress(void);
bool dk_anim_splash_on(void);
void dk_anim_splash_end(void);
void dk_anim_draw(tui_surface *sf);            /* the welcome, over everything */
void dk_anim_draw_trails(tui_surface *sf);     /* the zoom trails, under the menus */

/* ------------------------------------------------- Display Properties -- */

void dk_props_open(void);
void dk_props_tab(int tab);                     /* a fresh copy of the choices to edit */
void dk_props_draw(tui_surface *sf, tui_rect client, tui_rect clip);
bool dk_props_tap(tui_rect client, int col, int row);
bool dk_props_key(ls_tk_t k, char ch);
void dk_wall_preview(tui_surface *sf, tui_rect r, const dk_prefs_t *p);
/* `[_]` style key on KEY; the glyph is a codepoint or ASCII. */
void dk_key(tui_surface *sf, tui_rect clip, int x, int y, int cp, uint8_t glyph_attr);
/* A three-row push button; the double frame when it is the default. */
void dk_button(tui_surface *sf, tui_rect clip, tui_rect r, const char *label, bool is_default, bool pressed);
void dk_check(tui_surface *sf, tui_rect clip, int x, int y, bool on, const char *label);
void dk_radio(tui_surface *sf, tui_rect clip, int x, int y, bool on, const char *label);
/* `[=========-------]` with the value beside it. */
void dk_meter(tui_surface *sf, tui_rect clip, int x, int y, int w, int permille, uint8_t hue);
/* Braille sparkline of n samples scaled to max. */
void dk_spark(tui_surface *sf, tui_rect clip, int x, int y, const int *v, int n, int max, uint8_t attr);

/* ----------------------------------------------------------- windows -- */

enum { DK_APP = 1, DK_SHELL = 2 };
enum { DK_SHELL_ABOUT = 1, DK_SHELL_DISPLAY = 2, DK_SHELL_SHUTDOWN = 3, DK_SHELL_STUDIO = 4 };

typedef struct {
    bool used;
    uint8_t kind;           /* DK_APP or DK_SHELL */
    uint8_t shell;          /* DK_SHELL_* */
    uint8_t state;          /* ls_desk_win_state_t */
    int id;
    int x, y, w, h;         /* outer frame while normal */
    int screen;             /* router screen index, DK_APP */
    const ls_app_t *app;
    tui_cell *snap;         /* last frame of the client, PSRAM */
    int sw, sh;
    char title[40];
    char label[28];         /* the taskbar line, as last seen live */
} dk_win_t;

/* Geometry the frame decides: outer and client in grid cells, as drawn. */
void dk_win_rects(const dk_win_t *w, tui_rect *outer, tui_rect *client);
tui_rect dk_work(void);               /* rows above the taskbar */

/* -------------------------------------------------------------- compat -- */

/* Draw the front app window's client through the router into the live
   surface and copy it to the grid at `client`. */
bool dk_compat_select(dk_win_t *w);   /* true: handed to another app's window */
void dk_compat_finger(int col, int row, int px, int py, bool down);
void dk_compat_tap_later(dk_win_t *w, int col, int row);
void dk_compat_draw_live(tui_surface *sf, dk_win_t *w, tui_rect client, tui_rect clip);
/* A background window: its snapshot, or "paused" when it has none. */
void dk_compat_draw_snapshot(tui_surface *sf, dk_win_t *w, tui_rect client, tui_rect clip);
/* The live surface into w's snapshot (when w is the one drawn live). */
void dk_compat_snapshot(dk_win_t *w);
void dk_compat_forget(dk_win_t *w);       /* free its snapshot */
void dk_compat_release(void);             /* free the live surface */
int  dk_compat_blocks(void);
/* The window whose frame is in the live surface, or -1. */
int  dk_compat_live_id(void);
/* A tap and a key for the live app, in client cells. */
bool dk_compat_tap(int col, int row);
bool dk_compat_key(ls_tk_t k, char ch);

/* --------------------------------------------------------------- menus -- */

enum { MI_ITEM, MI_SUB, MI_SEP, MI_CHECK, MI_RADIO };
enum {
    A_NONE = 0, A_OPEN_APP, A_SUBMENU, A_THEME, A_ABOUT, A_DISPLAY, A_DESK_OFF,
    A_ARRANGE, A_WIN_RESTORE, A_WIN_MIN, A_WIN_MAX, A_WIN_CLOSE, A_WIN_FULL,
    A_BACK, A_OPTIONS, A_LOCK, A_PARK, A_CLOSE_ALL, A_KEYS,
    A_VOL_SET, A_VOL_STEP, A_MUTE, A_RX_RESUME, A_WALLPAPER, A_SHUTDOWN, A_STUDIO, A_WIN_FOCUS,
};
enum {
    M_START = 1, M_PROGRAMS, M_GROUP, M_DOCS, M_SETTINGS, M_THEME, M_FIND, M_SHUTDOWN,
    M_DESKTOP, M_WINDOW, M_APPCTX,
    /* the tray's popups; their rows are rebuilt every frame from real state */
    M_VOLUME, M_SOUND, M_CLOCK, M_RX, M_MESH, M_BT, M_GPS, M_WALLPAPER,
    M_DEV,                  /* Programs > Development */
};

#define DK_MENU_ITEMS 20
enum { MI_SLIDER = MI_RADIO + 1, MI_INFO };    /* a volume slider row; a line of state */
typedef struct {
    const char *label;
    char text[40];          /* the label's storage when it is built from state */
    char right[16];
    uint8_t kind;
    bool on, off;           /* checked / disabled */
    uint8_t lamp;           /* 0, or a role for a leading lamp dot */
    int act, arg;
} dk_item_t;

typedef struct {
    int id, arg;
    int x, y, w, h;
    int sel;
    int n;
    bool stripe;
    bool up;                /* opened upward (from the taskbar): unfolds from its foot */
    int64_t t0;             /* when it opened, for the unfold */
    dk_item_t it[DK_MENU_ITEMS];
} dk_menu_t;

void dk_menu_open(int id, int arg, int x, int y);
/* The deepest menu's (id, arg) when one is open, for the taskbar's look. */
int  dk_menu_top_id(void);
void dk_menu_close_all(void);
int  dk_menu_depth(void);
const dk_menu_t *dk_menu_at(int level);
void dk_menu_draw(tui_surface *sf);
/* Input while a menu is up: true when it was the menu's. */
bool dk_menu_key(ls_tk_t k, char ch);
bool dk_menu_pointer(const ls_input_ev_t *ev);
/* What an item does: carried out by ls_desk.c. */
void dk_do(int act, int arg);

/* ----------------------------------------------------------- the desk -- */

dk_win_t *dk_win(int id);
dk_win_t *dk_front_win(void);
int dk_open_shell(int shell, const char *title, int w, int h);
dk_win_t *dk_win_shell(int shell);           /* the open shell window of a kind, or NULL */
/* Apps by HOME group for the Start menu: 0 radio, 1 field, 2 system, 3 user. */
int dk_group_apps(int group, const ls_app_t **out, int cap);
const char *dk_group_name(int group);
/* An app's live line ("162.550", "TG 1201", "14 ac") from its own state,
   in buf, or NULL when it has none (ls_desk_live.c). */
const char *dk_live_line(const ls_app_t *a, char *buf, size_t n);
/* The taskbar line of a running app's window, or NULL. */
const char *dk_app_label(const ls_app_t *a);

void dk_shell_draw(tui_surface *sf, dk_win_t *w, tui_rect client, tui_rect clip, bool front);
bool dk_shell_tap(dk_win_t *w, tui_rect client, int col, int row);
bool dk_shell_key(dk_win_t *w, ls_tk_t k, char ch);
/* LakeShark Studio's window (ls_desk_studio.c): how to open Studio on the PC. */
int  dk_open_studio(void);
void dk_studio_draw(tui_surface *sf, tui_rect client, tui_rect clip);
bool dk_studio_tap(dk_win_t *w, tui_rect client, int col, int row);
bool dk_studio_key(dk_win_t *w, ls_tk_t k, char ch);

/* The board around the desk (ls_desk_live.c; stubbed in the host test):
   sound, the Bluetooth keyboards, the clock's source, what persists. */
int  dk_sys_volume(void);                 /* 0..100, -1 with no audio */
void dk_sys_set_volume(int pct);
bool dk_sys_muted(void);
void dk_sys_set_mute(bool on);            /* always explicit: on or off */
int  dk_sys_bt(int *connected);           /* bonded keyboards; -1 with no Bluetooth host */
int  dk_sys_time_source(void);            /* 0 none, 1 RTC/local, 2 network */
int  dk_sys_saved_theme(void);            /* -1 when nothing is saved */
void dk_sys_save_theme(int theme);
uint32_t dk_sys_saved_wallpaper(void);
uint32_t dk_sys_saved_word(int slot);
void dk_sys_save_word(int slot, uint32_t v);
void ls_desk_apply_prefs(const dk_prefs_t *p);
const char *ls_desk_version(void);       /* the firmware's, for About */
int  dk_open_shutdown(void);
void dk_sys_save_wallpaper(uint32_t id);
/* Image wallpapers on the card: names (no directory), sorted; how many. */
int  dk_sys_wallpaper_files(char names[][40], int cap);
/* Decode one (BMP; PNG where the board has a decoder) into PSRAM RGB565,
   at most max_w x max_h, keeping its aspect. Free with dk_psram_free. */
/* fit: DK_FIT_*; full_w: the area's width in screen pixels (for tile and
   centre, which keep the picture's own size); ground: the colour around a
   centred picture (a TUI colour index). */
bool dk_sys_load_wallpaper(const char *name, int max_w, int max_h, int fit, int full_w, uint8_t ground,
                           uint16_t **px, int *w, int *h);
int  dk_wall_pattern_count(void);
const char *dk_wall_pattern_name(int i);

/* -------------------------------------------------------- wallpapers -- */

/* Built-in: 0 plain, 1.. the patterns, the logo, the sky; image files are
   DK_WP_FILE | a hash of the name, so a saved choice survives the list
   changing. */
#define DK_WP_FILE 0x80000000u
int  dk_wall_builtin_count(void);
const char *dk_wall_builtin_name(int i);
uint32_t dk_wall_current(void);
void dk_wall_set(uint32_t id);
/* The files on the card, as the menu lists them; their ids. */
int  dk_wall_files(const char **names, uint32_t *ids, int cap);
/* The desktop's ground for `work` (and the bar's top row), before icons. */
void dk_wall_draw(tui_surface *sf, tui_rect area);
bool dk_wall_still(void);
/* After the windows: an app took the one picture, so the wallpaper's
   picture cells fall back to plain ground. */
void dk_wall_after_windows(tui_surface *sf, tui_rect area, tui_rect keep);
void dk_wall_glass(tui_surface *sf, tui_rect area);

void *dk_psram(size_t bytes);
void dk_psram_free(void *p);

#endif
