/* LakeShark Desktop: window manager, desktop, taskbar and tray, input.
   See ls_desk.h and lap3d_work/DESKTOP_UI_DESIGN.md sections 3 and 4.

   Motion rules: nothing here animates by timer. A window moves because the
   pointer moved, a lamp changes because the state did, the latency line
   changes because a sample came in. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "../ls_tui_screen.h"
#include "../ls_tui_ui.h"
#include "../ls_tui_touch.h"
#include "../ls_icons.h"
#include "../ls_rterm.h"
#include "ls_gps.h"
#include "../ls_braille.h"
#include "../ls_theme.h"

/* Things the tray reads that a small build may not link: weak, checked. */
extern void ls_rterm_status(ls_rterm_status_t *out) __attribute__((weak));
extern void ls_gps_get(ls_gps_state_t *out) __attribute__((weak));
extern int  ls_notify_unread(void) __attribute__((weak));

/* The desktop input side owns the latency meter; until it is linked, there
   is no figure and the tray shows none. */
__attribute__((weak)) bool ls_input_latency(int *p50_ms, int *p95_ms)
{
    (void)p50_ms; (void)p95_ms;
    return false;
}

#if defined(ESP_PLATFORM) && defined(CONFIG_LS_PANEL_RUNTIME) && CONFIG_LS_PANEL_RUNTIME
#define DK_BOARD "ESP32-P4-NANO"
#elif defined(ESP_PLATFORM)
#define DK_BOARD "ESP32-P4"
#else
#define DK_BOARD "lssim"
#endif

enum { DRAG_NONE, DRAG_MOVE, DRAG_SIZE, DRAG_KEY, DRAG_CLIENT, DRAG_ICON };
enum { KEY_MENU = 1, KEY_MIN, KEY_MAX, KEY_CLOSE };
#define DESK_MIN_COLS 100
#define DESK_MIN_ROWS 30
#define DBL_US 400000

EXT_RAM_BSS_ATTR static dk_win_t s_w[LS_DESK_MAX_WIN];
EXT_RAM_BSS_ATTR static ls_gps_state_t s_gps;
static int s_z[LS_DESK_MAX_WIN], s_nz;
static int s_next_id = 1;
static bool s_on, s_full, s_focus;
static int64_t s_focus_banner_us;
static int s_cols, s_rows;              /* the grid as last drawn */
static int s_last_front = -1;
static int s_icon_sel = -1;
static int s_ptr_col = 10, s_ptr_row = 10;
static int s_drag, s_drag_win, s_drag_dx, s_drag_dy, s_drag_key, s_down_col, s_down_row;
static bool s_drag_moved, s_down_was_front;
static unsigned s_outline_step;               /* moves so far: the outline's dashes march with them */
static tui_rect s_outline;
static int64_t s_last_down_us = -1;
static int s_last_down_col = -1, s_last_down_row = -1;
static bool s_attached, s_synth_down;
static int s_synth_col, s_synth_row;
static int s_synth_px, s_synth_py;
static int s_lat[16], s_lat_n, s_lat_last = -1;
static int64_t s_lat_us;
static bool s_pressed_start;
static const char *s_last_radio;          /* the receiver the tray can resume */

/* ------------------------------------------------------------- basics -- */

dk_win_t *dk_win(int id)
{
    for (int i = 0; i < LS_DESK_MAX_WIN; i++) if (s_w[i].used && s_w[i].id == id) return &s_w[i];
    return NULL;
}

dk_win_t *dk_front_win(void)
{
    for (int i = s_nz - 1; i >= 0; i--) {
        dk_win_t *w = dk_win(s_z[i]);
        if (w && w->state != LS_DESK_WIN_MIN) return w;
    }
    return NULL;
}

tui_rect dk_work(void)
{
    const int rows = s_rows > 2 ? s_rows - 2 : 0;
    return tui_rect_make(0, 0, s_cols, rows);
}

void dk_win_rects(const dk_win_t *w, tui_rect *outer, tui_rect *client)
{
    const tui_rect o = w->state == LS_DESK_WIN_MAX ? dk_work() : tui_rect_make(w->x, w->y, w->w, w->h);
    if (outer) *outer = o;
    if (client) {
        if (w->state == LS_DESK_WIN_MAX) *client = tui_rect_make(o.x, o.y + 1, o.w, o.h - 2);
        else *client = tui_rect_make(o.x + 1, o.y + 1, o.w - 2, o.h - 3);
    }
}

/* Display Properties' choices in the settings words: 0 the flags, 1-2 the
   item colours, 8.. the icons' places. */
enum { W_FLAGS = 0, W_COLOURS = 1, W_ICONS = 8 };
#define W_VALID 0x80000000u

static void save_prefs(const dk_prefs_t *p)
{
    dk_sys_save_theme(p->scheme);
    dk_sys_save_wallpaper(p->wall);
    dk_sys_save_word(W_FLAGS, W_VALID | (uint32_t)p->mask << 8 | (uint32_t)(p->pattern & 15) << 16 |
                     (uint32_t)(p->fit & 15) << 20 | (p->live_drag ? 1u << 24 : 0) |
                     (p->anim ? 1u << 25 : 0) | (p->labels ? 1u << 26 : 0));
    uint32_t a = 0, b = 0;
    for (int i = 0; i < 4; i++) a |= (uint32_t)((p->fg[i] & 15) | (p->bg[i] & 15) << 4) << (8 * i);
    for (int i = 4; i < DK_IT_COUNT; i++) b |= (uint32_t)((p->fg[i] & 15) | (p->bg[i] & 15) << 4) << (8 * (i - 4));
    dk_sys_save_word(W_COLOURS, a);
    dk_sys_save_word(W_COLOURS + 1, b);
}

static dk_prefs_t load_prefs(void)
{
    /* The defaults a new board starts with: Bay 95, the LakeShark mark,
       windows that move with their contents, animations on, labels on
       their own backing so any wallpaper reads. */
    dk_prefs_t p = {.scheme = 0, .wall = 5, .live_drag = true, .anim = true, .labels = true};
    const int t = dk_sys_saved_theme();
    if (t >= 0 && t < LS_DESK_THEME_COUNT) p.scheme = (uint8_t)t;
    const uint32_t f = dk_sys_saved_word(W_FLAGS);
    if (f & W_VALID) {
        p.mask = (uint8_t)(f >> 8);
        p.pattern = (uint8_t)(f >> 16 & 15);
        p.fit = (uint8_t)(f >> 20 & 15);
        p.live_drag = f >> 24 & 1;
        p.anim = f >> 25 & 1;
        p.labels = f >> 26 & 1;
        p.wall = dk_sys_saved_wallpaper();
        const uint32_t a = dk_sys_saved_word(W_COLOURS), b = dk_sys_saved_word(W_COLOURS + 1);
        for (int i = 0; i < 4; i++) { p.fg[i] = a >> (8 * i) & 15; p.bg[i] = a >> (8 * i + 4) & 15; }
        for (int i = 4; i < DK_IT_COUNT; i++) { p.fg[i] = b >> (8 * (i - 4)) & 15; p.bg[i] = b >> (8 * (i - 4) + 4) & 15; }
    }
    if (p.pattern >= 5) p.pattern = 0;
    if (p.fit >= DK_FIT_COUNT) p.fit = 0;
    return p;
}

/* ONE THEME ON THE DESK. A scheme is drawn in its own palette theme: the
   two used to be chosen apart, and a silver scheme in the Amber palette
   came out orange. A palette theme chosen anywhere else (Settings, F9,
   the console) brings the scheme made for it. */
static const ls_tui_theme_t *s_seen_theme;
/* Saved like Settings saves it; weak, as the host bench has no settings store. */
extern void settings_set_theme(int index) __attribute__((weak));

static void palette_apply(void)
{
    const char *name = dk_scheme_palette(dk_prefs()->scheme);
    const ls_tui_theme_t *t = name ? ls_tui_theme_by_name(name) : NULL;
    if (t && ls_tui_get_theme() != t) {
        ls_tui_set_theme(t);
        if (settings_set_theme) settings_set_theme(ls_tui_theme_index(t));
    }
    s_seen_theme = ls_tui_get_theme();
}

static void palette_follow(void)
{
    const ls_tui_theme_t *t = ls_tui_get_theme();
    if (t == s_seen_theme) return;
    s_seen_theme = t;
    const char *mine = dk_scheme_palette(dk_prefs()->scheme);
    if (!t || (mine && !strcmp(mine, t->name))) return;
    for (int i = 0; i < dk_scheme_count(); i++) {
        const char *pal = dk_scheme_palette(i);
        if (pal && !strcmp(pal, t->name)) { ls_desk_set_theme(i); return; }
    }
}

/* Display Properties' OK and Apply. */
void ls_desk_apply_prefs(const dk_prefs_t *p)
{
    dk_prefs_set(p);
    dk_wall_set(p->wall);
    save_prefs(dk_prefs());
    palette_apply();
    ls_tui_invalidate();
}

void ls_desk_set_theme(int t)
{
    if (t < 0 || t >= LS_DESK_THEME_COUNT) return;
    dk_prefs_t p = *dk_prefs();
    p.scheme = (uint8_t)t;
    ls_desk_apply_prefs(&p);
}
int  ls_desk_theme(void) { return dk_prefs()->scheme; }
const char *ls_desk_theme_name(int t)
{
    return dk_scheme_name(t);
}

static int min_w(const dk_win_t *w) { return w->kind == DK_APP ? 60 + 2 : 24; }
static int min_h(const dk_win_t *w) { return w->kind == DK_APP ? 20 + 3 : 6; }

/* Keep a window on the grid: its title row and 8 title cells at least,
   shrunk when bigger than the work area (never below its minimum). */
static void clamp(dk_win_t *w)
{
    const tui_rect work = dk_work();
    if (w->w > work.w) w->w = work.w;
    if (w->h > work.h) w->h = work.h;
    if (w->w < min_w(w)) w->w = min_w(w);
    if (w->h < min_h(w)) w->h = min_h(w);
    if (w->y < 0) w->y = 0;
    if (w->y > work.h - 1) w->y = work.h - 1;
    if (w->x > work.w - 8) w->x = work.w - 8;
    if (w->x + w->w < 8) w->x = 8 - w->w;
}

/* --------------------------------------------------------- z and focus -- */

static void z_remove(int id)
{
    int k = 0;
    for (int i = 0; i < s_nz; i++) if (s_z[i] != id) s_z[k++] = s_z[i];
    s_nz = k;
}

static tui_rect task_rect(int id);

void ls_desk_raise(int id)
{
    dk_win_t *w = dk_win(id);
    if (!w) return;
    if (w->state == LS_DESK_WIN_MIN) {
        w->state = LS_DESK_WIN_NORMAL;
        tui_rect o;
        dk_win_rects(w, &o, NULL);
        dk_anim_zoom(task_rect(id), o);           /* back up out of its button */
    }
    z_remove(id);
    s_z[s_nz++] = id;
    s_icon_sel = -1;
}

int ls_desk_front(void) { const dk_win_t *w = dk_front_win(); return w ? w->id : -1; }

static dk_win_t *alloc_win(void)
{
    for (int i = 0; i < LS_DESK_MAX_WIN; i++)
        if (!s_w[i].used) {
            memset(&s_w[i], 0, sizeof(s_w[i]));
            s_w[i].used = true;
            s_w[i].id = s_next_id++;
            return &s_w[i];
        }
    return NULL;
}

static void place_new(dk_win_t *w, int cw, int ch)
{
    const tui_rect work = dk_work();
    int n = 0;
    for (int i = 0; i < LS_DESK_MAX_WIN; i++) if (s_w[i].used && &s_w[i] != w) n++;
    w->w = cw + 2;
    w->h = ch + 3;
    if (w->w > work.w) w->w = work.w;
    if (w->h > work.h) w->h = work.h;
    /* Cascade from the top left, a step right and down for each window,
       starting over when the next one would not fit. */
    const int step_x = 4, step_y = 2;
    const int room_x = work.w - w->w, room_y = work.h - w->h;
    int k = n % 6;
    w->x = 14 + k * step_x;
    w->y = 1 + k * step_y;
    if (w->x > room_x) w->x = room_x > 0 ? room_x : 0;
    if (w->y > room_y) w->y = room_y > 0 ? room_y : 0;
    clamp(w);
}

/* LakeShark Studio's desktop icon: not an app of the board's, a window
   saying how to open Studio on the PC (ls_desk_studio.c). */
static const ls_app_t s_studio_icon = {.id = "studio", .name = "LakeShark Studio", .icon = LS_ICON_TERMINAL,
                                       .hue = TUI_YELLOW, .cat = LS_APP_EXTRA};

