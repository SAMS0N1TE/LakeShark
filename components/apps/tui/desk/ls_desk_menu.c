/* Popup menus: Start with its cascades, the desktop's, the window menu and
   an app window's own. A submenu opens beside its row and moves up when
   there is no room below; the deepest open menu takes the keys. */

#include "ls_desk_priv.h"
#include "../ls_braille.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#include "../ls_tui_screen.h"
#include "esp_attr.h"
#include "ls_gps.h"
#include <time.h>

extern void ls_gps_get(ls_gps_state_t *out) __attribute__((weak));
extern int  ls_notify_unread(void) __attribute__((weak));
EXT_RAM_BSS_ATTR static ls_gps_state_t s_menu_gps;

#define DK_MENU_MAX 5
EXT_RAM_BSS_ATTR static dk_menu_t s_m[DK_MENU_MAX];   /* PSRAM: 3.6 KB */
static int s_n;
static int s_down_level = -1, s_down_row = -1;

int dk_menu_depth(void) { return s_n; }
int dk_menu_top_id(void) { return s_n ? s_m[s_n - 1].id : 0; }
const dk_menu_t *dk_menu_at(int level) { return level >= 0 && level < s_n ? &s_m[level] : NULL; }
void dk_menu_close_all(void) { s_n = 0; s_down_level = -1; }

int ls_desk_menu_depth(void) { return s_n; }
const char *ls_desk_menu_label(int level, int row)
{
    if (level < 0) level = s_n - 1;
    if (level < 0 || level >= s_n) return NULL;
    if (row < 0) row = s_m[level].sel;
    if (row < 0 || row >= s_m[level].n) return NULL;
    return s_m[level].it[row].label;
}

/* ------------------------------------------------------------ building -- */

