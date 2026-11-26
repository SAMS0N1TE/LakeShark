/* Popup menus: Start with its cascades, the desktop's, the window menu and
   an app window's own. A submenu opens beside its row and moves up when
   there is no room below; the deepest open menu takes the keys. */

#include "ls_desk_priv.h"
#include "../ls_braille.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "../ls_tui_screen.h"
#include "esp_attr.h"
#include "ls_gps.h"
#include "../../../lakeshark/core/ls_tz.h"
#include <time.h>

extern void ls_gps_get(ls_gps_state_t *out) __attribute__((weak));
extern int  ls_notify_unread(void) __attribute__((weak));
EXT_RAM_BSS_ATTR static ls_gps_state_t s_menu_gps;

#define DK_MENU_MAX 5
EXT_RAM_BSS_ATTR static dk_menu_t s_m[DK_MENU_MAX];   /* PSRAM: 3.6 KB */
static int s_n;
static int s_down_level = -1, s_down_row = -1;
/* Hover: the row each open submenu hangs from, and a switch or an open the
   pointer has asked for by resting on a row (it happens at s_pend_us). */
#define HOVER_OPEN_US   220000
#define HOVER_SWITCH_US 400000
static int s_from[DK_MENU_MAX];
static int s_pend_level = -1, s_pend_row, s_last_col = -1;
static int64_t s_pend_us;