static int open_app(const ls_app_t *a)
{
    if (a == &s_studio_icon) return dk_open_studio();
    if (!a || !a->screen) return -1;
    for (int i = 0; i < LS_DESK_MAX_WIN; i++)
        if (s_w[i].used && s_w[i].kind == DK_APP && s_w[i].app == a) {
            ls_desk_raise(s_w[i].id);
            return s_w[i].id;
        }
    const int si = ls_tui_screen_index_of(a->screen);
    if (si < 0) return -1;
    dk_win_t *w = alloc_win();
    if (!w) return -1;
    w->kind = DK_APP;
    w->app = a;
    w->screen = si;
    snprintf(w->title, sizeof(w->title), "%s", a->name ? a->name : a->id);
    snprintf(w->label, sizeof(w->label), "%s", a->name ? a->name : a->id);
    place_new(w, 115, 27);
    ls_desk_raise(w->id);
    dk_anim_zoom(task_rect(w->id), tui_rect_make(w->x, w->y, w->w, w->h));
    return w->id;
}

int ls_desk_open_app(const char *id)
{
    if (id && !strcmp(id, "studio")) return dk_open_studio();     /* desk open studio */
    return id ? open_app(ls_app_by_id(id)) : -1;
}

int dk_open_shell(int shell, const char *title, int cw, int ch)
{
    for (int i = 0; i < LS_DESK_MAX_WIN; i++)
        if (s_w[i].used && s_w[i].kind == DK_SHELL && s_w[i].shell == shell) {
            ls_desk_raise(s_w[i].id);
            return s_w[i].id;
        }
    dk_win_t *w = alloc_win();
    if (!w) return -1;
    w->kind = DK_SHELL;
    w->shell = (uint8_t)shell;
    snprintf(w->title, sizeof(w->title), "%s", title);
    snprintf(w->label, sizeof(w->label), "%s", title);
    place_new(w, cw, ch);
    /* Shell windows open in the middle, as dialogs do. */
    const tui_rect work = dk_work();
    w->x = (work.w - w->w) / 2;
    w->y = (work.h - w->h) / 3;
    clamp(w);
    ls_desk_raise(w->id);
    if (shell != DK_SHELL_SHUTDOWN)                   /* that one dims the desk instead */
        dk_anim_zoom(task_rect(w->id), tui_rect_make(w->x, w->y, w->w, w->h));
    return w->id;
}

int ls_desk_open_display(int tab)
{
    dk_do(A_DISPLAY, 0);
    dk_props_tab(tab);
    return ls_desk_front();
}

int ls_desk_open_shutdown(void) { return dk_open_shutdown(); }
void ls_desk_welcome(void) { dk_anim_splash(); }

dk_win_t *dk_win_shell(int shell)
{
    for (int i = 0; i < LS_DESK_MAX_WIN; i++)
        if (s_w[i].used && s_w[i].kind == DK_SHELL && s_w[i].shell == shell) return &s_w[i];
    return NULL;
}

int ls_desk_open_about(void) { return dk_open_shell(DK_SHELL_ABOUT, "About LakeShark Desktop", 66, 17); }

void ls_desk_minimize(int id)
{
    dk_win_t *w = dk_win(id);
    if (!w || w->state == LS_DESK_WIN_MIN) return;
    tui_rect o;
    dk_win_rects(w, &o, NULL);
    w->state = LS_DESK_WIN_MIN;
    dk_anim_zoom(o, task_rect(id));               /* down into its button */
}

void ls_desk_maximize(int id)
{
    dk_win_t *w = dk_win(id);
    if (!w) return;
    tui_rect a, b2;
    dk_win_rects(w, &a, NULL);
    w->state = w->state == LS_DESK_WIN_MAX ? LS_DESK_WIN_NORMAL : LS_DESK_WIN_MAX;
    dk_win_rects(w, &b2, NULL);
    dk_anim_zoom(a, b2);
    ls_desk_raise(id);
}

void ls_desk_restore(int id)
{
    dk_win_t *w = dk_win(id);
    if (!w) return;
    w->state = LS_DESK_WIN_NORMAL;
    ls_desk_raise(id);
}

void ls_desk_move(int id, int x, int y)
{
    dk_win_t *w = dk_win(id);
    if (!w) return;
    w->x = x;
    w->y = y;
    clamp(w);
}

void ls_desk_resize(int id, int ww, int hh)
{
    dk_win_t *w = dk_win(id);
    if (!w) return;
    w->w = ww;
    w->h = hh;
    clamp(w);
}

void ls_desk_close(int id)
{
    dk_win_t *w = dk_win(id);
    if (!w) return;
    if (w->state != LS_DESK_WIN_MIN) {
        tui_rect o;
        dk_win_rects(w, &o, NULL);
        dk_anim_zoom(o, tui_rect_make(o.x + o.w / 2 - 1, o.y + o.h / 2 - 1, 2, 2));   /* folds to its middle */
    }
    dk_compat_forget(w);
    if (dk_compat_live_id() == id) dk_compat_release();
    z_remove(id);
    if (s_last_front == id) s_last_front = -1;
    w->used = false;
    if (s_drag_win == id) s_drag = DRAG_NONE;
    bool apps = false;
    for (int i = 0; i < LS_DESK_MAX_WIN; i++) if (s_w[i].used && s_w[i].kind == DK_APP) apps = true;
    if (!apps) dk_compat_release();
    if (!dk_front_win()) s_full = false;
}

void ls_desk_close_all(void)
{
    for (int i = 0; i < LS_DESK_MAX_WIN; i++) if (s_w[i].used) ls_desk_close(s_w[i].id);
}

int ls_desk_window_count(void) { return s_nz; }

bool ls_desk_window_at(int z, ls_desk_win_info_t *o)
{
    if (z < 0 || z >= s_nz || !o) return false;
    const dk_win_t *w = dk_win(s_z[z]);
    if (!w) return false;
    tui_rect r, c;
    dk_win_rects(w, &r, &c);
    const dk_win_t *f = dk_front_win();
    *o = (ls_desk_win_info_t){
        .id = w->id, .app = w->kind == DK_APP, .state = w->state,
        .x = r.x, .y = r.y, .w = r.w, .h = r.h, .cx = c.x, .cy = c.y, .cw = c.w, .ch = c.h,
        .front = f == w, .has_snapshot = w->snap != NULL, .title = w->title,
    };
    return true;
}

int ls_desk_alloc_blocks(void) { return dk_compat_blocks(); }

void ls_desk_fullscreen(bool on)
{
    ls_tui_touch_client_clear();
    const dk_win_t *f = dk_front_win();
    s_full = on && f && f->kind == DK_APP;
    if (!s_full) s_focus = false;
    ls_tui_invalidate();
}
bool ls_desk_is_fullscreen(void) { return s_full; }

/* FOCUSED MODE: the front app alone, every cell, and the UI loop on a
   20 ms period instead of 40 (compact_ui reads ls_desk_focused). Nothing
   of the desk is drawn - no wallpaper, windows, taskbar or animation - and
   the other windows keep the last frame they drew. For a game or a heavy
   app; F11 or Alt+Enter, or Ctrl+F11 again, comes back to the desk. */
void ls_desk_focus(bool on)
{
    ls_desk_fullscreen(on);
    s_focus = on && s_full;
    s_focus_banner_us = s_focus ? esp_timer_get_time() + 2500000 : 0;
}
bool ls_desk_focused(void) { return s_on && s_full && s_focus; }

/* ------------------------------------------------------- app directory -- */

static const char *const GROUP_NAMES[5] = {"Radio", "Field", "System", "User apps", "Games"};
static const char *const GROUP_IDS[3][16] = {
    {"p25", "fm", "adsb", "falls", "mesh", "subghz", "digi", "labs", "experiments", "mixrf", "meters",
     "rec", "tmach", "radio", NULL},
    {"map", "tiles", "gps", "compass", "fox", "rfsurvey", "notes", "journal", "sweep", "drones", "cell", NULL},
    {"set", "link", "radios", "update", "diag", "terminal", "files", "music", NULL},
};

const char *dk_group_name(int g) { return g >= 0 && g < 5 ? GROUP_NAMES[g] : ""; }

static bool is_game(const ls_app_t *a)
{
    return a->cat == LS_APP_USER && (!strncmp(a->id, "lap", 3) || strstr(a->name, "CARRIER"));
}

static int listed_group(const ls_app_t *a)
{
    for (int g = 0; g < 3; g++)
        for (int i = 0; GROUP_IDS[g][i]; i++) if (!strcmp(GROUP_IDS[g][i], a->id)) return g;
    return -1;
}

int dk_group_apps(int g, const ls_app_t **out, int cap)
{
    int n = 0;
    if (g < 3) {
        for (int i = 0; GROUP_IDS[g][i] && n < cap; i++) {
            const ls_app_t *a = ls_app_by_id(GROUP_IDS[g][i]);
            if (a && a->screen) out[n++] = a;
        }
    }
    /* A built-in this list does not name still appears, at the end of Radio,
       as HOME does it; user apps and games by their category. */
    for (int i = 0; i < ls_app_count() && n < cap; i++) {
        const ls_app_t *a = ls_app_at(i);
        if (!a || !a->screen || !strcmp(a->id, "home") || strstr(a->id, "-design")) continue;
        if (a->cat == LS_APP_USER) {
            if ((g == 4) == is_game(a) && g >= 3) out[n++] = a;
        } else if (g == 0 && listed_group(a) < 0) {
            out[n++] = a;
        }
    }
    return n;
}

const char *dk_app_label(const ls_app_t *a)
{
    for (int i = 0; i < LS_DESK_MAX_WIN; i++)
        if (s_w[i].used && s_w[i].kind == DK_APP && s_w[i].app == a && s_w[i].label[0]) {
            const char *l = s_w[i].label;
            const size_t n = strlen(a->name);
            if (!strncmp(l, a->name, n) && l[n] == ' ') return l + n + 1;   /* the name is the row's */
            return "open";
        }
    return NULL;
}

/* ------------------------------------------------------------- actions -- */

static void reset_icons(void);

void dk_do(int act, int arg)
{
    dk_win_t *w = dk_win(arg);
    switch (act) {
    case A_OPEN_APP: open_app(arg >= 0 ? ls_app_at(arg) : NULL); break;
    case A_THEME: ls_desk_set_theme(arg); break;
    case A_ABOUT: ls_desk_open_about(); break;
    case A_DISPLAY:
        if (!dk_win_shell(DK_SHELL_DISPLAY)) dk_props_open();
        dk_open_shell(DK_SHELL_DISPLAY, "Display Properties", 66, 25);
        break;
    case A_SHUTDOWN: dk_open_shutdown(); break;
    case A_STUDIO: dk_open_studio(); break;
    case A_DESK_OFF: ls_desk_set_enabled(false); break;
    case A_ARRANGE:
        /* back to the columns, and forget where they were dragged */
        reset_icons();
        s_icon_sel = -1;
        ls_tui_invalidate();
        break;
    case A_WIN_RESTORE: ls_desk_restore(arg); break;
    case A_WIN_MIN: ls_desk_minimize(arg); break;
    case A_WIN_MAX: ls_desk_maximize(arg); break;
    case A_WIN_CLOSE: ls_desk_close(arg); break;
    case A_WIN_FULL: ls_desk_raise(arg); ls_desk_fullscreen(true); break;
    case A_WIN_FOCUS: ls_desk_raise(arg); ls_desk_focus(true); break;
    case A_BACK: if (w) { ls_desk_raise(arg); dk_compat_key(LS_TK_ESC, 0); } break;
    case A_OPTIONS: if (w) { ls_desk_raise(arg); dk_compat_key(LS_TK_CHAR, 'o'); } break;
    case A_LOCK: ls_tui_set_locked(true); break;
    case A_PARK: ls_tui_radio_want(NULL); break;
    case A_CLOSE_ALL: ls_desk_close_all(); break;
    case A_VOL_SET: dk_sys_set_volume(arg); break;
    case A_VOL_STEP: { const int v = dk_sys_volume(); if (v >= 0) dk_sys_set_volume(v + arg); break; }
    case A_MUTE: dk_sys_set_mute(arg != 0); break;
    case A_RX_RESUME: if (s_last_radio) ls_tui_radio_want(s_last_radio); break;
    case A_WALLPAPER: dk_wall_set((uint32_t)arg); break;
    default: break;
    }
}

/* --------------------------------------------------------------- icons -- */