static dk_item_t *add(dk_menu_t *m, const char *label, int kind, int act, int arg)
{
    if (m->n >= DK_MENU_ITEMS) return NULL;
    dk_item_t *it = &m->it[m->n++];
    memset(it, 0, sizeof(*it));
    it->label = label;
    it->kind = (uint8_t)kind;
    it->act = act;
    it->arg = arg;
    return it;
}
static void sep(dk_menu_t *m) { add(m, "", MI_SEP, A_NONE, 0); }
/* A line built from state: its text lives in the item. */
static dk_item_t *info(dk_menu_t *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static dk_item_t *info(dk_menu_t *m, const char *fmt, ...)
{
    dk_item_t *it = add(m, "", MI_INFO, A_NONE, 0);
    if (!it) return NULL;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(it->text, sizeof(it->text), fmt, ap);
    va_end(ap);
    it->label = it->text;
    return it;
}

/* Menus whose rows are state, rebuilt every frame while open. */
static bool live_menu(int id)
{
    return id == M_VOLUME || id == M_SOUND || id == M_CLOCK || id == M_RX || id == M_MESH ||
           id == M_BT || id == M_GPS;
}
static void right(dk_item_t *it, const char *r) { if (it) snprintf(it->right, sizeof(it->right), "%s", r); }

static int app_index(const ls_app_t *a)
{
    for (int i = 0; i < ls_app_count(); i++) if (ls_app_at(i) == a) return i;
    return -1;
}

static void open_app_item(dk_menu_t *m, const char *label, const char *id)
{
    const ls_app_t *a = ls_app_by_id(id);
    dk_item_t *it = add(m, label, MI_ITEM, A_OPEN_APP, a ? app_index(a) : -1);
    if (it && !a) it->off = true;
}

static void build(dk_menu_t *m)
{
    m->n = 0;
    m->stripe = false;
    dk_item_t *it;
    switch (m->id) {
    case M_START:
        m->stripe = true;
        add(m, "Programs", MI_SUB, A_SUBMENU, M_PROGRAMS);
        add(m, "Documents", MI_SUB, A_SUBMENU, M_DOCS);
        add(m, "Settings", MI_SUB, A_SUBMENU, M_SETTINGS);
        add(m, "Find", MI_SUB, A_SUBMENU, M_FIND);
        right(add(m, "Help", MI_ITEM, A_ABOUT, 0), "F1");
        open_app_item(m, "Run command...", "terminal");
        sep(m);
        add(m, "Lock screen", MI_ITEM, A_LOCK, 0);
        it = add(m, "Disconnect viewer", MI_ITEM, A_NONE, 0);
        if (it) it->off = true;
        add(m, "Shut down...", MI_ITEM, A_SHUTDOWN, 0);
        break;
    case M_PROGRAMS:
        for (int g = 0; g < 4; g++) add(m, dk_group_name(g), MI_SUB, A_SUBMENU, M_GROUP + 100 * (g + 1));
        add(m, "Development", MI_SUB, A_SUBMENU, M_DEV);
        sep(m);
        add(m, dk_group_name(4), MI_SUB, A_SUBMENU, M_GROUP + 100 * 5);
        break;
    case M_DEV:
        add(m, "LakeShark Studio", MI_ITEM, A_STUDIO, 0);
        open_app_item(m, "Terminal", "terminal");
        open_app_item(m, "Diagnostics", "diag");
        break;
    case M_DOCS:
        it = add(m, "(no recent captures)", MI_ITEM, A_NONE, 0);
        if (it) it->off = true;
        break;
    case M_SETTINGS:
        open_app_item(m, "Settings", "set");
        add(m, "Display...", MI_ITEM, A_DISPLAY, 0);
        add(m, "Theme", MI_SUB, A_SUBMENU, M_THEME);
        sep(m);
        add(m, "Leave the desktop", MI_ITEM, A_DESK_OFF, 0);
        break;
    case M_THEME:
        for (int t = 0; t < LS_DESK_THEME_COUNT; t++) {
            it = add(m, ls_desk_theme_name(t), MI_RADIO, A_THEME, t);
            if (it) it->on = ls_desk_theme() == t;
        }
        break;
    case M_FIND:
        open_app_item(m, "A signal (SWEEP)", "sweep");
        open_app_item(m, "A place (MAP)", "map");
        open_app_item(m, "A file", "files");
        break;
    case M_SHUTDOWN:
        add(m, "Park radios", MI_ITEM, A_PARK, 0);
        add(m, "Close all windows", MI_ITEM, A_CLOSE_ALL, 0);
        add(m, "Leave the desktop", MI_ITEM, A_DESK_OFF, 0);
        it = add(m, "Restart board...", MI_ITEM, A_NONE, 0);
        if (it) it->off = true;
        break;
    case M_DESKTOP:
        add(m, "Arrange icons", MI_ITEM, A_ARRANGE, 0);
        add(m, "Wallpaper", MI_SUB, A_SUBMENU, M_WALLPAPER);
        add(m, "Theme", MI_SUB, A_SUBMENU, M_THEME);
        sep(m);
        add(m, "Properties", MI_ITEM, A_DISPLAY, 0);
        add(m, "Leave the desktop", MI_ITEM, A_DESK_OFF, 0);
        break;
    case M_WALLPAPER: {
        const uint32_t cur = dk_wall_current();
        for (int i = 0; i < dk_wall_builtin_count() && m->n < DK_MENU_ITEMS - 1; i++) {
            it = add(m, dk_wall_builtin_name(i), MI_RADIO, A_WALLPAPER, i);
            if (it) it->on = cur == (uint32_t)i;
        }
        const char *names[8];
        uint32_t ids[8];
        const int nf = dk_wall_files(names, ids, 8);
        if (nf) sep(m);
        for (int i = 0; i < nf && m->n < DK_MENU_ITEMS; i++) {
            it = add(m, names[i], MI_RADIO, A_WALLPAPER, (int)ids[i]);
            if (it) it->on = cur == ids[i];
        }
        break;
    }
    case M_VOLUME: {
        const int v = dk_sys_volume();
        if (v < 0) { info(m, "No sound on this board"); break; }
        info(m, "Board volume");
        it = add(m, "", MI_SLIDER, A_VOL_SET, v);
        const bool muted = dk_sys_muted();
        it = add(m, "Sound on", MI_RADIO, A_MUTE, 0);
        if (it) it->on = !muted;
        it = add(m, "Mute", MI_RADIO, A_MUTE, 1);
        if (it) it->on = muted;
        break;
    }
    case M_SOUND: {
        const int v = dk_sys_volume();
        if (v < 0) { info(m, "No sound on this board"); break; }
        const bool muted = dk_sys_muted();
        it = add(m, "Sound on", MI_RADIO, A_MUTE, 0);
        if (it) it->on = !muted;
        it = add(m, "Mute", MI_RADIO, A_MUTE, 1);
        if (it) it->on = muted;
        sep(m);
        right(add(m, "Volume up", MI_ITEM, A_VOL_STEP, 5), "+5");
        right(add(m, "Volume down", MI_ITEM, A_VOL_STEP, -5), "-5");
        sep(m);
        open_app_item(m, "Music player", "music");
        open_app_item(m, "Sound settings", "set");
        break;
    }
    case M_CLOCK: {
        const time_t now = time(NULL);
        struct tm g;
        const struct tm *gp = gmtime(&now);          /* UI task only */
        if (gp) g = *gp; else memset(&g, 0, sizeof(g));
        static const char *const DAY[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const int src = dk_sys_time_source();
        if (src > 0 || g.tm_year > 120) {
            info(m, "%s %d %s %d", DAY[g.tm_wday % 7], g.tm_mday, MON[g.tm_mon % 12], 1900 + g.tm_year);
            info(m, "%02d:%02d:%02d UTC", g.tm_hour, g.tm_min, g.tm_sec);
        } else {
            info(m, "The clock is not set");
        }
        bool gps_time = false;
        if (ls_gps_get) {
            ls_gps_get(&s_menu_gps);
            gps_time = s_menu_gps.alive && s_menu_gps.year >= 2024;
        }
        sep(m);
        if (gps_time) info(m, "Source: GPS, %d satellites", s_menu_gps.sats_used);
        else info(m, "Source: %s", src == 2 ? "network time" : src == 1 ? "board clock (RTC)" : "none yet");
        open_app_item(m, "GPS...", "gps");
        break;
    }
    case M_RX: {
        const char *claimed = ls_tui_radio_claimed();
        if (claimed) {
            info(m, "Receiving: %s", claimed);
            add(m, "Park the receiver", MI_ITEM, A_PARK, 0);
        } else {
            info(m, "Receiver parked");
            add(m, "Resume", MI_ITEM, A_RX_RESUME, 0);
        }
        sep(m);
        open_app_item(m, "Radios...", "radios");
        break;
    }
    case M_MESH: {
        const int unread = ls_notify_unread ? ls_notify_unread() : 0;
        info(m, unread > 0 ? "%d unread" : "No unread messages", unread);
        open_app_item(m, "Mesh chat", "mesh");
        break;
    }
    case M_BT: {
        int up = 0;
        const int bonded = dk_sys_bt(&up);
        info(m, "Keyboards: %d paired, %d connected", bonded < 0 ? 0 : bonded, up);
        open_app_item(m, "Bluetooth devices...", "set");
        break;
    }
    case M_GPS:
        if (ls_gps_get) {
            ls_gps_get(&s_menu_gps);
            if (s_menu_gps.fix) {
                info(m, "Fix: %s, %d satellites", s_menu_gps.sats_used >= 4 ? "3D" : "2D", s_menu_gps.sats_used);
                info(m, "%.5f %c  %.5f %c", s_menu_gps.lat_deg < 0 ? -s_menu_gps.lat_deg : s_menu_gps.lat_deg,
                     s_menu_gps.lat_deg < 0 ? 'S' : 'N',
                     s_menu_gps.lon_deg < 0 ? -s_menu_gps.lon_deg : s_menu_gps.lon_deg, s_menu_gps.lon_deg < 0 ? 'W' : 'E');
            } else {
                info(m, "No fix: %d satellites in view", s_menu_gps.sats_visible);
            }
        }
        open_app_item(m, "GPS...", "gps");
        open_app_item(m, "Map", "map");
        break;
    case M_WINDOW:
    case M_APPCTX: {
        const dk_win_t *w = dk_win(m->arg);
        if (m->id == M_APPCTX) {
            right(add(m, "Back", MI_ITEM, A_BACK, m->arg), "Esc");
            right(add(m, "Options...", MI_ITEM, A_OPTIONS, m->arg), "o");
            sep(m);
        }
        it = add(m, "Restore", MI_ITEM, A_WIN_RESTORE, m->arg);
        if (it && w && w->state == LS_DESK_WIN_NORMAL) it->off = true;
        if (m->id == M_WINDOW) {
            it = add(m, "Move", MI_ITEM, A_NONE, 0);
            if (it) it->off = true;
            it = add(m, "Size", MI_ITEM, A_NONE, 0);
            if (it) it->off = true;
        }
        add(m, "Minimize", MI_ITEM, A_WIN_MIN, m->arg);
        it = add(m, "Maximize", MI_ITEM, A_WIN_MAX, m->arg);
        if (it && w && w->state == LS_DESK_WIN_MAX) it->off = true;
        it = add(m, "Full screen", MI_ITEM, A_WIN_FULL, m->arg);
        right(it, "F11");
        if (it && w && w->kind != DK_APP) it->off = true;
        it = add(m, "Focused mode", MI_ITEM, A_WIN_FOCUS, m->arg);
        right(it, "Ctrl+F11");
        if (it && w && w->kind != DK_APP) it->off = true;
        sep(m);
        right(add(m, "Close", MI_ITEM, A_WIN_CLOSE, m->arg), "Ctrl+F4");
        break;
    }
    default:
        if (m->id >= M_GROUP + 100) {
            const int g = m->id / 100 - 1;
            const ls_app_t *list[LS_TUI_MAX_SCREENS];
            const int n = dk_group_apps(g, list, DK_MENU_ITEMS);
            for (int i = 0; i < n; i++) {
                it = add(m, list[i]->name, MI_ITEM, A_OPEN_APP, app_index(list[i]));
                if (!it) break;
                if (list[i]->live && list[i]->live()) it->lamp = R_GOOD;
                const char *lbl = dk_app_label(list[i]);
                if (lbl) right(it, lbl);
            }
            if (!n && (it = add(m, "(empty)", MI_ITEM, A_NONE, 0))) it->off = true;
        }
        break;
    }
}

static void place(dk_menu_t *m, int x, int y)
{
    int lw = 0, rw = 0;
    bool sub = false;
    for (int i = 0; i < m->n; i++) {
        const int l = dk_text_len(m->it[i].label), r = dk_text_len(m->it[i].right);
        if (l > lw) lw = l;
        if (r > rw) rw = r;
        if (m->it[i].kind == MI_SUB) sub = true;
    }
    /* frame, two of padding with the lamp/check column, label, gap, right, arrow, one, frame */
    m->w = 1 + 3 + lw + 2 + rw + (sub ? 2 : 0) + 1 + 1 + (m->stripe ? 2 : 0);
    if (m->w < 18) m->w = 18;
    if (live_menu(m->id) && m->w < 34) m->w = 34;
    m->h = m->n + 2;
    const tui_rect work = dk_work();
    if (x + m->w > work.x + work.w) x = work.x + work.w - m->w;
    /* down to the row the taskbar's top line is drawn in: a menu meets the bar */
    if (y + m->h > work.y + work.h + 1) y = work.y + work.h + 1 - m->h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    m->x = x;
    m->y = y;
}

static bool pickable(const dk_item_t *it) { return it->kind != MI_SEP && it->kind != MI_INFO && !it->off; }

static int first_sel(const dk_menu_t *m)
{
    for (int i = 0; i < m->n; i++) if (pickable(&m->it[i])) return i;
    return -1;
}

static void open_level(int level, int id, int arg, int x, int y, bool select_first)
{
    if (level >= DK_MENU_MAX) return;
    dk_menu_t *m = &s_m[level];
    memset(m, 0, sizeof(*m));
    m->id = id;
    m->arg = arg;
    build(m);
    place(m, x, y);
    m->up = m->y < y;
    m->t0 = esp_timer_get_time();
    m->sel = select_first ? first_sel(m) : -1;
    s_n = level + 1;
}

void dk_menu_open(int id, int arg, int x, int y)
{
    open_level(0, id, arg, x, y, id == M_START);
}

/* The submenu of item `row` of menu `level`, beside it. */
static void open_sub(int level, int row, bool select_first)
{
    const dk_menu_t *p = &s_m[level];
    const dk_item_t *it = &p->it[row];
    if (it->kind != MI_SUB || it->off) return;
    if (s_n > level + 1 && s_m[level + 1].id == it->arg) return;   /* already open */
    open_level(level + 1, it->arg, 0, p->x + p->w, p->y + row, select_first);
    dk_menu_t *m = &s_m[level + 1];
    if (m->x < p->x + p->w) {                     /* no room on the right: the left */
        int x = p->x - m->w;
        if (x < 0) x = 0;
        m->x = x;
    }
}

/* ------------------------------------------------------------- drawing -- */

/* The slider's track: from 3 cells in, leaving room for its value and arrow. */
static void slider_geom(const dk_menu_t *m, int *mx, int *mw)
{
    const int ix = m->x + 1, iw = m->w - 2;
    *mx = ix + 3;
    *mw = iw - 3 - 8;
    if (*mw < 6) *mw = 6;
}

static void draw_one(tui_surface *sf, const dk_menu_t *m)
{
    const tui_rect all = tui_surface_rect(sf);
    const tui_rect r = tui_rect_make(m->x, m->y, m->w, m->h);
    /* One solid panel: the face to the frame's outer edge, and the Start
       stripe to it too, so the stripe, the items and the frame touch. */
    const uint8_t mf = dk_attr(R_MENU);
    #define MENU_ON(role) ((uint8_t)((mf & 0xF0) | (dk_attr(role) & 0x0F)))
    dk_fill(sf, r, mf);
    int ix = m->x + 1;
    if (m->stripe) {
        const uint8_t st = dk_attr(R_STRIPE);
        dk_fill(sf, tui_rect_make(m->x, m->y, 3, m->h), st);
        /* a rain of dots thickening to the foot, the stripe's own gradient */
        static const uint16_t RAIN[7] = {0x2801, 0x2805, 0x2815, 0x2855, 0x28D5, 0x28F5, 0x28FF};
        const uint8_t rain = (uint8_t)((st & 0xF0) | ((st & 0x0F) ^ TUI_BRIGHT));
        for (int yy = m->y + 3; yy < m->y + m->h - 1; yy++) {
            const int k = (yy - m->y - 3) * 7 / (m->h - 3 > 0 ? m->h - 3 : 1);
            ls_braille_put(sf, all, m->x, yy, (int16_t)RAIN[k > 6 ? 6 : k], rain);
        }
        ls_box_put(sf, all, m->x + 1, m->y + 1, LS_BOX_DIAG_R, st);
        ls_box_put(sf, all, m->x + 2, m->y + 1, LS_BOX_V, st);
        static const char WORD[] = "LAKESHARK";
        const int letters = (int)sizeof(WORD) - 1;
        const int top = m->y + m->h - 1 - letters;
        for (int i = 0; i < letters; i++)
            if (top + i > m->y + 1) tui_put_char(sf, all, m->x + 1, top + i, WORD[i], st);
        ix = m->x + 3;
    }
    dk_frame(sf, all, r, true);
    const int iw = m->x + m->w - 1 - ix;
    for (int i = 0; i < m->n; i++) {
        const dk_item_t *it = &m->it[i];
        const int y = m->y + 1 + i;
        if (it->kind == MI_SEP) {
            const uint8_t sh = MENU_ON(R_SHADOW);
            /* Across the face only: it meets the frame's edge lines. */
            dk_hline(sf, all, ix, y, iw, LS_BOX_H, sh);
            continue;
        }
        if (it->kind == MI_INFO) {
            dk_text(sf, tui_rect_make(ix, y, iw, 1), ix + 3, y, it->label, MENU_ON(R_STRONG));
            continue;
        }
        if (it->kind == MI_SLIDER) {
            /* <  [=========-------]  30%  > : the arrows step, the track sets */
            int mx, mw;
            slider_geom(m, &mx, &mw);
            const bool sel = i == m->sel;
            const uint8_t face = sel ? dk_attr(R_SELECT) : mf;
            const tui_rect row = tui_rect_make(ix, y, iw, 1);
            if (sel) dk_fill(sf, row, face);
            ls_box_put(sf, row, mx - 2, y, LS_SHAPE_LEFT, face);
            dk_meter(sf, row, mx, y, mw, it->arg * 10, TUI_GREEN);
            char v[8];
            snprintf(v, sizeof(v), "%3d%%", it->arg);
            dk_text(sf, row, mx + mw + 1, y, v, sel ? face : MENU_ON(R_STRONG));
            ls_box_put(sf, row, mx + mw + 6, y, LS_SHAPE_RIGHT, face);
            continue;
        }
        const bool sel = i == m->sel;
        const uint8_t face = sel ? dk_attr(R_SELECT) : it->off ? MENU_ON(R_DISABLED) : mf;
        if (sel) dk_fill(sf, tui_rect_make(ix, y, iw, 1), face);
        const tui_rect row = tui_rect_make(ix, y, iw, 1);
        if (it->kind == MI_CHECK && it->on) ls_box_put(sf, row, ix + 1, y, LS_SHAPE_CHECK, face);
        else if (it->kind == MI_RADIO && it->on) ls_box_put(sf, row, ix + 1, y, LS_SHAPE_DOT, face);
        else if (it->lamp)
            ls_box_put(sf, row, ix + 1, y, LS_SHAPE_DOT,
                       sel ? face : (uint8_t)((face & 0xF0) | (dk_attr(it->lamp) & 0x0F)));
        if (it->off || sel) dk_text(sf, row, ix + 3, y, it->label, face);
        else dk_text_accel(sf, row, ix + 3, y, it->label, face, MENU_ON(R_ACCEL));
        int rx = ix + iw - 1;
        if (it->kind == MI_SUB) { ls_box_put(sf, row, rx, y, LS_SHAPE_RIGHT, face); rx -= 2; }
        if (it->right[0]) {
            const int n = dk_text_len(it->right);
            dk_text(sf, row, rx - n + 1, y, it->right, face);
        }
    }
}

/* A menu unfolds as it opens, a row at a time from where it was pulled
   (down from a menu bar, up from the taskbar), over about a tenth of a
   second; the rows not reached yet keep what was under them. */
#define UNFOLD_US 110000
static void draw_unfolding(tui_surface *sf, const dk_menu_t *m)
{
    const int64_t t = esp_timer_get_time() - m->t0;
    if (!dk_prefs()->anim || t >= UNFOLD_US || t < 0 || m->h < 3) { draw_one(sf, m); return; }
    const int shown = 2 + (int)((m->h - 2) * t / UNFOLD_US);
    const tui_rect r = tui_rect_intersect(tui_surface_rect(sf), tui_rect_make(m->x, m->y, m->w, m->h));
    if (r.w <= 0 || r.h <= 0) return;
    tui_cell *keep = malloc((size_t)r.w * r.h * sizeof(tui_cell));
    if (!keep) { draw_one(sf, m); return; }
    for (int y = 0; y < r.h; y++) memcpy(&keep[(size_t)y * r.w], &sf->back[(size_t)(r.y + y) * sf->w + r.x], (size_t)r.w * sizeof(tui_cell));
    draw_one(sf, m);
    for (int y = 0; y < r.h; y++) {
        const int my = r.y + y - m->y;              /* row within the menu */
        const bool hidden = m->up ? my < m->h - shown : my >= shown;
        if (hidden) memcpy(&sf->back[(size_t)(r.y + y) * sf->w + r.x], &keep[(size_t)y * r.w], (size_t)r.w * sizeof(tui_cell));
    }
    /* the leading edge, a raised line where the fold is */
    const int edge = m->up ? m->y + m->h - shown : m->y + shown - 1;
    for (int x = r.x; x < r.x + r.w; x++)
        ls_box_put(sf, tui_surface_rect(sf), x, edge, m->up ? LS_EDGE_T : LS_EDGE_B, dk_attr(R_SHADOW));
    free(keep);
}

void dk_menu_draw(tui_surface *sf)
{
    for (int i = 0; i < s_n; i++)
        if (live_menu(s_m[i].id)) {
            const int sel = s_m[i].sel;
            build(&s_m[i]);
            if (sel < s_m[i].n) s_m[i].sel = sel;
        }
    for (int i = 0; i < s_n; i++) draw_unfolding(sf, &s_m[i]);
}

/* --------------------------------------------------------------- input -- */

static void activate(int level, int row)
{
    const dk_item_t *it = &s_m[level].it[row];
    if (it->kind == MI_SEP || it->off) return;
    if (it->kind == MI_SUB) { open_sub(level, row, true); return; }
    const int act = it->act, arg = it->arg;
    dk_menu_close_all();
    dk_do(act, arg);
}

static void step(dk_menu_t *m, int d)
{
    if (!m->n) return;
    int s = m->sel;
    for (int k = 0; k < m->n; k++) {
        s = s < 0 ? (d > 0 ? 0 : m->n - 1) : (s + d + m->n) % m->n;
        if (pickable(&m->it[s])) { m->sel = s; return; }
    }
}

bool dk_menu_key(ls_tk_t k, char ch)
{
    if (!s_n) return false;
    const int level = s_n - 1;
    dk_menu_t *m = &s_m[level];
    switch (k) {
    case LS_TK_UP: step(m, -1); return true;
    case LS_TK_HOME: m->sel = -1; step(m, 1); return true;
    case LS_TK_END: m->sel = -1; step(m, -1); return true;
    case LS_TK_DOWN: case LS_TK_TAB: step(m, 1); return true;
    case LS_TK_RIGHT:
        if (m->sel >= 0 && m->it[m->sel].kind == MI_SLIDER) { dk_do(A_VOL_STEP, 5); return true; }
        if (m->sel >= 0 && m->it[m->sel].kind == MI_SUB) open_sub(level, m->sel, true);
        return true;
    case LS_TK_LEFT:
        if (m->sel >= 0 && m->it[m->sel].kind == MI_SLIDER) { dk_do(A_VOL_STEP, -5); return true; }
        if (s_n > 1) s_n--;
        return true;
    case LS_TK_ESC:
        s_n--;
        return true;
    case LS_TK_ENTER:
        if (m->sel >= 0) activate(level, m->sel);
        return true;
    case LS_TK_CHAR: {
        const char want = (char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        for (int i = 0; i < m->n; i++) {
            const char *l = m->it[i].label;
            if (m->it[i].kind == MI_SEP || m->it[i].off || !l || !l[0]) continue;
            const char c = (char)(l[0] >= 'a' && l[0] <= 'z' ? l[0] - 32 : l[0]);
            if (c == want) { m->sel = i; activate(level, i); return true; }
        }
        return true;
    }
    default:
        return true;                  /* a menu up swallows the rest */
    }
}

/* The menu level and row under a cell; row -1 on a menu's frame. */
static int hit(int col, int row, int *out_row)
{
    for (int l = s_n - 1; l >= 0; l--) {
        const dk_menu_t *m = &s_m[l];
        if (!tui_rect_contains(tui_rect_make(m->x, m->y, m->w, m->h), col, row)) continue;
        const int r = row - m->y - 1;
        *out_row = r >= 0 && r < m->n && col > m->x && col < m->x + m->w - 1 ? r : -1;
        return l;
    }
    return -1;
}

bool dk_menu_pointer(const ls_input_ev_t *ev)
{
    if (!s_n) return false;
    int row = -1;
    const int level = hit(ev->col, ev->row, &row);
    switch (ev->kind) {
    case LS_IEV_PTR_MOVE:
        if (level < 0) return false;
        if (row >= 0 && pickable(&s_m[level].it[row])) {
            s_m[level].sel = row;
            if (s_n > level + 1 && !(s_m[level].it[row].kind == MI_SUB && s_m[level + 1].id == s_m[level].it[row].arg))
                s_n = level + 1;
            if (s_m[level].it[row].kind == MI_SUB) open_sub(level, row, false);
        }
        return true;
    case LS_IEV_PTR_DOWN:
        if (level < 0) {
            dk_menu_close_all();
            return true;              /* the click that closes a menu does nothing else */
        }
        s_down_level = level;
        s_down_row = row;
        if (row >= 0) {
            s_m[level].sel = row;
            if (s_n > level + 1) s_n = level + 1;
            if (s_m[level].it[row].kind == MI_SUB) open_sub(level, row, false);
        }
        return true;
    case LS_IEV_PTR_UP:
        if (level >= 0 && row >= 0 && s_m[level].it[row].kind == MI_SLIDER) {
            int mx, mw;
            slider_geom(&s_m[level], &mx, &mw);
            if (ev->col <= mx - 1) dk_do(A_VOL_STEP, -5);
            else if (ev->col >= mx + mw) dk_do(A_VOL_STEP, 5);
            else dk_do(A_VOL_SET, (ev->col - mx) * 100 / (mw - 1));
        } else if (level >= 0 && row >= 0 && level == s_down_level && row == s_down_row &&
            ev->button == LS_IBTN_LEFT && s_m[level].it[row].kind != MI_SUB)
            activate(level, row);
        s_down_level = -1;
        return true;
    case LS_IEV_WHEEL:
        return level >= 0;
    default:
        return false;
    }
}