int dk_menu_depth(void) { return s_n; }
int dk_menu_top_id(void) { return s_n ? s_m[s_n - 1].id : 0; }
const dk_menu_t *dk_menu_at(int level) { return level >= 0 && level < s_n ? &s_m[level] : NULL; }
void dk_menu_close_all(void) { s_n = 0; s_down_level = -1; s_pend_level = -1; }

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
        /* Windows 98's: a banner down the left, tall rows (the label, and
           under it what the row is for: the `text` line), so a row is an
           easy thing to hit. */
        m->stripe = true;
        it = add(m, "Programs", MI_SUB, A_SUBMENU, M_PROGRAMS);
        if (it) snprintf(it->text, sizeof(it->text), "Every app, by group");
        it = add(m, "Documents", MI_SUB, A_SUBMENU, M_DOCS);
        if (it) snprintf(it->text, sizeof(it->text), "The card, recordings, notes");
        it = add(m, "Settings", MI_SUB, A_SUBMENU, M_SETTINGS);
        if (it) snprintf(it->text, sizeof(it->text), "Control Panel, display, themes");
        it = add(m, "Find", MI_SUB, A_SUBMENU, M_FIND);
        if (it) snprintf(it->text, sizeof(it->text), "An app, a signal, a place");
        it = add(m, "Help", MI_ITEM, A_ABOUT, 0);
        right(it, "F1");
        if (it) snprintf(it->text, sizeof(it->text), "About this desktop");
        it = add(m, "Run...", MI_ITEM, A_RUN, 0);
        if (it) snprintf(it->text, sizeof(it->text), "Open an app or a console word");
        sep(m);
        it = add(m, "Lock screen", MI_ITEM, A_LOCK, 0);
        if (it) snprintf(it->text, sizeof(it->text), "The board asks for its code");
        /* a browser or a network terminal watching: send it away */
        it = add(m, "Disconnect viewer", MI_ITEM, A_KICK, 0);
        if (it) { it->off = !dk_net_viewer(); snprintf(it->text, sizeof(it->text), it->off ? "No one else is watching" : "Send the other viewer away"); }
        it = add(m, "Shut down...", MI_ITEM, A_SHUTDOWN, 0);
        if (it) snprintf(it->text, sizeof(it->text), "Park, close or leave");
        break;
    case M_WINSIZE: {
        /* a window's size presets; Maximized is the state */
        const dk_win_t *w = dk_win(m->arg);
        for (int p = 0; p <= DK_SIZE_N; p++) {
            it = add(m, dk_size_name(p), MI_RADIO, A_WIN_PRESET, m->arg * 4 + p);
            if (!it || !w) continue;
            it->on = p == DK_SIZE_N ? w->state == LS_DESK_WIN_MAX : w->state == LS_DESK_WIN_NORMAL && w->size == p;
            it->off = w->kind != DK_APP || (p < DK_SIZE_N && p != DK_SIZE_SMALL && !dk_size_fits(p));
            static const char *const CLIENT[DK_SIZE_N] = {"80x24", "100x30", "120x38"};
            right(it, p < DK_SIZE_N ? CLIENT[p] : "");
        }
        break;
    }
    case M_PROGRAMS:
        for (int g = 0; g < 4; g++) add(m, dk_group_name(g), MI_SUB, A_SUBMENU, M_GROUP + 100 * (g + 1));
        add(m, "Development", MI_SUB, A_SUBMENU, M_DEV);
        sep(m);
        add(m, dk_group_name(4), MI_SUB, A_SUBMENU, M_GROUP + 100 * 5);
        break;
    case M_DEV:
        add(m, "LakeShark Studio", MI_ITEM, A_STUDIO, 0);
        add(m, "Home Base", MI_ITEM, A_HOMEBASE, 0);
        open_app_item(m, "Terminal", "terminal");
        open_app_item(m, "Diagnostics", "diag");
        break;
    case M_DOCS:
        /* what is on the card, and what was thrown away */
        add(m, "SD card folder...", MI_ITEM, A_FOLDER, 0);
        open_app_item(m, "Files (SD CARD app)", "files");
        open_app_item(m, "Recordings", "rec");
        open_app_item(m, "Notes", "notes");
        sep(m);
        it = add(m, "Recycle Bin", MI_ITEM, A_BIN, 0);
        if (it) snprintf(it->right, sizeof(it->right), "%d", dk_items_binned(NULL, 0));
        break;
    case M_SETTINGS:
        add(m, "Control Panel", MI_SUB, A_SUBMENU, M_CPL);
        open_app_item(m, "Settings app", "set");
        add(m, "Display...", MI_ITEM, A_DISPLAY, 0);
        add(m, "Taskbar & windows", MI_SUB, A_SUBMENU, M_TASKBAR);
        add(m, "Theme", MI_SUB, A_SUBMENU, M_THEME);
        add(m, "Task Manager", MI_ITEM, A_TASKS, 0);
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
        add(m, "An app...", MI_ITEM, A_RUN, 0);
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
        add(m, "Arrange icons", MI_SUB, A_SUBMENU, M_ARRANGE);
        add(m, "New", MI_SUB, A_SUBMENU, M_NEW);
        add(m, "Gadgets", MI_SUB, A_SUBMENU, M_GADGETS);
        sep(m);
        add(m, "Wallpaper", MI_SUB, A_SUBMENU, M_WALLPAPER);
        add(m, "Theme", MI_SUB, A_SUBMENU, M_THEME);
        sep(m);
        add(m, "Run...", MI_ITEM, A_RUN, 0);
        add(m, "Control Panel", MI_ITEM, A_CPL, -1);
        add(m, "Properties", MI_ITEM, A_DISPLAY, 0);
        add(m, "Leave the desktop", MI_ITEM, A_DESK_OFF, 0);
        break;
    case M_NEW:
        add(m, "Shortcut to an app...", MI_ITEM, A_NEW_APP, 0);
        add(m, "File or folder shortcut...", MI_ITEM, A_NEW_FILE, 0);
        break;
    case M_ARRANGE:
        add(m, "By name", MI_ITEM, A_ARRANGE_HOW, DK_ARR_NAME);
        add(m, "Line up", MI_ITEM, A_ARRANGE_HOW, DK_ARR_LINEUP);
        add(m, "Back to the columns", MI_ITEM, A_ARRANGE, 0);
        break;
    case M_GADGETS: {
        static const struct { const char *name; int bit; } G[] = {
            {"Clock and GPS", DKG_CLOCK}, {"Receiver", DKG_RADIO}, {"Mesh", DKG_MESH}, {"Board", DKG_SYSTEM},
        };
        for (unsigned i = 0; i < sizeof(G) / sizeof(G[0]); i++) {
            it = add(m, G[i].name, MI_CHECK, A_GADGET, G[i].bit);
            if (it) it->on = (dk_shell_gadgets() & G[i].bit) != 0;
        }
        break;
    }
    case M_ICON: {
        const dk_icon_t *ic = dk_items_at(m->arg);
        right(add(m, "Open", MI_ITEM, A_ITEM_OPEN, m->arg), "Enter");
        sep(m);
        right(add(m, "Rename...", MI_ITEM, A_ITEM_RENAME, m->arg), "F2");
        it = add(m, "Delete", MI_ITEM, A_ITEM_DELETE, m->arg);
        right(it, "Del");
        if (it && ic && ic->kind == DKI_SYS && (ic->sys == DKS_BIN || ic->sys == DKS_CPL)) it->off = true;
        sep(m);
        add(m, "Properties", MI_ITEM, A_ITEM_PROPS, m->arg);
        break;
    }
    case M_PIN:
        add(m, "Open", MI_ITEM, A_OPEN_APP, m->arg);
        add(m, "Add to desktop", MI_ITEM, A_PIN_APP, m->arg);
        break;
    case M_TASKBAR: {
        bool any = false;
        ls_desk_win_info_t wi;
        for (int z = 0; ls_desk_window_at(z, &wi); z++) any |= wi.state != LS_DESK_WIN_MIN;
        const bool none = ls_desk_window_count() == 0;
        it = add(m, "Cascade", MI_ITEM, A_CASCADE, 0);
        if (it) it->off = !any;
        it = add(m, "Tile side by side", MI_ITEM, A_TILE, 1);
        if (it) it->off = !any;
        it = add(m, "Tile stacked", MI_ITEM, A_TILE, 2);
        if (it) it->off = !any;
        it = add(m, any || none ? "Show the desktop" : "Bring the windows back", MI_ITEM, A_MIN_ALL, 0);
        if (it) it->off = none;
        sep(m);
        add(m, "Task Manager", MI_ITEM, A_TASKS, 0);
        add(m, "Run...", MI_ITEM, A_RUN, 0);
        add(m, "Control Panel", MI_ITEM, A_CPL, -1);
        break;
    }
    case M_CPL:
        add(m, "Open Control Panel", MI_ITEM, A_CPL, -1);
        if (dk_cpl_count()) sep(m);
        for (int i = 0; i < dk_cpl_count() && m->n < DK_MENU_ITEMS; i++) add(m, dk_cpl_at(i)->name, MI_ITEM, A_CPL, i);
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
        add(m, "Sound settings...", MI_ITEM, A_CPL, DK_CPL_SOUND);
        break;
    }
    case M_CLOCK: {
        const time_t now = time(NULL);
        struct tm g;
        if (!ls_tz_local(now, &g)) memset(&g, 0, sizeof(g));   /* the zone SET picked */
        char zone[12];
        ls_tz_abbr(&g, zone, sizeof(zone));
        static const char *const DAY[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        const int src = dk_sys_time_source();
        if (src > 0 || g.tm_year > 120) {
            info(m, "%s %d %s %d", DAY[g.tm_wday % 7], g.tm_mday, MON[g.tm_mon % 12], 1900 + g.tm_year);
            info(m, "%02d:%02d:%02d %s", g.tm_hour, g.tm_min, g.tm_sec, zone);
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
        add(m, "Date, Time & GPS...", MI_ITEM, A_CPL, DK_CPL_DATE);
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
        add(m, "Radio settings...", MI_ITEM, A_CPL, DK_CPL_RADIOS);
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
        add(m, "Keyboards...", MI_ITEM, A_CPL, DK_CPL_INPUT);
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
    case M_BAR_FILE: {
        const dk_win_t *w = dk_win(m->arg);
        if (w && w->kind == DK_APP && w->app) {
            int ai = -1;
            for (int i = 0; i < ls_app_count(); i++) if (ls_app_at(i) == w->app) ai = i;
            it = add(m, "Add to desktop", MI_ITEM, A_PIN_APP, ai);
            if (it && ai < 0) it->off = true;
            add(m, "Run...", MI_ITEM, A_RUN, 0);
        } else if (w && w->shell == DK_SHELL_FOLDER) {
            it = add(m, "Open", MI_ITEM, A_DLG, DKC_FOLD_OPEN); right(it, "Enter");
            if (it) it->off = !dk_dialogs_can(DKC_FOLD_OPEN);
            it = add(m, "Up one level", MI_ITEM, A_DLG, DKC_FOLD_UP); right(it, "Bksp");
            if (it) it->off = !dk_dialogs_can(DKC_FOLD_UP);
            it = add(m, "Shortcut on the desktop", MI_ITEM, A_DLG, DKC_FOLD_SHORTCUT); right(it, "D");
            right(add(m, "Refresh", MI_ITEM, A_DLG, DKC_FOLD_REFRESH), "F5");
        } else if (w && w->shell == DK_SHELL_BIN) {
            it = add(m, "Restore", MI_ITEM, A_DLG, DKC_BIN_RESTORE); right(it, "Enter");
            if (it) it->off = !dk_dialogs_can(DKC_BIN_RESTORE);
            it = add(m, "Delete for good", MI_ITEM, A_DLG, DKC_BIN_DELETE); right(it, "Del");
            if (it) it->off = !dk_dialogs_can(DKC_BIN_DELETE);
            it = add(m, "Empty the bin", MI_ITEM, A_DLG, DKC_BIN_EMPTY);
            if (it) it->off = !dk_dialogs_can(DKC_BIN_EMPTY);
        } else if (w && w->shell == DK_SHELL_TASKS) {
            it = add(m, "Switch to", MI_ITEM, A_DLG, DKC_TASK_SWITCH); right(it, "Enter");
            if (it) it->off = !dk_dialogs_can(DKC_TASK_SWITCH);
            it = add(m, "End task", MI_ITEM, A_DLG, DKC_TASK_END); right(it, "Del");
            if (it) it->off = !dk_dialogs_can(DKC_TASK_END);
            add(m, "New task (Run...)", MI_ITEM, A_RUN, 0);
        }
        sep(m);
        right(add(m, "Close", MI_ITEM, A_WIN_CLOSE, m->arg), "Ctrl+F4");
        break;
    }
    case M_BAR_VIEW: {
        const dk_win_t *w = dk_win(m->arg);
        if (w && w->kind == DK_APP) {
            right(add(m, "Back", MI_ITEM, A_BACK, m->arg), "Esc");
            right(add(m, "Options...", MI_ITEM, A_OPTIONS, m->arg), "o");
            sep(m);
            right(add(m, "Full screen", MI_ITEM, A_WIN_FULL, m->arg), "F11");
            right(add(m, "Focused mode", MI_ITEM, A_WIN_FOCUS, m->arg), "Ctrl+F11");
            sep(m);
        }
        add(m, "Theme", MI_SUB, A_SUBMENU, M_THEME);
        add(m, "Display properties...", MI_ITEM, A_DISPLAY, 0);
        add(m, "Control Panel", MI_ITEM, A_CPL, -1);
        break;
    }
    case M_BAR_WINDOW: {
        const dk_win_t *w = dk_win(m->arg);
        it = add(m, "Restore", MI_ITEM, A_WIN_RESTORE, m->arg);
        if (it && w && w->state == LS_DESK_WIN_NORMAL) it->off = true;
        it = add(m, "Move", MI_ITEM, A_WIN_MOVE, m->arg);
        if (it && w && w->state == LS_DESK_WIN_MAX) it->off = true;
        it = add(m, "Size", MI_SUB, A_SUBMENU, M_WINSIZE);       /* Small, Medium, Large, Maximized */
        if (it && w && w->kind != DK_APP) it->off = true;
        add(m, "Minimize", MI_ITEM, A_WIN_MIN, m->arg);
        it = add(m, "Maximize", MI_ITEM, A_WIN_MAX, m->arg);
        if (it && w && w->state == LS_DESK_WIN_MAX) it->off = true;
        sep(m);
        add(m, "Cascade", MI_ITEM, A_CASCADE, 0);
        add(m, "Tile side by side", MI_ITEM, A_TILE, 1);
        add(m, "Tile stacked", MI_ITEM, A_TILE, 2);
        add(m, "Show the desktop", MI_ITEM, A_MIN_ALL, 0);
        sep(m);
        add(m, "Task Manager", MI_ITEM, A_TASKS, 0);
        break;
    }
    case M_BAR_HELP:
        add(m, "Keys", MI_ITEM, A_KEYS, m->arg);
        right(add(m, "About LakeShark Desktop", MI_ITEM, A_ABOUT, 0), "F1");
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
            /* by the arrow keys: Enter keeps, Esc puts back */
            add(m, "Move", MI_ITEM, A_WIN_MOVE, m->arg);
        }
        it = add(m, "Size", MI_SUB, A_SUBMENU, M_WINSIZE);       /* Small, Medium, Large, Maximized */
        if (it && w && w->kind != DK_APP) it->off = true;
        add(m, "Minimize", MI_ITEM, A_WIN_MIN, m->arg);
        it = add(m, "Maximize", MI_ITEM, A_WIN_MAX, m->arg);
        if (it && w && w->state == LS_DESK_WIN_MAX) it->off = true;
        it = add(m, "Full screen", MI_ITEM, A_WIN_FULL, m->arg);
        right(it, "F11");
        if (it && w && w->kind != DK_APP) it->off = true;
        it = add(m, "Focused mode", MI_ITEM, A_WIN_FOCUS, m->arg);
        right(it, "Ctrl+F11");
        if (it && w && w->kind != DK_APP) it->off = true;
        if (w && w->kind == DK_APP && w->app) {
            /* this app as an icon on the desktop */
            int ai = -1;
            for (int i = 0; i < ls_app_count(); i++) if (ls_app_at(i) == w->app) ai = i;
            if (ai >= 0) add(m, "Add to desktop", MI_ITEM, A_PIN_APP, ai);
        }
        sep(m);
        right(add(m, "Close", MI_ITEM, A_WIN_CLOSE, m->arg), "Ctrl+F4");
        break;
    }
    default:
        if (m->id >= M_GROUP + 100) {
            const int g = m->id / 100 - 1;
            const ls_app_t *list[DK_MENU_ITEMS];
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

/* Each row gets a letter of its own: its first letter when that is free,
   else the first letter of a later word, else any letter still free. The
   letter is lit, and pressing it runs the row (no second key to confirm). */
static void accels(dk_menu_t *m)
{
    bool used[128] = {0};
    for (int i = 0; i < m->n; i++) m->it[i].acc = 0xFF;
    const int passes = m->id == M_START ? 2 : 3;     /* Start keeps its spare letters for the Run search */
    for (int pass = 0; pass < passes; pass++)
        for (int i = 0; i < m->n; i++) {
            dk_item_t *it = &m->it[i];
            const char *l = it->label;
            if (it->acc != 0xFF || it->kind == MI_SEP || it->kind == MI_INFO || it->kind == MI_SLIDER || !l) continue;
            int cell = 0;
            for (const char *c = l; *c && cell < 0xFF; c++) {
                const unsigned char u = (unsigned char)*c;
                if ((u & 0xC0) == 0x80) continue;                    /* inside a UTF-8 character */
                const bool start = c == l, word = c > l && c[-1] == ' ';
                const bool fits = pass == 0 ? start : pass == 1 ? word : true;
                const char up = (char)(u >= 'a' && u <= 'z' ? u - 32 : u);
                if (fits && u < 128 && ((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9')) && !used[(int)up]) {
                    used[(int)up] = true;
                    it->acc = (uint8_t)cell;
                    break;
                }
                if (pass == 0) break;
                cell++;
            }
        }
}

static char accel_of(const dk_item_t *it)
{
    if (it->acc == 0xFF || !it->label) return 0;
    int cell = 0;
    for (const char *c = it->label; *c; c++) {
        if (((unsigned char)*c & 0xC0) == 0x80) continue;
        if (cell++ == it->acc) return (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
    }
    return 0;
}

/* Rows: the Start menu's are two cells high (the label, and what the row is
   for under it), its separators one; every other menu's are one. */
static bool tall(const dk_menu_t *m) { return m->stripe; }
static int row_h(const dk_menu_t *m, int i) { return tall(m) && m->it[i].kind != MI_SEP ? 2 : 1; }
static int row_y(const dk_menu_t *m, int i)
{
    int y = m->y + 1;
    for (int k = 0; k < i && k < m->n; k++) y += row_h(m, k);
    return y;
}

static void place(dk_menu_t *m, int x, int y)
{
    int lw = 0, rw = 0;
    bool sub = false;
    for (int i = 0; i < m->n; i++) {
        int l = dk_text_len(m->it[i].label);
        const int r = dk_text_len(m->it[i].right);
        if (tall(m) && m->it[i].label != m->it[i].text && dk_text_len(m->it[i].text) > l) l = dk_text_len(m->it[i].text);
        if (l > lw) lw = l;
        if (r > rw) rw = r;
        if (m->it[i].kind == MI_SUB) sub = true;
    }
    /* frame, two of padding with the lamp/check column, label, gap, right, arrow, one, frame */
    m->w = 1 + 3 + lw + 2 + rw + (sub ? 2 : 0) + 1 + 1 + (m->stripe ? 2 : 0);
    if (m->w < 18) m->w = 18;
    if (live_menu(m->id) && m->w < 34) m->w = 34;
    if (tall(m)) m->w += 2;                       /* the Start menu: its icons' column */
    m->h = row_y(m, m->n) - m->y + 1;
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
    accels(m);
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

void dk_menu_open_kbd(int id, int arg, int x, int y)
{
    open_level(0, id, arg, x, y, true);
}

/* The submenu of item `row` of menu `level`, beside it. */
static void open_sub(int level, int row, bool select_first)
{
    const dk_menu_t *p = &s_m[level];
    const dk_item_t *it = &p->it[row];
    if (it->kind != MI_SUB || it->off) return;
    if (s_n > level + 1 && s_m[level + 1].id == it->arg) return;   /* already open */
    /* a window's Size list is that window's: it takes the window menu's arg */
    open_level(level + 1, it->arg, it->arg == M_WINSIZE ? p->arg : 0, p->x + p->w, row_y(p, row) - 1, select_first);
    s_from[level + 1] = row;
    s_pend_level = -1;
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
        /* the banner's name down its foot, as Windows 98 had its own: the
           product in the stripe's bright ink, "Desktop" in its plain one */
        static const char WORD[] = "LAKESHARK DESKTOP";
        const int letters = (int)sizeof(WORD) - 1;
        const int top = m->y + m->h - 1 - letters;
        const uint8_t bright = (uint8_t)((st & 0xF0) | ((st & 0x0F) | TUI_BRIGHT));
        for (int i = 0; i < letters; i++)
            if (top + i > m->y + 2 && WORD[i] != ' ') tui_put_char(sf, all, m->x + 1, top + i, WORD[i], i < 9 ? bright : st);
        ix = m->x + 3;
    }
    dk_frame(sf, all, r, true);
    const int iw = m->x + m->w - 1 - ix;
    for (int i = 0; i < m->n; i++) {
        const dk_item_t *it = &m->it[i];
        const int y = row_y(m, i);
        if (tall(m) && it->kind != MI_SEP) {
            /* A Start row: an icon in its hue, the label (its letter lit),
               and under it what the row is for, two cells high to hit. */
            const bool sel = i == m->sel;
            const uint8_t face = sel ? dk_attr(R_SELECT) : it->off ? MENU_ON(R_DISABLED) : mf;
            const tui_rect band = tui_rect_make(ix, y, iw, 2);
            if (sel) dk_fill(sf, band, face);
            int cp = LS_SHAPE_DOT;
            uint8_t hue = TUI_BLUE;
            switch (it->act == A_SUBMENU ? it->arg : -it->act) {
            case M_PROGRAMS: cp = LS_SHAPE_SQUARE; hue = TUI_BLUE; break;
            case M_DOCS:     cp = LS_BOX_DBL_H;    hue = TUI_YELLOW; break;
            case M_SETTINGS: cp = '*';             hue = TUI_MAGENTA; break;
            case M_FIND:     cp = '?';             hue = TUI_CYAN; break;
            case -A_ABOUT:   cp = 'i';             hue = TUI_GREEN; break;
            case -A_RUN:     cp = '>';             hue = TUI_BLUE; break;
            case -A_LOCK:    cp = LS_SHAPE_SQUARE; hue = TUI_YELLOW; break;
            case -A_KICK:    cp = LS_SHAPE_TIMES;  hue = TUI_RED; break;
            case -A_SHUTDOWN: cp = LS_SHAPE_DOT;   hue = TUI_RED; break;
            default: break;
            }
            const uint8_t ia = sel ? face : it->off ? face : dk_fit((uint8_t)(hue | (dk_light_face() ? 0 : TUI_BRIGHT)), TUI_ATTR_BG(mf), 30);
            if (cp < 0x80) tui_put_char(sf, band, ix + 1, y, (char)cp, ia);
            else ls_box_put(sf, band, ix + 1, y, cp, ia);
            const int lx = ix + 4;
            dk_text(sf, band, lx, y, it->label, face);
            if (!it->off && !sel && accel_of(it)) {
                const int ax = lx + it->acc;
                if (tui_rect_contains(tui_rect_intersect(band, all), ax, y)) sf->back[(size_t)y * sf->w + ax].attr = MENU_ON(R_ACCEL);
            }
            if (it->label != it->text && it->text[0])
                dk_text(sf, band, lx, y + 1, it->text,
                        sel ? face : it->off ? MENU_ON(R_DISABLED) : dk_fit(TUI_ATTR_FG(dk_attr(R_INFO)), TUI_ATTR_BG(mf), 45));
            int rx = ix + iw - 2;
            if (it->kind == MI_SUB) { ls_box_put(sf, band, rx, y, LS_SHAPE_RIGHT, face); rx -= 2; }
            if (it->right[0]) dk_text(sf, band, rx - dk_text_len(it->right) + 1, y, it->right, face);
            continue;
        }
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
        dk_text(sf, row, ix + 3, y, it->label, face);
        if (!it->off && !sel && accel_of(it)) {
            const int ax = ix + 3 + it->acc;
            if (tui_rect_contains(tui_rect_intersect(row, all), ax, y))
                sf->back[(size_t)y * sf->w + ax].attr = MENU_ON(R_ACCEL);
        }
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
    tui_cell *keep = heap_caps_malloc((size_t)r.w * r.h * sizeof(tui_cell), MALLOC_CAP_SPIRAM);   /* not the Nano's scarce internal RAM, each unfold frame */
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

/* A row the pointer rested on long enough: its submenu opens, or the open
   one beside it gives way. */
static void hover_tick(void)
{
    if (s_pend_level < 0) return;
    if (s_pend_level >= s_n) { s_pend_level = -1; return; }
    if (esp_timer_get_time() < s_pend_us) return;
    const int level = s_pend_level, row = s_pend_row;
    s_pend_level = -1;
    if (row < 0 || row >= s_m[level].n || s_m[level].sel != row) return;
    s_n = level + 1;
    if (s_m[level].it[row].kind == MI_SUB) open_sub(level, row, false);
}

void dk_menu_draw(tui_surface *sf)
{
    hover_tick();
    for (int i = 0; i < s_n; i++)
        if (live_menu(s_m[i].id)) {
            const int sel = s_m[i].sel;
            build(&s_m[i]);
            accels(&s_m[i]);
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
    s_pend_level = -1;                        /* the keys lead now, not where the pointer rests */
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
        int hits = 0, first = -1, next = -1;
        for (int i = 0; i < m->n; i++) {
            if (!pickable(&m->it[i])) continue;
            const char c = accel_of(&m->it[i]);
            if (!c || c != want) continue;
            hits++;
            if (first < 0) first = i;
            if (next < 0 && i > m->sel) next = i;
        }
        if (hits == 1) { m->sel = first; activate(level, first); }
        else if (hits > 1) m->sel = next >= 0 ? next : first;      /* Enter then does it */
        else if (s_m[0].id == M_START && ch > 32 && ch < 127) {
            /* no row starts with it: the letter begins a search in Run */
            dk_menu_close_all();
            dk_run_open_with(ch);
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
        int r = -1;
        for (int i = 0; i < m->n; i++) {
            const int y = row_y(m, i);
            if (row >= y && row < y + row_h(m, i)) { r = i; break; }
        }
        /* the Start banner and the frame are the menu's, not a row's */
        const int left = m->x + (m->stripe ? 3 : 1);
        *out_row = r >= 0 && col >= left && col < m->x + m->w - 1 ? r : -1;
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
    case LS_IEV_PTR_MOVE: {
        /* Windows 98's way: a submenu opens after the pointer rests on its
           row a moment, and one that is open stays open while the pointer
           travels toward it, even across other rows; only resting on
           another row a while longer switches. Nothing closes on a move. */
        const int64_t now = esp_timer_get_time();
        const int was_col = s_last_col;
        s_last_col = ev->col;
        if (level < 0) return false;
        for (int k = 0; k < level; k++) s_m[k].sel = s_from[k + 1];     /* the path to here stays lit */
        if (s_pend_level >= 0 && s_pend_level < level) s_pend_level = -1;   /* it got there */
        if (row < 0 || !pickable(&s_m[level].it[row])) return true;
        dk_menu_t *m = &s_m[level];
        const bool deeper = s_n > level + 1;
        m->sel = row;
        if (deeper && row == s_from[level + 1]) { s_pend_level = -1; return true; }   /* back on its own row */
        if (deeper) {
            /* heading for the open submenu: the switch waits while it keeps coming */
            const dk_menu_t *sub = &s_m[level + 1];
            const bool toward = was_col >= 0 && (sub->x > m->x ? ev->col > was_col : ev->col < was_col);
            if (s_pend_level != level || s_pend_row != row || toward) s_pend_us = now + (toward ? 2 : 1) * HOVER_SWITCH_US;
            s_pend_level = level;
            s_pend_row = row;
        } else if (m->it[row].kind == MI_SUB) {
            if (s_pend_level != level || s_pend_row != row) s_pend_us = now + HOVER_OPEN_US;
            s_pend_level = level;
            s_pend_row = row;
        } else s_pend_level = -1;
        return true;
    }
    case LS_IEV_PTR_DOWN:
        if (level < 0) {
            dk_menu_close_all();
            return true;              /* the click that closes a menu does nothing else */
        }
        if (row >= 0 && ev->button == LS_IBTN_RIGHT && s_m[level].it[row].act == A_OPEN_APP &&
            s_m[level].it[row].arg >= 0 && s_m[level].id != M_PIN) {
            const int app = s_m[level].it[row].arg;
            dk_menu_close_all();
            dk_menu_open(M_PIN, app, ev->col, ev->row);
            return true;
        }
        s_down_level = level;
        s_down_row = row;
        s_pend_level = -1;                    /* a press opens a submenu at once */
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