static const struct { const char *id, *label; } ICONS[] = {
    {"diag", "MY BOARD"}, {"fm", "FM"}, {"map", "MAP"}, {"p25", "P25"}, {"adsb", "ADS-B"},
    {"terminal", "TERMINAL"}, {"mesh", "MESH CHAT"}, {"sweep", "SWEEP"}, {"set", "SETTINGS"},
    {"files", "SD CARD"}, {"radios", "RADIOS"}, {"gps", "GPS"}, {"studio", "STUDIO"},
};
#define N_ICONS ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

/* Where an icon was dragged to, cell for cell; -1 for its column slot. */
static int16_t s_icon_x[N_ICONS], s_icon_y[N_ICONS];
static bool s_icon_moved[N_ICONS];
static int s_drag_icon = -1;

static void load_icons(void)
{
    for (int j = 0; j < N_ICONS; j++) {
        const uint32_t v = dk_sys_saved_word(W_ICONS + j);
        s_icon_moved[j] = (v & W_VALID) != 0;
        s_icon_x[j] = (int16_t)(v & 0xFFF);
        s_icon_y[j] = (int16_t)(v >> 12 & 0xFFF);
    }
}

static void save_icon(int j)
{
    dk_sys_save_word(W_ICONS + j, s_icon_moved[j] ? W_VALID | (uint32_t)(s_icon_x[j] & 0xFFF) |
                                                   (uint32_t)(s_icon_y[j] & 0xFFF) << 12 : 0);
}

static void reset_icons(void)
{
    for (int j = 0; j < N_ICONS; j++) if (s_icon_moved[j]) { s_icon_moved[j] = false; save_icon(j); }
}

/* The i-th icon that exists in this build, its slot rect and its table index. */
static const ls_app_t *icon_find(int i, tui_rect *slot, int *index)
{
    const tui_rect work = dk_work();
    const int per_col = (work.h - 1) / 8 > 0 ? (work.h - 1) / 8 : 1;
    int k = 0;
    for (int j = 0; j < N_ICONS; j++) {
        const ls_app_t *a = !strcmp(ICONS[j].id, "studio") ? &s_studio_icon : ls_app_by_id(ICONS[j].id);
        if (!a || (!a->screen && a != &s_studio_icon)) continue;
        if (k == i) {
            if (slot) {
                *slot = tui_rect_make(1 + (k / per_col) * 13, 1 + (k % per_col) * 8, 12, 7);
                if (s_icon_moved[j]) {
                    /* kept on the work area as the grid changes */
                    int x = s_icon_x[j], y = s_icon_y[j];
                    if (x > work.w - 12) x = work.w - 12;
                    if (y > work.h - 7) y = work.h - 7;
                    slot->x = x < 0 ? 0 : x;
                    slot->y = y < 0 ? 0 : y;
                }
            }
            if (index) *index = j;
            return a;
        }
        k++;
    }
    return NULL;
}
static const ls_app_t *icon_at(int i, tui_rect *slot) { return icon_find(i, slot, NULL); }

static const char *icon_label(const ls_app_t *a)
{
    for (int j = 0; j < N_ICONS; j++) if (!strcmp(ICONS[j].id, a->id)) return ICONS[j].label;
    return a->name;
}

static int icon_hit(int col, int row)
{
    tui_rect r;
    for (int i = 0; icon_at(i, &r); i++) if (tui_rect_contains(r, col, row)) return i;
    return -1;
}

static void draw_desktop_cells(tui_surface *sf);

/* The ground, icons and wordmark cost ~4.5 ms a frame on the Nano and change
   only with what is in this key: drawn once, then copied. */
static void draw_desktop(tui_surface *sf)
{
    static struct {
        int w, rows, sel, anim, p50, p95;
        uint32_t wall;
        dk_prefs_t prefs;
        const void *theme;
        bool day, lat;
        uint8_t desk, text, select;
        tui_rect work;
        int16_t ix[N_ICONS], iy[N_ICONS];
        bool moved[N_ICONS];
    } key, now;
    static tui_cell *cells;
    static size_t cells_n;
    const size_t n = (size_t)sf->w * (s_rows - 1);
    if (!dk_wall_still() || s_rows < 2 || n > (size_t)sf->w * sf->h) { cells_n = 0; draw_desktop_cells(sf); return; }
    memset(&now, 0, sizeof(now));
    now.w = sf->w; now.rows = s_rows; now.sel = s_icon_sel;
    now.anim = s_icon_sel >= 0 ? dk_anim_icon_progress(s_icon_sel) : 0;
    now.lat = ls_input_latency(&now.p50, &now.p95);
    now.wall = dk_wall_current(); now.prefs = *dk_prefs();
    now.theme = ls_tui_get_theme(); now.day = ls_tui_daylight();
    now.desk = dk_attr(R_DESK); now.text = dk_attr(R_DESK_TEXT); now.select = dk_attr(R_SELECT);
    now.work = dk_work();
    memcpy(now.ix, s_icon_x, sizeof(now.ix)); memcpy(now.iy, s_icon_y, sizeof(now.iy));
    memcpy(now.moved, s_icon_moved, sizeof(now.moved));
    if (cells && cells_n == n && !memcmp(&key, &now, sizeof(now))) {
        memcpy(sf->back, cells, n * sizeof(tui_cell));
        return;
    }
    draw_desktop_cells(sf);
    if (cells_n != n) {
        heap_caps_free(cells);
        cells = heap_caps_malloc(n * sizeof(tui_cell), MALLOC_CAP_SPIRAM);
        cells_n = 0;
    }
    if (!cells) return;
    memcpy(cells, sf->back, n * sizeof(tui_cell));
    cells_n = n;
    memcpy(&key, &now, sizeof(key));
}

static void draw_desktop_cells(tui_surface *sf)
{
    const tui_rect work = dk_work();
    dk_wall_draw(sf, tui_rect_make(0, 0, s_cols, s_rows - 1));
    tui_rect r;
    /* the art first, glass over a picture; then the labels, solid */
    for (int i = 0; icon_at(i, &r); i++) {
        const ls_app_t *a = icon_at(i, NULL);
        const uint8_t bg = TUI_ATTR_BG(dk_attr(R_DESK));
        uint8_t hue = a->hue & 15;
        if (!dk_light_desk() && hue != TUI_WHITE) hue |= TUI_BRIGHT;
        if (dk_light_desk() && (hue & 7) == TUI_CYAN) hue = TUI_BLUE;
        ls_icon_draw(sf, work, r.x + 1, r.y, a->icon, TUI_ATTR(hue, bg));
    }
    dk_wall_glass(sf, work);
    for (int i = 0; icon_at(i, &r); i++) {
        const ls_app_t *a = icon_at(i, NULL);
        const char *l = icon_label(a);
        const int n = dk_text_len(l);
        const uint8_t la = i == s_icon_sel ? dk_attr(R_SELECT) : dk_attr(R_DESK_TEXT);
        const int lx = r.x + (r.w - n) / 2;
        if (dk_prefs()->labels || i == s_icon_sel)       /* a backing, so any wallpaper reads */
            dk_fill(sf, tui_rect_intersect(work, tui_rect_make(lx - 1, r.y + 6, n + 2, 1)),
                    i == s_icon_sel ? la : dk_attr(R_DESK_TEXT));
        dk_text(sf, work, lx, r.y + 6, l, dk_attr(R_DESK_TEXT));
        if (i == s_icon_sel) {
            const int k = dk_anim_icon_progress(i), half = (n + 2) * k / 2000 + 1;
            const int mid = lx + n / 2;
            for (int x = lx - 1; x <= lx + n; x++)
                if (x >= mid - half && x <= mid + half) {
                    tui_cell *c = &sf->back[(size_t)(r.y + 6) * sf->w + x];
                    if (tui_rect_contains(work, x, r.y + 6)) c->attr = la;
                }
        }
    }
    /* The wordmark, and what the link is doing; only facts. */
    char line[96];
    snprintf(line, sizeof(line), " LakeShark Desktop - %s - %dx%d cells ", DK_BOARD, s_cols, s_rows);
    const int n = dk_text_len(line);
    dk_text(sf, work, work.w - n - 1, work.h - 2, line, dk_attr(R_DESK_TEXT));
    int p50 = 0, p95 = 0;
    if (ls_input_latency(&p50, &p95)) {
        snprintf(line, sizeof(line), " Round trip to the board: %d ms (95%%: %d ms) ", p50, p95);
        dk_text(sf, work, work.w - dk_text_len(line) - 1, work.h - 1, line, dk_attr(R_DESK_TEXT));
    }
}

/* ------------------------------------------------------------- windows -- */

static void title_keys(const dk_win_t *w, tui_rect o, int *kmin, int *kmax, int *kclose)
{
    *kclose = o.x + o.w - 3;
    *kmax = w->kind == DK_APP || w->shell != DK_SHELL_ABOUT ? o.x + o.w - 6 : -1;
    *kmin = *kmax >= 0 ? o.x + o.w - 9 : o.x + o.w - 6;
}

/* Each app's mark in its title, in its hue. */
static int app_glyph(const ls_app_t *a)
{
    static const struct { const char *id; int cp; } G[] = {
        {"fm", '~'}, {"p25", 0x256B}, {"adsb", LS_SHAPE_UP}, {"map", '#'}, {"mesh", LS_BOX_DIAG_X},
        {"diag", LS_SHAPE_CHECK}, {"sweep", '*'}, {"rec", LS_SHAPE_DOT}, {"drones", 0x256C},
        {"gps", '+'}, {"set", '='}, {"terminal", '>'}, {"link", '&'}, {"radios", LS_SHAPE_RIGHT},
        {"subghz", LS_BOX_DBL_H}, {"music", LS_BOX_DBL_V},
    };
    for (unsigned i = 0; i < sizeof(G) / sizeof(G[0]); i++) if (!strcmp(G[i].id, a->id)) return G[i].cp;
    return LS_SHAPE_DOT;
}

/* A title key: a raised cap on the front window, lit under the pointer
   (with a glint the moment it arrives), flashing as it is let go; on a
   back window just its mark in the band's colours. */
static int s_hover_win = -1, s_hover_key;
static int64_t s_hover_us;
static void title_key(tui_surface *sf, tui_rect clip, const dk_win_t *w, int x, int y, int cp, int key,
                      bool front, uint8_t quiet, bool pressed)
{
    if (!front) {
        tui_put_char(sf, clip, x, y, '[', quiet);
        if (cp < 0x80) tui_put_char(sf, clip, x + 1, y, (char)cp, quiet);
        else ls_box_put(sf, clip, x + 1, y, cp, quiet);
        tui_put_char(sf, clip, x + 2, y, ']', quiet);
        return;
    }
    const bool hover = key && s_hover_win == w->id && s_hover_key == key && s_drag == DRAG_NONE;
    const int flash = dk_anim_key_progress(w->id, key);
    uint8_t cap = dk_attr(R_KEY);
    if (pressed || flash < 1000) cap = dk_attr(R_SELECT);
    else if (hover) cap = esp_timer_get_time() - s_hover_us < 120000 && dk_prefs()->anim ? dk_attr(R_SELECT) : dk_attr(R_TASKHOVER);
    const uint8_t glyph = key == KEY_CLOSE && !pressed && flash >= 1000 && !hover ? (uint8_t)((cap & 0xF0) | (dk_attr(R_CLOSE) & 0x0F)) : cap;
    tui_put_char(sf, clip, x, y, '[', cap);
    if (cp < 0x80) tui_put_char(sf, clip, x + 1 + (pressed ? 0 : 0), y, (char)cp, glyph);
    else ls_box_put(sf, clip, x + 1, y, cp, glyph);
    tui_put_char(sf, clip, x + 2, y, ']', cap);
}

static const char *status_text(const dk_win_t *w, bool front, char *buf, size_t cap)
{
    if (w->kind == DK_SHELL) return front ? "ready" : "";
    if (front) {
        const char *hint = w->app && w->app->screen ? w->app->screen->hint : NULL;
        return hint && hint[0] ? hint : "running";
    }
    const char *radio = w->app && w->app->screen ? w->app->screen->radio : NULL;
    const char *claimed = ls_tui_radio_claimed();
    if (radio && claimed && !strcmp(radio, claimed)) {
        snprintf(buf, cap, "receiving %s, view paused", radio);
        return buf;
    }
    return "paused - click to resume";
}

static void draw_window(tui_surface *sf, dk_win_t *w, bool front)
{
    tui_rect o, c;
    dk_win_rects(w, &o, &c);
    const tui_rect work = dk_work();
    const tui_rect clip = tui_rect_intersect(o, work);
    const bool max = w->state == LS_DESK_WIN_MAX;
    /* Title bar: one band. */
    const uint8_t ta = dk_attr(front ? R_TITLE_ON : R_TITLE_OFF);
    dk_fill(sf, tui_rect_intersect(tui_rect_make(o.x, o.y, o.w, 1), work), ta);
    /* A back window's keys sit quietly in its band: no white caps, no red
       cross floating beside the front window. */
    const uint8_t quiet = (uint8_t)((ta & 0xF0) | (TUI_ATTR_FG(ta)));
    title_key(sf, clip, w, o.x, o.y, '-', 0, front, quiet, 0);
    {
        int cp = LS_SHAPE_SQUARE;
        uint8_t hue = TUI_YELLOW | TUI_BRIGHT;
        if (w->kind == DK_APP && w->app) { cp = app_glyph(w->app); hue = (uint8_t)(w->app->hue | TUI_BRIGHT); }
        if (cp < 0x80) tui_put_char(sf, clip, o.x + 4, o.y, (char)cp, (uint8_t)((ta & 0xF0) | (hue & 0x0F)));
        else ls_box_put(sf, clip, o.x + 4, o.y, cp, (uint8_t)((ta & 0xF0) | (hue & 0x0F)));
    }
    int kmin, kmax, kclose;
    title_keys(w, o, &kmin, &kmax, &kclose);
    char title[64];
    if (w->kind == DK_APP) snprintf(title, sizeof(title), "%s  (desk layout, %dx%d)", w->title, c.w, c.h);
    else snprintf(title, sizeof(title), "%s", w->title);
    const tui_rect tclip = tui_rect_intersect(clip, tui_rect_make(o.x + 6, o.y, kmin - o.x - 7, 1));
    dk_text(sf, tclip, o.x + 6, o.y, title, ta);
    const bool pressing = s_drag == DRAG_KEY && s_drag_win == w->id;
    title_key(sf, clip, w, kmin, o.y, 0x2501, KEY_MIN, front, quiet, pressing && s_drag_key == KEY_MIN);
    if (kmax >= 0)
        title_key(sf, clip, w, kmax, o.y, max ? LS_SHAPE_RESTORE : LS_SHAPE_SQUARE, KEY_MAX, front, quiet,
                  pressing && s_drag_key == KEY_MAX);
    title_key(sf, clip, w, kclose, o.y, LS_SHAPE_TIMES, KEY_CLOSE, front, quiet, pressing && s_drag_key == KEY_CLOSE);
    /* Sides and bottom: the face out to the window's outer edge, the line
       on it: left HILITE, right and bottom SHADOW. A solid frame. */
    const int sy = max ? o.y + o.h - 1 : o.y + o.h - 2;
    if (!max) {
        const uint8_t face = dk_attr(R_FACE);
        dk_fill(sf, tui_rect_intersect(tui_rect_make(o.x, o.y + 1, 1, o.h - 1), clip), face);
        dk_fill(sf, tui_rect_intersect(tui_rect_make(o.x + o.w - 1, o.y + 1, 1, o.h - 1), clip), face);
        dk_fill(sf, tui_rect_intersect(tui_rect_make(o.x, o.y + o.h - 1, o.w, 1), clip), face);
        for (int i = 1; i < o.h - 1; i++) {
            dk_edge_put(sf, clip, o.x, o.y + i, LS_EDGE_L, R_HILITE);
            dk_edge_put(sf, clip, o.x + o.w - 1, o.y + i, LS_EDGE_R, R_SHADOW);
        }
        for (int i = 1; i < o.w - 1; i++) dk_edge_put(sf, clip, o.x + i, o.y + o.h - 1, LS_EDGE_B, R_SHADOW);
        dk_edge_put(sf, clip, o.x, o.y + o.h - 1, LS_EDGE_BL, R_HILITE);
        dk_edge_put(sf, clip, o.x + o.w - 1, o.y + o.h - 1, LS_EDGE_BR, R_SHADOW);
    }
    /* Status bar. */
    const int sx = max ? o.x : o.x + 1, sw = max ? o.w : o.w - 2;
    const tui_rect srow = tui_rect_intersect(clip, tui_rect_make(sx, sy, sw, 1));
    dk_fill(sf, srow, dk_attr(R_FACE));
    char buf[64];
    const char *st = status_text(w, front, buf, sizeof(buf));
    int x = sx + 1;
    x += dk_text(sf, srow, x, sy, st, front && w->kind == DK_APP ? dk_attr(R_FACE) : dk_attr(R_FACE));
    if (w->kind == DK_APP) {
        ls_box_put(sf, srow, x + 1, sy, LS_BOX_V, dk_attr(R_SHADOW));
        snprintf(buf, sizeof(buf), "%dx%d", c.w, c.h);
        dk_text(sf, srow, x + 3, sy, buf, dk_attr(R_STRONG));
        if (w->app && w->app->screen && w->app->screen->radio) {
            const int bx = x + 3 + dk_text_len(buf) + 1;
            ls_box_put(sf, srow, bx, sy, LS_BOX_V, dk_attr(R_SHADOW));
            dk_text(sf, srow, bx + 2, sy, w->app->screen->radio, dk_attr(R_INFO));
        }
    }
    if (!max) {
        /* the grip: dots stepping up to the corner, light over dark */
        ls_braille_put(sf, srow, sx + sw - 2, sy, (int16_t)0x28C0, dk_attr(R_HILITE));
        ls_braille_put(sf, srow, sx + sw - 1, sy, (int16_t)0x28F8, dk_attr(R_SHADOW));
    }
    /* The client. */
    const tui_rect cclip = tui_rect_intersect(c, work);
    if (w->kind == DK_SHELL) {
        dk_fill(sf, cclip, dk_attr(R_FACE));
        dk_shell_draw(sf, w, c, cclip, front);
    } else if (front) {
        dk_compat_draw_live(sf, w, c, cclip);
        /* The app's line, for its taskbar button. */
        const char *left = ls_tui_status_left();
        char line[sizeof(w->label)];
        if (left && left[0]) snprintf(line, sizeof(line), "%s %s", w->title, left);
        else snprintf(line, sizeof(line), "%s", w->title);
        memcpy(w->label, line, sizeof(line));
    } else {
        dk_compat_draw_snapshot(sf, w, c, cclip);
    }
}

/* --------------------------------------------------------------- taskbar -- */

/* One row of keys under the bar's raised top line, as the Windows 95 bar
   reads: every key solid face out to its side lines, raised or pressed;
   the tray the one sunken panel, its pieces clickable. Every colour here
   says something: an app's hue, a lamp's state, a latency band; nothing
   moves unless the state behind it did. */

#define TB_START_W 11                 /* |/| Start .| with a cell kept for the press */

/* A hue for text on the bar's face: bright on Bay 95's black, plain on
   Classic 95's silver (bright yellow and cyan vanish on silver). */
static uint8_t hue_fg(uint8_t hue, bool on_select)
{
    uint8_t c = hue & 7;
    if (on_select) return c == TUI_BLUE ? (TUI_CYAN | TUI_BRIGHT) : (uint8_t)(c | TUI_BRIGHT);
    if (!dk_light_face()) return c == TUI_WHITE ? (TUI_WHITE | TUI_BRIGHT) : (uint8_t)(c | TUI_BRIGHT);
    if (c == TUI_YELLOW) return TUI_RED;            /* readable on silver */
    if (c == TUI_CYAN) return TUI_BLUE;
    if (c == TUI_WHITE) return TUI_BLACK;
    return c;
}

static uint8_t on(uint8_t attr, uint8_t fg) { return (uint8_t)((attr & 0xF0) | (fg & 0x0F)); }

/* A taskbar key on the bar's row: solid face out to its side lines, which
   sit on the outer edges of its end cells. Raised: left HILITE, right
   SHADOW; pressed: the reverse. The bar's raised top line runs along the
   bottom of the row above, so the keys sit right under it with no gap. */
static void key_sides(tui_surface *sf, int x, int w, bool pressed)
{
    const tui_rect all = tui_surface_rect(sf);
    const int y = s_rows - 1;
    dk_edge_put(sf, all, x, y, LS_EDGE_L, pressed ? R_SHADOW : R_HILITE);
    dk_edge_put(sf, all, x + w - 1, y, LS_EDGE_R, pressed ? R_HILITE : R_SHADOW);
}

/* What the tray's pieces are, for clicks. */
enum { TR_NONE, TR_LINK, TR_SPK, TR_GPS, TR_RX, TR_MESH, TR_BT, TR_CLOCK };

typedef struct { char text[24]; uint8_t attr; int8_t spark; uint8_t item; } seg_t;

static struct { int x0, x1; uint8_t item; } s_tray_hits[12];
static int s_tray_hits_n;

/* The tray's contents, as coloured runs; `│` runs split it into groups. */
static int tray_build(seg_t *s, int cap_n)
{
    int n = 0;
    uint8_t item = TR_NONE;
    #define SEG(a, ...) do { if (n < cap_n) { snprintf(s[n].text, sizeof(s[n].text), __VA_ARGS__); \
                             s[n].attr = (a); s[n].spark = 0; s[n].item = item; n++; } } while (0)
    const uint8_t sh = dk_attr(R_SHADOW), tx = dk_attr(R_FACE);
    const int g0 = n;
    item = TR_LINK;
    if (s_lat_n > 1) {
        SEG(tx, " %*s", (s_lat_n + 1) / 2, "");      /* the sparkline goes over these */
        s[n - 1].spark = 1;
    }
    if (s_lat_last >= 0)
        SEG(s_lat_last < 60 ? dk_attr(R_GOOD) : s_lat_last < 150 ? dk_attr(R_WARN) : dk_attr(R_BAD), " %dms", s_lat_last);
    if (ls_rterm_status) {
        ls_rterm_status_t rs;
        ls_rterm_status(&rs);
        const int viewers = (rs.serial_on ? 1 : 0) + (rs.web_client ? 1 : 0) + (rs.client && rs.authed ? 1 : 0) +
                            (rs.ssh_client && rs.ssh_authed ? 1 : 0);
        const char *kind = rs.serial_on ? "USB" : rs.web_client ? "WEB" : rs.client ? "TCP" : rs.ssh_client ? "SSH" : NULL;
        if (kind) SEG(dk_attr(R_INFO), viewers > 1 ? " %s+%d" : " %s", kind, viewers - 1);
    }
    item = TR_NONE;
    if (n > g0) SEG(sh, " \xe2\x94\x82");                         /* │ */
    /* The speaker: its cone, and waves while there is sound. */
    item = TR_SPK;
    const int vol = dk_sys_volume();
    if (vol >= 0) {
        const bool muted = dk_sys_muted();
        SEG(dk_attr(R_STRONG), " \xe2\x97\x80");                  /* ◀ */
        if (muted) SEG(dk_attr(R_BAD), "\xc3\x97");               /* × */
        else SEG(vol > 0 ? dk_attr(R_INFO) : dk_attr(R_FACE), "%s", vol > 66 ? ")))" : vol > 33 ? "))" : vol > 0 ? ")" : "");
    }
    /* Lamps: the dot in the state's colour, the word in the face's. */
    if (ls_gps_get) {
        ls_gps_get(&s_gps);
        if (s_gps.alive) {
            item = TR_GPS;
            SEG(s_gps.fix ? dk_attr(R_GOOD) : dk_attr(R_WARN), " \xe2\x80\xa2");
            SEG(tx, "GPS");
            if (s_gps.fix) SEG(dk_attr(R_STRONG), " %s", s_gps.sats_used >= 4 ? "3D" : "2D");
            else SEG(dk_attr(R_WARN), " --");
        }
    }
    const char *claimed = ls_tui_radio_claimed();
    if (claimed) s_last_radio = claimed;
    if (claimed || s_last_radio) {
        item = TR_RX;
        SEG(claimed ? dk_attr(R_GOOD) : dk_attr(R_SHADOW), " \xe2\x80\xa2");
        SEG(tx, "%s", claimed ? claimed : "RX");
        if (!claimed) SEG(dk_attr(R_WARN), " parked");
    }
    item = TR_MESH;
    const int unread = ls_notify_unread ? ls_notify_unread() : 0;
    SEG(unread > 0 ? dk_attr(R_MESH) : dk_attr(R_SHADOW), " \xe2\x80\xa2");
    SEG(tx, unread > 0 ? "MESH " : "MESH");
    if (unread > 0) SEG(dk_attr(R_STRONG), "%d", unread > 99 ? 99 : unread);
    int bt_up = 0;
    const int bt = dk_sys_bt(&bt_up);
    if (bt >= 0) {
        item = TR_BT;
        SEG(bt_up ? dk_attr(R_INFO) : bt ? dk_attr(R_WARN) : dk_attr(R_SHADOW), " \xe2\x80\xa2");
        SEG(tx, "KBD");
    }
    item = TR_NONE;
    SEG(sh, " \xe2\x94\x82");
    item = TR_CLOCK;
    const char *clock = ls_tui_status_clock();
    SEG(dk_attr(R_STRONG), " %s ", clock && clock[0] ? clock : "--:--");
    #undef SEG
    return n;
}

/* Taskbar buttons, left to right, in the order the windows opened. */
static int task_order(int *ids)
{
    int n = 0;
    for (int i = 0; i < LS_DESK_MAX_WIN; i++) if (s_w[i].used) ids[n++] = i;
    for (int a = 1; a < n; a++)
        for (int b = a; b > 0 && s_w[ids[b]].id < s_w[ids[b - 1]].id; b--) {
            const int t = ids[b]; ids[b] = ids[b - 1]; ids[b - 1] = t;
        }
    for (int i = 0; i < n; i++) ids[i] = s_w[ids[i]].id;
    return n;
}

static int s_tray_x;

/* Buttons from x0, each bw wide with a one-cell gap. Wider on a big grid. */
static void task_geometry(int *x0, int *bw, int *n, int *ids)
{
    *n = task_order(ids);
    *x0 = TB_START_W + 1;
    const int free = s_tray_x - 1 - *x0;
    const int most = s_cols >= 300 ? 30 : s_cols >= 200 ? 24 : 20;
    *bw = *n ? free / *n - 1 : 0;
    if (*bw > most) *bw = most;
}

static int task_hit(int col)
{
    int ids[LS_DESK_MAX_WIN], n, x0, bw;
    task_geometry(&x0, &bw, &n, ids);
    if (bw < 6 || col < x0) return -1;
    const int i = (col - x0) / (bw + 1);
    return i < n && (col - x0) % (bw + 1) != bw ? ids[i] : -1;
}

static int tray_hit(int col)
{
    for (int i = 0; i < s_tray_hits_n; i++)
        if (col >= s_tray_hits[i].x0 && col < s_tray_hits[i].x1) return s_tray_hits[i].item;
    return TR_NONE;
}

static int tray_item_x(int item)
{
    for (int i = 0; i < s_tray_hits_n; i++) if (s_tray_hits[i].item == item) return s_tray_hits[i].x0;
    return s_tray_x;
}

static const char *name_of(const dk_win_t *w) { return w->app && w->app->name ? w->app->name : w->title; }

static bool hovering(int x, int w)
{
    return s_ptr_row == s_rows - 1 && s_ptr_col >= x && s_ptr_col < x + w && s_drag == DRAG_NONE;
}

static void draw_start(tui_surface *sf)
{
    const tui_rect all = tui_surface_rect(sf);
    const int y = s_rows - 1;
    const bool down = s_pressed_start || (dk_menu_depth() && dk_menu_at(0)->id == M_START);
    const bool hover = !down && hovering(0, TB_START_W);
    const uint8_t face = down ? dk_attr(R_SELECT) : hover ? dk_attr(R_TASKHOVER) : dk_attr(R_TASKKEY);
    dk_fill(sf, tui_rect_make(0, y, TB_START_W, 1), face);
    key_sides(sf, 0, TB_START_W, down);
    /* The fin, and the word with its accelerator; a pressed key's label
       moves one cell right, into the spare cell kept for it. */
    const int x = 1 + (down ? 1 : 0);
    const bool silver = dk_light_face() && !down;
    ls_box_put(sf, all, x, y, LS_BOX_DIAG_R, on(face, silver ? TUI_BLUE : (TUI_CYAN | TUI_BRIGHT)));
    ls_box_put(sf, all, x + 1, y, LS_BOX_V, on(face, silver ? TUI_BLUE : (TUI_CYAN | TUI_BRIGHT)));
    const uint8_t word = on(face, down ? (TUI_WHITE | TUI_BRIGHT) : dk_attr(R_TASKKEY));
    dk_text_accel(sf, all, x + 3, y, "Start", word,
                  on(face, down ? (TUI_YELLOW | TUI_BRIGHT) : silver ? TUI_RED : (TUI_YELLOW | TUI_BRIGHT)));
    const int k = dk_anim_start_progress();
    if (k < 1000) {
        /* a glint runs along the word and off the fin as the key goes down */
        static const char W[] = "Start";
        const int at = k * 7 / 1000 - 1;
        if (at >= 0 && at < 5) tui_put_char(sf, all, x + 3 + at, y, W[at], on(face, TUI_YELLOW | TUI_BRIGHT));
        if (at == -1 || at == 0) ls_box_put(sf, all, x, y, LS_BOX_DIAG_R, on(face, TUI_WHITE | TUI_BRIGHT));
    }
}

/* One window's button: its hue as a tag, the app's live line coloured.
   Front: pressed on the selection colour. On screen behind: a raised key.
   Minimized: flat on the bar, with a hollow tag. */
static void draw_button(tui_surface *sf, dk_win_t *w, int x, int bw, bool front)
{
    const int y = s_rows - 1;
    const bool min = w->state == LS_DESK_WIN_MIN;
    const bool hover = !front && hovering(x, bw);
    const uint8_t face = front ? dk_attr(R_SELECT) : hover ? dk_attr(R_TASKHOVER)
                       : min ? dk_attr(R_FACE) : dk_attr(R_TASKKEY);
    dk_fill(sf, tui_rect_make(x, y, bw, 1), face);
    key_sides(sf, x, bw, front);
    const tui_rect inner = tui_rect_make(x + 1, y, bw - 2, 1);
    int cx = x + 1 + (front ? 1 : 0);
    const bool on_sel = front || hover;
    if (w->kind == DK_APP && w->app)
        ls_box_put(sf, inner, cx, y, min ? LS_SHAPE_SQUARE : LS_SHAPE_DOT, on(face, hue_fg(w->app->hue, on_sel)));
    else
        ls_box_put(sf, inner, cx, y, LS_SHAPE_SQUARE, on(face, on_sel ? (TUI_YELLOW | TUI_BRIGHT) : hue_fg(TUI_YELLOW, false)));
    cx += 2;
    /* The name strong (plain when minimized), the live part in the app's
       hue: the app's own state, else what its screen put on the status row. */
    const size_t nl = strlen(w->title);
    char live[20];
    const char *lv = w->app ? dk_live_line(w->app, live, sizeof(live)) : NULL;
    if (!lv && !strncmp(w->label, w->title, nl) && w->label[nl] == ' ') {
        snprintf(live, sizeof(live), "%s", w->label + nl + 1);
        lv = live;
    }
    if (lv) {
        char line[sizeof(w->label)];
        snprintf(line, sizeof(line), "%s %s", name_of(w), lv);
        memcpy(w->label, line, sizeof(line));
    }
    const uint8_t name_at = on_sel ? on(face, TUI_WHITE | TUI_BRIGHT) : min ? face : on(face, dk_attr(R_TASKKEY));
    char name[sizeof(w->title)];
    snprintf(name, sizeof(name), "%s", w->title);
    cx += dk_text(sf, inner, cx, y, name, name_at);
    if (lv) {
        const uint8_t hue = w->app ? hue_fg(w->app->hue, on_sel) : dk_hue(TUI_CYAN);
        dk_text(sf, inner, cx + 1, y, lv, on(face, front ? (TUI_YELLOW | TUI_BRIGHT) : hue));
    }
}

static void draw_tray(tui_surface *sf)
{
    const tui_rect all = tui_surface_rect(sf);
    const int y = s_rows - 1;
    seg_t seg[24];
    const int ns = tray_build(seg, 24);
    int tw = 0;
    for (int i = 0; i < ns; i++) tw += dk_text_len(seg[i].text);
    const int w = tw + 2;                                   /* its two sides */
    s_tray_x = s_cols - w;
    /* Sunken: solid out to its sides, left SHADOW, right HILITE. */
    dk_fill(sf, tui_rect_make(s_tray_x, y, w, 1), dk_attr(R_TRAY));
    dk_edge_put(sf, all, s_tray_x, y, LS_EDGE_L, R_SHADOW);
    dk_edge_put(sf, all, s_cols - 1, y, LS_EDGE_R, R_HILITE);
    int tx = s_tray_x + 1;
    s_tray_hits_n = 0;
    for (int i = 0; i < ns; i++) {
        const int at = tx;
        tx += dk_text(sf, all, tx, y, seg[i].text, on(dk_attr(R_TRAY), seg[i].attr));
        if (seg[i].item) {
            if (s_tray_hits_n && s_tray_hits[s_tray_hits_n - 1].item == seg[i].item)
                s_tray_hits[s_tray_hits_n - 1].x1 = tx;
            else if (s_tray_hits_n < (int)(sizeof(s_tray_hits) / sizeof(s_tray_hits[0])))
                s_tray_hits[s_tray_hits_n++] = (typeof(s_tray_hits[0])){at, tx, seg[i].item};
        }
        if (seg[i].spark) {
            /* Two samples a cell, each cell coloured by the worse of its pair. */
            int mx = 1;
            for (int k = 0; k < s_lat_n; k++) if (s_lat[k] > mx) mx = s_lat[k];
            for (int k = 0; k < s_lat_n; k += 2) {
                const int worst = k + 1 < s_lat_n && s_lat[k + 1] > s_lat[k] ? s_lat[k + 1] : s_lat[k];
                const uint8_t a = worst < 60 ? dk_attr(R_GOOD) : worst < 150 ? dk_attr(R_WARN) : dk_attr(R_BAD);
                dk_spark(sf, all, at + 1 + k / 2, y, s_lat + k, k + 1 < s_lat_n ? 2 : 1, mx, on(dk_attr(R_TRAY), a));
            }
        }
    }
}

static void draw_taskbar(tui_surface *sf)
{
    const int y = s_rows - 1, top = s_rows - 2;
    const tui_rect all = tui_surface_rect(sf);
    dk_fill(sf, tui_rect_make(0, y, s_cols, 1), dk_attr(R_FACE));
    /* The bar's raised top line, on the bottom edge of the row above (the
       desktop's, wallpaper and all), so the bar starts at the line. */
    for (int x = 0; x < s_cols; x++) dk_edge_put(sf, all, x, top, LS_EDGE_B, R_HILITE);
    dk_wall_glass(sf, tui_rect_make(0, top, s_cols, 1));
    draw_tray(sf);
    draw_start(sf);
    int ids[LS_DESK_MAX_WIN], n, x0, bw;
    task_geometry(&x0, &bw, &n, ids);
    const dk_win_t *front = dk_front_win();
    for (int i = 0; i < n && bw >= 6; i++) {
        dk_win_t *w = dk_win(ids[i]);
        if (w) draw_button(sf, w, x0 + i * (bw + 1), bw, w == front);
    }
}

/* ---------------------------------------------------------------- draw -- */

static void relayout(int cols, int rows)
{
    s_cols = cols;
    s_rows = rows;
    for (int i = 0; i < LS_DESK_MAX_WIN; i++) {
        dk_win_t *w = &s_w[i];
        if (!w->used) continue;
        clamp(w);
        tui_rect c;
        dk_win_rects(w, NULL, &c);
        if (w->snap && (w->sw != c.w || w->sh != c.h)) dk_compat_forget(w);   /* a frame of another size is dropped */
    }
}

static void poll_finger(void);
static void dimmed(tui_surface *sf);
static void draw_tip(tui_surface *sf);
static void viewer_watch(void);

static void apply_console(void);
/* 'desk cost': where a desk frame's time goes, per stage, averaged (1/8) and
   the worst since the last read. */
enum { DC_DESKTOP, DC_BACK, DC_FRONT, DC_AFTER, DC_TASKBAR, DC_REST, DC_N };
static const char *const DC_NAME[DC_N] = { "wallpaper+icons", "back windows", "front window",
                                           "wall after", "taskbar", "menus+tips" };
static uint32_t s_dc_avg[DC_N], s_dc_max[DC_N];
static void dc_add(int k, int64_t us)
{
    const uint32_t u = (uint32_t)(us > 0 ? us : 0);
    s_dc_avg[k] = s_dc_avg[k] ? (s_dc_avg[k] * 7 + u) / 8 : u;
    if (u > s_dc_max[k]) s_dc_max[k] = u;
}

void ls_desk_draw(tui_surface *sf)
{
    if (!sf) return;
    apply_console();
    if (!s_on || ls_tui_locked()) { ls_tui_router_draw(sf); return; }
    if (sf->w != s_cols || sf->h != s_rows) relayout(sf->w, sf->h);
    palette_follow();
    poll_finger();

    /* The window that just left the front keeps its last frame. */
    dk_win_t *front = dk_front_win();
    const int fid = front ? front->id : -1;
    if (fid != s_last_front) {
        dk_win_t *old = dk_win(s_last_front);
        if (old && old->kind == DK_APP && dk_compat_live_id() == old->id) dk_compat_snapshot(old);
        s_last_front = fid;
    }
    /* Latency: a sample a second, only when there is one. */
    int p50 = 0, p95 = 0;
    const int64_t now = esp_timer_get_time();
    if (now - s_lat_us >= 1000000 && ls_input_latency(&p50, &p95)) {
        s_lat_us = now;
        if (s_lat_n == 16) { memmove(s_lat, s_lat + 1, sizeof(s_lat) - sizeof(s_lat[0])); s_lat_n--; }
        s_lat[s_lat_n++] = p50;
        s_lat_last = p50;
    }

    const bool small = sf->w < DESK_MIN_COLS || sf->h < DESK_MIN_ROWS;
    if ((s_full || small) && front && front->kind == DK_APP) {
        /* The app gets every cell, as it would without the desk. */
        dk_compat_select(front);
        ls_tui_screen_set_full(true);
        ls_tui_router_draw(sf);
        if (s_focus && esp_timer_get_time() < s_focus_banner_us) {
            /* Two seconds to say where the desk went, then the app has the row back. */
            char msg[96];
            snprintf(msg, sizeof(msg), " FOCUSED: %s at full rate   F11 returns to the desk ",
                     front->app && front->app->name ? front->app->name : "app");
            const int n = dk_text_len(msg);
            dk_text(sf, tui_surface_rect(sf), (sf->w - n) / 2 > 0 ? (sf->w - n) / 2 : 0, 0, msg, dk_attr(R_TIP));
        }
        if (small) {
            const char *msg = " The PC window is too small for the desktop (100x30). ";
            dk_text(sf, tui_surface_rect(sf), 0, sf->h - 1, msg, dk_attr(R_TIP));
        }
        return;
    }
    s_full = false;
    s_focus = false;
    tui_frame_begin(sf);
    int64_t t0 = esp_timer_get_time(), t1;
    draw_desktop(sf);
    t1 = esp_timer_get_time(); dc_add(DC_DESKTOP, t1 - t0); t0 = t1;
    int64_t front_us = 0;
    for (int i = 0; i < s_nz; i++) {
        dk_win_t *w = dk_win(s_z[i]);
        if (!w || w->state == LS_DESK_WIN_MIN) continue;
        const int64_t ws = esp_timer_get_time();
        draw_window(sf, w, w == front);
        if (w == front) front_us = esp_timer_get_time() - ws;
    }
    t1 = esp_timer_get_time(); dc_add(DC_BACK, t1 - t0 - front_us); dc_add(DC_FRONT, front_us); t0 = t1;
    tui_rect keep = tui_rect_make(0, 0, 0, 0);
    if (front && front->kind == DK_APP) dk_win_rects(front, NULL, &keep);
    dk_wall_after_windows(sf, tui_rect_make(0, 0, s_cols, s_rows - 1), keep);
    t1 = esp_timer_get_time(); dc_add(DC_AFTER, t1 - t0); t0 = t1;
    if ((s_drag == DRAG_MOVE || s_drag == DRAG_SIZE) && s_drag_moved && !dk_prefs()->live_drag)
        dk_frame_march(sf, dk_work(), s_outline, dk_attr(R_STRONG), s_outline_step);
    draw_taskbar(sf);
    t1 = esp_timer_get_time(); dc_add(DC_TASKBAR, t1 - t0); t0 = t1;
    dk_anim_draw_trails(sf);
    dimmed(sf);
    dk_menu_draw(sf);
    draw_tip(sf);
    viewer_watch();
    if (dk_anim_splash_on()) dk_anim_draw(sf);
    dc_add(DC_REST, esp_timer_get_time() - t0);
}

/* The taskbar button of a window, for the zoom trails. */
static tui_rect task_rect(int id)
{
    int ids[LS_DESK_MAX_WIN], n, x0, bw;
    task_geometry(&x0, &bw, &n, ids);
    for (int i = 0; i < n; i++)
        if (ids[i] == id && bw >= 6) return tui_rect_make(x0 + i * (bw + 1), s_rows - 1, bw, 1);
    return tui_rect_make(0, s_rows - 1, TB_START_W, 1);
}

/* The Shut Down dialog's dimmed desktop: everything else in the shadow
   colours with a halftone over its empty cells, swept in from the top left
   as the dialog opens. */
static void dimmed(tui_surface *sf)
{
    const dk_win_t *sd = dk_win_shell(DK_SHELL_SHUTDOWN);
    if (!sd) return;
    tui_rect keep;
    dk_win_rects(sd, &keep, NULL);
    const int k = dk_anim_dim_progress();
    const int reach = (sf->w + sf->h) * k / 1000;
    for (int y = 0; y < sf->h; y++)
        for (int x = 0; x < sf->w; x++) {
            if (tui_rect_contains(keep, x, y) || x + y > reach) continue;
            tui_cell *c = &sf->back[(size_t)y * sf->w + x];
            if (c->ch < 0x100 && (uint8_t)c->ch == (uint8_t)LS_TUI_IMAGE_CELL) c->ch = ' ';
            if (c->ch == ' ' || c->ch == 0) {
                c->ch = (int16_t)(((x + y) & 1) ? 0x2855 : 0x28AA);
                c->attr = TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK);
            } else {
                c->attr = TUI_ATTR(TUI_BLACK | TUI_BRIGHT, TUI_BLACK);
            }
        }
}

/* Tooltips: half a second still over a key, its words typed in a letter
   at a time; the next move takes it away. */
static int s_tip_col = -1, s_tip_row = -1;
static int64_t s_tip_us;
static void hover_at(int col, int row)
{
    if (col != s_tip_col || row != s_tip_row) { s_tip_col = col; s_tip_row = row; s_tip_us = esp_timer_get_time(); }
    /* the title key under the pointer */
    int win = -1, key = 0;
    const dk_win_t *f = dk_front_win();
    if (f) {
        tui_rect o;
        dk_win_rects(f, &o, NULL);
        if (row == o.y) {
            int kmin, kmax, kclose;
            title_keys(f, o, &kmin, &kmax, &kclose);
            key = col >= kclose && col <= kclose + 2 ? KEY_CLOSE : kmax >= 0 && col >= kmax && col <= kmax + 2 ? KEY_MAX
                : col >= kmin && col <= kmin + 2 ? KEY_MIN : 0;
            if (key) win = f->id;
        }
    }
    if (win != s_hover_win || key != s_hover_key) { s_hover_win = win; s_hover_key = key; s_hover_us = esp_timer_get_time(); }
}

static bool tip_text(int col, int row, char *buf, size_t n)
{
    if (s_hover_win >= 0 && s_hover_key)
        return snprintf(buf, n, "%s", s_hover_key == KEY_CLOSE ? "Close" : s_hover_key == KEY_MAX ? "Maximize" : "Minimize") > 0;
    if (row != s_rows - 1) return false;
    if (col < TB_START_W) return snprintf(buf, n, "Click here to begin.") > 0;
    if (col >= s_tray_x) {
        switch (tray_hit(col)) {
        case TR_SPK: return snprintf(buf, n, dk_sys_muted() ? "Sound off" : "Volume %d%%", dk_sys_volume()) > 0;
        case TR_CLOCK: {
            const char *c = ls_tui_status_clock();
            return snprintf(buf, n, "UTC %s - click for the date", c && c[0] ? c : "--:--") > 0;
        }
        case TR_GPS: return snprintf(buf, n, s_gps.fix ? "GPS fix, %d satellites" : "GPS: no fix yet", s_gps.sats_used) > 0;
        case TR_RX: { const char *r = ls_tui_radio_claimed(); return snprintf(buf, n, r ? "Receiving: %s" : "Receiver parked", r) > 0; }
        case TR_MESH: return snprintf(buf, n, "Mesh chat") > 0;
        case TR_BT: return snprintf(buf, n, "Bluetooth keyboards") > 0;
        case TR_LINK: return snprintf(buf, n, s_lat_last >= 0 ? "Round trip to the board: %d ms" : "The link to this viewer", s_lat_last) > 0;
        default: return false;
        }
    }
    const int id = task_hit(col);
    const dk_win_t *w = dk_win(id);
    return w && snprintf(buf, n, "%s", w->label) > 0;
}

static void draw_tip(tui_surface *sf)
{
    if (dk_menu_depth() || s_drag != DRAG_NONE || s_tip_col < 0) return;
    const int64_t held = esp_timer_get_time() - s_tip_us;
    if (held < 500000) return;
    char t[48];
    if (!tip_text(s_tip_col, s_tip_row, t, sizeof(t))) return;
    const int n = dk_text_len(t);
    const int shown = dk_prefs()->anim ? (int)((held - 500000) / 18000) : n;
    int x = s_tip_col, y = s_tip_row == s_rows - 1 ? s_rows - 2 : s_tip_row + 1;
    if (x + n + 2 > sf->w) x = sf->w - n - 2;
    if (x < 0) x = 0;
    char part[48];
    snprintf(part, sizeof(part), " %.*s", shown < n ? shown : n, t);
    const tui_rect r = tui_rect_make(x, y, (shown < n ? shown : n) + 2, 1);
    dk_fill(sf, r, dk_attr(R_TIP));
    dk_text(sf, r, x, y, part, dk_attr(R_TIP));
}

/* A viewer arriving gets the welcome. */
static int s_viewers = -1;
static void viewer_watch(void)
{
    if (!ls_rterm_status) return;
    ls_rterm_status_t rs;
    ls_rterm_status(&rs);
    const int v = (rs.serial_on ? 1 : 0) + (rs.web_client ? 1 : 0) + (rs.client && rs.authed ? 1 : 0) +
                  (rs.ssh_client && rs.ssh_authed ? 1 : 0);
    if (s_viewers >= 0 && v > s_viewers) dk_anim_splash();
    s_viewers = v;
}

/* --------------------------------------------------------------- input -- */

static dk_win_t *win_at(int col, int row)
{
    for (int i = s_nz - 1; i >= 0; i--) {
        dk_win_t *w = dk_win(s_z[i]);
        if (!w || w->state == LS_DESK_WIN_MIN) continue;
        tui_rect o;
        dk_win_rects(w, &o, NULL);
        if (tui_rect_contains(o, col, row)) return w;
    }
    return NULL;
}

static void cycle_windows(void)
{
    if (s_nz < 2) { if (s_nz == 1) ls_desk_raise(s_z[0]); return; }
    ls_desk_raise(s_z[0]);                        /* the bottom one comes up */
}

static void context_menu_for(dk_win_t *w, int col, int row, bool title)
{
    if (!w) { dk_menu_open(M_DESKTOP, 0, col, row); return; }
    dk_menu_open(title || w->kind != DK_APP ? M_WINDOW : M_APPCTX, w->id, col, row);
}

static void toggle_start(void)
{
    if (dk_menu_depth() && dk_menu_at(0)->id == M_START) { dk_menu_close_all(); return; }
    dk_anim_start();
    dk_menu_open(M_START, 0, 0, s_rows - 2);
}

static bool desk_key(ls_tk_t k, char ch, uint8_t mods)
{
    const bool ctrl = mods & LS_IMOD_CTRL, alt = mods & LS_IMOD_ALT;
    if (k == LS_TK_META || k == LS_TK_MENU || (ctrl && k == LS_TK_ESC) || k == LS_TK_F10) { toggle_start(); return true; }
    if (dk_menu_depth()) return dk_menu_key(k, ch);
    dk_win_t *front = dk_front_win();
    if (ctrl && k == LS_TK_F11) { ls_desk_focus(!s_focus); return true; }
    if (k == LS_TK_F11 || (alt && k == LS_TK_ENTER)) { ls_desk_fullscreen(!s_full); return true; }
    if (k == LS_TK_F9) { ls_desk_set_theme((ls_desk_theme() + 1) % LS_DESK_THEME_COUNT); return true; }
    if (k == LS_TK_F6 || (ctrl && k == LS_TK_CHAR && ch == '`')) { cycle_windows(); return true; }
    if (ctrl && k == LS_TK_F4) { if (front) ls_desk_close(front->id); return true; }
    if (front) {
        bool took = false;
        if (front->kind == DK_SHELL) took = dk_shell_key(front, k, ch);
        else took = dk_compat_key(k, ch);
        if (!took && k == LS_TK_ESC) {
            /* An Esc nobody wanted is the right button: the window's menu. */
            tui_rect c;
            dk_win_rects(front, NULL, &c);
            const bool inside = tui_rect_contains(c, s_ptr_col, s_ptr_row);
            context_menu_for(front, inside ? s_ptr_col : c.x + 1, inside ? s_ptr_row : c.y, false);
        }
        return true;
    }
    /* The desktop: arrows walk the icons, Enter opens. */
    int count = 0;
    while (icon_at(count, NULL)) count++;
    const tui_rect work = dk_work();
    const int per_col = (work.h - 1) / 8 > 0 ? (work.h - 1) / 8 : 1;
    switch (k) {
    case LS_TK_DOWN:  s_icon_sel = s_icon_sel < 0 ? 0 : s_icon_sel + 1; break;
    case LS_TK_UP:    s_icon_sel = s_icon_sel <= 0 ? 0 : s_icon_sel - 1; break;
    case LS_TK_RIGHT: s_icon_sel = s_icon_sel < 0 ? 0 : s_icon_sel + per_col; break;
    case LS_TK_LEFT:  s_icon_sel = s_icon_sel < per_col ? 0 : s_icon_sel - per_col; break;
    case LS_TK_ENTER: if (s_icon_sel >= 0) open_app(icon_at(s_icon_sel, NULL)); return true;
    case LS_TK_ESC:   dk_menu_open(M_DESKTOP, 0, s_ptr_col, s_ptr_row); return true;
    case LS_TK_TAB:   if (s_nz) ls_desk_raise(s_z[s_nz - 1]); return true;
    default: return true;
    }
    if (s_icon_sel >= count) s_icon_sel = count - 1;
    return true;
}

static void title_press(dk_win_t *w, tui_rect o, int col, bool dbl)
{
    int kmin, kmax, kclose;
    title_keys(w, o, &kmin, &kmax, &kclose);
    if (col >= o.x && col <= o.x + 2) {
        if (dbl) ls_desk_close(w->id);
        else dk_menu_open(M_WINDOW, w->id, o.x, o.y + 1);
        return;
    }
    int key = 0;
    if (col >= kclose && col <= kclose + 2) key = KEY_CLOSE;
    else if (kmax >= 0 && col >= kmax && col <= kmax + 2) key = KEY_MAX;
    else if (col >= kmin && col <= kmin + 2) key = KEY_MIN;
    if (key) { s_drag = DRAG_KEY; s_drag_key = key; s_drag_win = w->id; return; }
    if (dbl) { ls_desk_maximize(w->id); return; }
    if (w->state == LS_DESK_WIN_MAX) return;
    s_drag = DRAG_MOVE;
    s_drag_win = w->id;
    s_drag_dx = col - o.x;
    s_drag_dy = 0;
    s_outline = o;
}

/* The tray's pieces open their popups above themselves. */
static void tray_click(int item, int button)
{
    const int x = tray_item_x(item), y = s_rows;      /* clamped up onto the bar */
    switch (item) {
    case TR_SPK:   dk_menu_open(button == LS_IBTN_RIGHT ? M_SOUND : M_VOLUME, 0, x, y); break;
    case TR_CLOCK: dk_menu_open(M_CLOCK, 0, x, y); break;
    case TR_RX:    dk_menu_open(M_RX, 0, x, y); break;
    case TR_MESH:  dk_menu_open(M_MESH, 0, x, y); break;
    case TR_BT:    dk_menu_open(M_BT, 0, x, y); break;
    case TR_GPS:   dk_menu_open(M_GPS, 0, x, y); break;
    default: break;
    }
}

/* A tray popup by name, as its click opens it (tests, the console). */
bool ls_desk_tray_popup(const char *name, bool right)
{
    static const struct { const char *name; int item; } T[] = {
        {"volume", TR_SPK}, {"clock", TR_CLOCK}, {"radio", TR_RX}, {"mesh", TR_MESH},
        {"keyboard", TR_BT}, {"gps", TR_GPS},
    };
    for (unsigned i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (name && !strcmp(name, T[i].name)) {
            tray_click(T[i].item, right ? LS_IBTN_RIGHT : LS_IBTN_LEFT);
            return dk_menu_depth() > 0;
        }
    return false;
}

/* Hand the app in the front window the held finger, in its own coordinates:
   sliders, map pan and game sticks follow it. Only the live front app. */
static void client_finger(const ls_input_ev_t *ev, bool down)
{
    dk_win_t *w = dk_win(s_drag_win);
    if (!w || w->kind != DK_APP || !s_down_was_front || w != dk_front_win()) return;
    tui_rect c;
    dk_win_rects(w, NULL, &c);
    int cw = 0, ch = 0;
    ls_tui_geometry(NULL, NULL, &cw, &ch);
    dk_compat_finger(ev->col - c.x, ev->row - c.y, ev->px - c.x * cw, ev->py - c.y * ch, down);
}

static bool desk_pointer(const ls_input_ev_t *ev)
{
    const int col = ev->col, row = ev->row;
    s_ptr_col = col;
    s_ptr_row = row;
    const int bar = s_rows - 1;
    if (ev->kind == LS_IEV_PTR_MOVE) {
        if (s_drag == DRAG_MOVE) {
            const tui_rect work = dk_work();
            int x = col - s_drag_dx, y = row - s_drag_dy;
            if (y < 0) y = 0;
            if (y > work.h - 1) y = work.h - 1;
            if (x < 1 && x > -2) x = 0;                               /* snap to the edges within a cell */
            if (x + s_outline.w > work.w - 1 && x + s_outline.w < work.w + 2) x = work.w - s_outline.w;
            s_outline.x = x;
            s_outline.y = y;
            s_drag_moved = true;
            s_outline_step++;                              /* the outline marches as it moves */
            dk_win_t *w = dk_win(s_drag_win);
            if (w && dk_prefs()->live_drag) { w->x = x; w->y = y; clamp(w); }
        } else if (s_drag == DRAG_ICON && s_drag_icon >= 0) {
            const tui_rect work = dk_work();
            int x = col - s_drag_dx, y = row - s_drag_dy;
            if (x < 0) x = 0;
            if (y < 0) y = 0;
            if (x > work.w - 12) x = work.w - 12;
            if (y > work.h - 7) y = work.h - 7;
            if (s_drag_moved || col != s_down_col || row != s_down_row) {
                s_icon_x[s_drag_icon] = (int16_t)x;
                s_icon_y[s_drag_icon] = (int16_t)y;
                s_icon_moved[s_drag_icon] = true;
                s_drag_moved = true;
            }
        } else if (s_drag == DRAG_SIZE) {
            dk_win_t *w = dk_win(s_drag_win);
            if (w) {
                int ww = col - s_outline.x + 1, hh = row - s_outline.y + 1;
                if (ww < min_w(w)) ww = min_w(w);
                if (hh < min_h(w)) hh = min_h(w);
                const tui_rect work = dk_work();
                if (s_outline.x + ww > work.w) ww = work.w - s_outline.x;
                if (s_outline.y + hh > work.h) hh = work.h - s_outline.y;
                if (ww != s_outline.w || hh != s_outline.h) s_outline_step++;
                s_outline.w = ww;
                s_outline.h = hh;
                s_drag_moved = true;
                if (dk_prefs()->live_drag) { w->w = ww; w->h = hh; clamp(w); }
            }
        } else if (s_drag == DRAG_CLIENT) {
            if (col != s_down_col || row != s_down_row) s_drag_moved = true;
            client_finger(ev, true);
        } else if (dk_menu_depth()) {
            dk_menu_pointer(ev);
        }
        return true;
    }
    if (ev->kind == LS_IEV_WHEEL) {
        dk_win_t *f = dk_front_win();
        tui_rect c;
        if (f && f->kind == DK_APP && (dk_win_rects(f, NULL, &c), tui_rect_contains(c, col, row))) {
            ls_tui_pointer_set(col-c.x, row-c.y);
            for (int i=0, n=abs(ev->wheel); i<n; i++)
                dk_compat_key(ev->wheel > 0 ? LS_TK_WHEEL_DOWN : LS_TK_WHEEL_UP, 0);
            ls_tui_pointer_set(-1,-1);
        }
        return true;
    }
    if (ev->kind == LS_IEV_PTR_UP) {
        const int drag = s_drag;
        s_drag = DRAG_NONE;
        if (drag == DRAG_ICON) {
            if (s_drag_moved && s_drag_icon >= 0) save_icon(s_drag_icon);
            s_drag_icon = -1;
            s_drag_moved = false;
            return true;
        }
        s_pressed_start = false;
        dk_win_t *w = dk_win(s_drag_win);
        if (drag == DRAG_MOVE && w) {
            const tui_rect work = dk_work();
            if (col <= 0 && s_drag_moved) {                          /* to the left edge: the left half */
                w->x = 0; w->y = 0; w->w = work.w / 2; w->h = work.h;
            } else if (col >= work.w - 1 && s_drag_moved) {
                w->x = work.w - work.w / 2; w->y = 0; w->w = work.w / 2; w->h = work.h;
            } else {
                w->x = s_outline.x;
                w->y = s_outline.y;
            }
            clamp(w);
        } else if (drag == DRAG_SIZE && w) {
            w->w = s_outline.w;
            w->h = s_outline.h;
            clamp(w);
            if (w->snap) dk_compat_forget(w);
        } else if (drag == DRAG_KEY && w) {
            tui_rect o;
            dk_win_rects(w, &o, NULL);
            int kmin, kmax, kclose;
            title_keys(w, o, &kmin, &kmax, &kclose);
            const int at = row != o.y ? 0 : col >= kclose && col <= kclose + 2 ? KEY_CLOSE
                         : kmax >= 0 && col >= kmax && col <= kmax + 2 ? KEY_MAX
                         : col >= kmin && col <= kmin + 2 ? KEY_MIN : 0;
            if (at == s_drag_key) {
                dk_anim_key(w->id, at);
                if (at == KEY_CLOSE) ls_desk_close(w->id);
                else if (at == KEY_MAX) ls_desk_maximize(w->id);
                else if (at == KEY_MIN) ls_desk_minimize(w->id);
            }
        } else if (drag == DRAG_CLIENT && w && s_drag_moved) {
            client_finger(ev, false);               /* the end of a drag inside the app */
        } else if (drag == DRAG_CLIENT && w && !s_drag_moved && ev->button == LS_IBTN_LEFT) {
            tui_rect c;
            dk_win_rects(w, NULL, &c);
            client_finger(ev, false);
            if (w->kind == DK_SHELL) dk_shell_tap(w, c, col, row);
            else if (s_down_was_front) dk_compat_tap(col - c.x, row - c.y);
            else dk_compat_tap_later(w, col - c.x, row - c.y);    /* the click that raised it still clicks */
        } else if (drag == DRAG_NONE && dk_menu_depth()) {
            dk_menu_pointer(ev);
        }
        s_drag_moved = false;
        return true;
    }
    if (ev->kind != LS_IEV_PTR_DOWN) return true;

    /* A press. */
    const int64_t now = esp_timer_get_time();
    const bool dbl = ev->button == LS_IBTN_LEFT &&
                     (ev->clicks >= 2 || (col == s_last_down_col && row == s_last_down_row &&
                                          s_last_down_us >= 0 && now - s_last_down_us < DBL_US));
    s_last_down_us = dbl ? -1 : now;
    s_last_down_col = col;
    s_last_down_row = row;
    s_drag_moved = false;

    /* The taskbar's Start closes its own menu; any other press outside a
       menu closes it and does nothing else. */
    if (row == bar && col < TB_START_W && ev->button == LS_IBTN_LEFT) { s_pressed_start = true; toggle_start(); return true; }
    if (dk_menu_depth()) return dk_menu_pointer(ev);

    if (row == bar && col >= s_tray_x) { tray_click(tray_hit(col), ev->button); return true; }
    if (row == bar) {
        const int id = task_hit(col);
        dk_win_t *w = dk_win(id);
        if (!w) return true;
        if (ev->button == LS_IBTN_RIGHT) { dk_menu_open(M_WINDOW, w->id, col, row); return true; }
        if (w == dk_front_win()) ls_desk_minimize(w->id);
        else ls_desk_raise(w->id);
        return true;
    }

    dk_win_t *w = win_at(col, row);
    if (!w) {
        const int ic = icon_hit(col, row);
        if (ev->button == LS_IBTN_RIGHT) { dk_menu_open(M_DESKTOP, 0, col, row); return true; }
        if (ic >= 0 && dbl) open_app(icon_at(ic, NULL));
        else {
            if (ic != s_icon_sel && ic >= 0) dk_anim_icon(ic);
            s_icon_sel = ic;
            tui_rect r;
            int j = -1;
            if (ic >= 0 && icon_find(ic, &r, &j)) {
                s_drag = DRAG_ICON;
                s_drag_icon = j;
                s_drag_dx = col - r.x;
                s_drag_dy = row - r.y;
                s_down_col = col;
                s_down_row = row;
            }
        }
        return true;
    }
    const bool was_front = w == dk_front_win();
    if (!was_front) ls_desk_raise(w->id);
    tui_rect o, c;
    dk_win_rects(w, &o, &c);
    if (ev->button == LS_IBTN_RIGHT) {
        context_menu_for(w, col, row, row == o.y);
        return true;
    }
    if (ev->button != LS_IBTN_LEFT) return true;
    if (row == o.y) { title_press(w, o, col, dbl); return true; }
    const bool grip = w->state != LS_DESK_WIN_MAX &&
                      ((col == o.x + o.w - 1 && row == o.y + o.h - 1) || (col == o.x + o.w - 2 && row == o.y + o.h - 2));
    if (grip) {
        s_drag = DRAG_SIZE;
        s_drag_win = w->id;
        s_outline = o;
        return true;
    }
    if (tui_rect_contains(c, col, row)) {
        s_drag = DRAG_CLIENT;
        s_drag_win = w->id;
        s_down_col = col;
        s_down_row = row;
        s_down_was_front = was_front;
        if (was_front && w->kind == DK_APP) client_finger(ev, true);
    }
    return true;
}

bool ls_desk_active(void) { return s_on; }

bool ls_desk_input(const ls_input_ev_t *ev)
{
    if (!s_on || !ev) return false;
    if (ls_tui_locked()) return false;
    if (dk_anim_splash_on() && (ev->kind == LS_IEV_KEY_DOWN || ev->kind == LS_IEV_PTR_DOWN)) {
        dk_anim_splash_end();                     /* the welcome gets out of the way */
        return true;
    }
    if (ev->kind == LS_IEV_PTR_MOVE || ev->kind == LS_IEV_PTR_DOWN) hover_at(ev->col, ev->row);
    const dk_win_t *sd = dk_win_shell(DK_SHELL_SHUTDOWN);
    if (sd && ev->kind == LS_IEV_PTR_DOWN) {
        tui_rect o;
        dk_win_rects(sd, &o, NULL);
        if (!tui_rect_contains(o, ev->col, ev->row)) return true;     /* modal: the rest is dimmed */
    }
    const bool small = s_cols < DESK_MIN_COLS || s_rows < DESK_MIN_ROWS;
    dk_win_t *front = dk_front_win();
    if ((s_full || small) && front && front->kind == DK_APP) {
        /* Full screen: everything to the app but the key that leaves it. */
        if (ev->kind == LS_IEV_KEY_DOWN) {
            if (ev->key == LS_TK_F11 || ((ev->mods & LS_IMOD_ALT) && ev->key == LS_TK_ENTER)) {
                if (!small) ls_desk_fullscreen(false);
                return true;
            }
            ls_tui_router_key_direct((ls_tk_t)ev->key, ev->ch);
        } else if (ev->kind == LS_IEV_PTR_DOWN && ev->button == LS_IBTN_LEFT) {
            ls_tui_router_touch_direct(ev->col, ev->row);
        } else if (ev->kind == LS_IEV_WHEEL) {
            ls_tui_pointer_set(ev->col,ev->row);
            for (int i=0, n=abs(ev->wheel); i<n; i++)
                ls_tui_router_key_direct(ev->wheel > 0 ? LS_TK_WHEEL_DOWN : LS_TK_WHEEL_UP, 0);
            ls_tui_pointer_set(-1,-1);
        }
        return true;
    }
    switch (ev->kind) {
    case LS_IEV_KEY_DOWN: return desk_key((ls_tk_t)ev->key, ev->ch, ev->mods);
    case LS_IEV_KEY_UP: return true;
    case LS_IEV_PTR_MOVE: case LS_IEV_PTR_DOWN: case LS_IEV_PTR_UP: case LS_IEV_WHEEL:
        return desk_pointer(ev);
    case LS_IEV_RELEASE_ALL:
        ls_tui_touch_client_clear();
        s_drag = DRAG_NONE;
        s_pressed_start = false;
        return true;
    default:
        return false;
    }
}

/* ----------------------------------------------- today's input, adapted -- */

static bool hook_key(ls_tk_t k, char ch)
{
    if (!s_on || ls_tui_locked()) return ls_tui_router_key_direct(k, ch);
    ls_input_ev_t e = {.source = LS_ISRC_TERM, .col = (int16_t)s_ptr_col, .row = (int16_t)s_ptr_row};
    if (k == LS_TK_WHEEL_UP || k == LS_TK_WHEEL_DOWN) {
        e.kind = LS_IEV_WHEEL;
        e.wheel = k == LS_TK_WHEEL_DOWN ? 1 : -1;
    } else {
        e.kind = LS_IEV_KEY_DOWN;
        e.key = (uint16_t)k;
        e.ch = ch;
    }
    return ls_desk_input(&e);
}

/* A tap from the tap queue: a press and release on one cell. */
static bool hook_touch(int col, int row)
{
    if (!s_on || ls_tui_locked()) return ls_tui_router_touch_direct(col, row);
    ls_input_ev_t e = {.kind = LS_IEV_PTR_DOWN, .source = LS_ISRC_TERM, .button = LS_IBTN_LEFT,
                       .buttons = 1, .clicks = 1, .col = (int16_t)col, .row = (int16_t)row};
    ls_desk_input(&e);
    e.kind = LS_IEV_PTR_UP;
    e.buttons = 0;
    ls_desk_input(&e);
    return true;
}

/* Poll the original grid finger, never the app's translated finger. Feeding
   client coordinates back here subtracts the window origin on every draw.
   A press that moved is a drag; one that did not becomes a tap by itself. */
static void poll_finger(void)
{
    if (!s_attached) return;
    int sc, sr, c, r;
    const bool held = ls_tui_touch_input_held(&sc, &sr, &c, &r);
    ls_input_ev_t e = {.source = LS_ISRC_TERM, .button = LS_IBTN_LEFT, .clicks = 1};
    if (held) {
        if (!s_synth_down && (c != sc || r != sr)) {
            int x0=0,y0=0;
            ls_tui_touch_input_px(NULL,NULL,&x0,&y0,NULL);
            e.px=(int16_t)x0; e.py=(int16_t)y0;
            e.kind = LS_IEV_PTR_DOWN; e.buttons = 1; e.col = (int16_t)sc; e.row = (int16_t)sr;
            ls_desk_input(&e);
            s_synth_down = true;
            s_synth_col = sc; s_synth_row = sr;
        }
        if (s_synth_down) {
            int x=0,y=0;
            ls_tui_touch_input_px(&x,&y,NULL,NULL,NULL);
            e.px=(int16_t)x; e.py=(int16_t)y;
            e.kind = LS_IEV_PTR_MOVE; e.buttons = 1; e.col = (int16_t)c; e.row = (int16_t)r;
            ls_desk_input(&e);
            s_synth_col = c; s_synth_row = r;
            s_synth_px = x; s_synth_py = y;
        }
    } else if (s_synth_down) {
        e.px=(int16_t)s_synth_px; e.py=(int16_t)s_synth_py;
        e.kind = LS_IEV_PTR_UP; e.buttons = 0; e.col = (int16_t)s_synth_col; e.row = (int16_t)s_synth_row;
        ls_desk_input(&e);
        s_synth_down = false;
    }
}

void ls_desk_attach(bool on)
{
    s_attached = on;
    s_synth_down = false;
    if (on) ls_tui_router_set_hooks(hook_key, hook_touch);
    else ls_tui_router_set_hooks(NULL, NULL);
}

void ls_desk_set_enabled(bool on)
{
    if (on == s_on) return;
    s_on = on;
    dk_menu_close_all();
    s_drag = DRAG_NONE;
    s_full = false;
    ls_tui_router_set_globals(!on);
    ls_desk_attach(on);
    /* Apps in windows draw their desk forms: one-row keys, no keypads. */
    ls_tui_set_compact(on);
    if (!on) {
        ls_tui_screen_set_full(false);
        dk_compat_release();
        for (int i = 0; i < LS_DESK_MAX_WIN; i++) if (s_w[i].used) dk_compat_forget(&s_w[i]);
        s_last_front = -1;
    }
    if (on) {
        /* What the owner chose last time, from before the reboot. */
        const dk_prefs_t p = load_prefs();
        dk_prefs_set(&p);
        dk_wall_set(p.wall);
        palette_apply();
        load_icons();
    }
    s_cols = s_rows = 0;
    ls_tui_invalidate();
}

bool ls_desk_enabled(void) { return s_on; }

/* --------------------------------------------------------------- console -- */

/* The console runs on its own task; what changes the desk is done by the
   UI task at its next frame. One request at a time. */
static char s_req[2][24];
static volatile int s_req_n;          /* 0 none, else argc of the request */

static void apply_console(void)
{
    const int n = __atomic_load_n(&s_req_n, __ATOMIC_ACQUIRE);
    if (!n) return;
    const char *verb = s_req[0], *arg = n > 1 ? s_req[1] : "";
    if (!strcmp(verb, "on") || !strcmp(verb, "off")) ls_desk_set_enabled(verb[1] == 'n');
    else if (!strcmp(verb, "theme")) {
        /* Any scheme by the start of its name: bay, classic, synth, vfd... */
        int t = -1;
        for (int i = 0; i < dk_scheme_count() && t < 0; i++)
            if (!strncasecmp(dk_scheme_name(i), arg, strlen(arg))) t = i;
        if (t < 0) printf("desk: no scheme '%s'\n", arg);
        else ls_desk_set_theme(t);
    } else if (!strcmp(verb, "focus")) {
        if (!strcmp(arg, "off")) ls_desk_focus(false);
        else {
            if (!s_on) ls_desk_set_enabled(true);
            if (arg[0] && ls_desk_open_app(arg) < 0) printf("desk: no app '%s'\n", arg);
            else ls_desk_focus(true);
        }
    }
    else if (!strcmp(verb, "open")) {
        if (!s_on) ls_desk_set_enabled(true);
        if (ls_desk_open_app(arg) < 0) printf("desk: no app '%s'\n", arg);
    } else if (!strcmp(verb, "close")) {
        const int id = ls_desk_front();
        if (id >= 0) ls_desk_close(id);
    }
    __atomic_store_n(&s_req_n, 0, __ATOMIC_RELEASE);
}

int ls_desk_console(int argc, char **argv)
{
    if (argc < 2) {
        printf("desk: %s, theme %s, %d window%s, %d PSRAM block%s\n", s_on ? "on" : "off",
               ls_desk_theme_name(ls_desk_theme()), s_nz, s_nz == 1 ? "" : "s",
               dk_compat_blocks(), dk_compat_blocks() == 1 ? "" : "s");
        printf("desk on|off, desk theme <scheme>, desk open <app>, desk focus [app|off], desk close, desk list\n");
        return 0;
    }
    if (!strcmp(argv[1], "on") || !strcmp(argv[1], "off") || !strcmp(argv[1], "close") || !strcmp(argv[1], "focus") ||
        ((!strcmp(argv[1], "theme") || !strcmp(argv[1], "open")) && argc >= 3)) {
        if (__atomic_load_n(&s_req_n, __ATOMIC_ACQUIRE)) { printf("desk: busy, try again\n"); return 1; }
        snprintf(s_req[0], sizeof(s_req[0]), "%s", argv[1]);
        snprintf(s_req[1], sizeof(s_req[1]), "%s", argc >= 3 ? argv[2] : "");
        __atomic_store_n(&s_req_n, argc >= 3 ? 2 : 1, __ATOMIC_RELEASE);
        printf("desk: %s%s%s\n", argv[1], argc >= 3 ? " " : "", argc >= 3 ? argv[2] : "");
        return 0;
    }
    if (!strcmp(argv[1], "cost")) {
        for (int k = 0; k < DC_N; k++) {
            printf("desk cost %-16s avg %6lu us  max %6lu us\n", DC_NAME[k], (unsigned long)s_dc_avg[k], (unsigned long)s_dc_max[k]);
            s_dc_max[k] = 0;
        }
        return 0;
    }
    if (!strcmp(argv[1], "list")) {
        ls_desk_win_info_t wi;
        for (int z = 0; ls_desk_window_at(z, &wi); z++)
            printf("desk: %d %-20s %3d,%-3d %3dx%-3d %s%s%s\n", wi.id, wi.title, wi.x, wi.y, wi.w, wi.h,
                   wi.state == LS_DESK_WIN_MIN ? "min" : wi.state == LS_DESK_WIN_MAX ? "max" : "normal",
                   wi.front ? " front" : "", wi.has_snapshot ? " snapshot" : "");
        return 0;
    }
    printf("desk: unknown '%s'\n", argv[1]);
    return 1;
}
