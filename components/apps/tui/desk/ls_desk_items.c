/* What is on the desktop, and the desk's store.

   An item is a shortcut to an app, to a file or a folder on the card, or one
   of the shell's own things (Control Panel, Recycle Bin...). They are made,
   renamed, moved and deleted from the GUI. A deleted one goes to the bin and
   can come back.

   The store is one small record: the items, where each app's window was
   last, which gadgets are on and the double-click time. It is read when the
   desk comes on and written a moment after something changed, through
   dk_sys_store_save (an NVS blob on the board). Everything here is PSRAM or
   flash; nothing is kept in internal RAM beyond a few words. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "../ls_icons.h"

/* ---------------------------------------------------------------- store -- */

#define STORE_MAGIC 0x31534B44u          /* "DKS1" */
#define PLACES 12

typedef struct {
    uint32_t magic;
    uint8_t version, n_icons, n_places, gadgets;
    uint16_t dbl_ms, spare;
} store_hdr_t;

typedef struct {
    char id[12];
    int16_t x, y, w, h;
    uint8_t state, used;
    uint16_t stamp;                      /* the oldest is the one replaced */
} place_t;

EXT_RAM_BSS_ATTR static dk_icon_t s_it[DK_ICON_MAX];
EXT_RAM_BSS_ATTR static place_t s_place[PLACES];
static int s_n;
static uint8_t s_gadgets = DKG_CLOCK | DKG_RADIO;
static uint16_t s_dbl_ms = 400, s_stamp;
static uint32_t s_serial = 1;
static bool s_dirty;
static int64_t s_dirty_us;

uint32_t dk_items_serial(void) { return s_serial; }

static void touch(void)
{
    s_serial++;
    s_dirty = true;
    s_dirty_us = esp_timer_get_time();
    ls_tui_invalidate();
}

void dk_store_flush(void)
{
    if (!s_dirty) return;
    s_dirty = false;
    const size_t cap = sizeof(store_hdr_t) + sizeof(s_it) + sizeof(s_place);
    uint8_t *b = dk_psram(cap);
    if (!b) return;
    store_hdr_t h = {.magic = STORE_MAGIC, .version = 1, .n_icons = (uint8_t)s_n, .gadgets = s_gadgets, .dbl_ms = s_dbl_ms};
    size_t at = sizeof(h);
    memcpy(b + at, s_it, (size_t)s_n * sizeof(dk_icon_t));
    at += (size_t)s_n * sizeof(dk_icon_t);
    for (int i = 0; i < PLACES; i++)
        if (s_place[i].used) { memcpy(b + at, &s_place[i], sizeof(place_t)); at += sizeof(place_t); h.n_places++; }
    memcpy(b, &h, sizeof(h));
    dk_sys_store_save(b, at);
    dk_psram_free(b);
}

/* A drag, a rename or a closed window marks the store; it is written once
   things have been still for two seconds, not on every step. */
void dk_store_tick(void)
{
    if (s_dirty && esp_timer_get_time() - s_dirty_us > 2000000) dk_store_flush();
}

/* The icons a new board starts with, in their columns. */
static const struct { uint8_t kind, sys; const char *target, *label; } STOCK[] = {
    {DKI_APP, 0, "diag", "MY BOARD"}, {DKI_APP, 0, "fm", "FM"}, {DKI_APP, 0, "map", "MAP"},
    {DKI_APP, 0, "p25", "P25"}, {DKI_APP, 0, "adsb", "ADS-B"}, {DKI_APP, 0, "terminal", "TERMINAL"},
    {DKI_APP, 0, "mesh", "MESH CHAT"}, {DKI_APP, 0, "sweep", "SWEEP"}, {DKI_APP, 0, "set", "SETTINGS"},
    {DKI_APP, 0, "files", "SD CARD"}, {DKI_APP, 0, "radios", "RADIOS"}, {DKI_APP, 0, "gps", "GPS"},
    {DKI_SYS, DKS_STUDIO, "", "STUDIO"}, {DKI_SYS, DKS_CPL, "", "CONTROL PANEL"},
    {DKI_SYS, DKS_BIN, "", "RECYCLE BIN"},
};
#define N_STOCK ((int)(sizeof(STOCK) / sizeof(STOCK[0])))

static void defaults(void)
{
    memset(s_it, 0, sizeof(s_it));
    s_n = 0;
    for (int j = 0; j < N_STOCK; j++) {
        dk_icon_t *it = &s_it[s_n++];
        it->kind = STOCK[j].kind;
        it->sys = STOCK[j].sys;
        it->flags = DKI_F_STOCK;
        snprintf(it->target, sizeof(it->target), "%s", STOCK[j].target);
        snprintf(it->label, sizeof(it->label), "%s", STOCK[j].label);
        /* Where the owner had dragged it before there was a store: the old
           settings words, 8 and up, one per icon of the first thirteen. */
        if (j < 13) {
            const uint32_t v = dk_sys_saved_word(8 + j);
            if (v & 0x80000000u) {
                it->flags |= DKI_F_MOVED;
                it->x = (int16_t)(v & 0xFFF);
                it->y = (int16_t)(v >> 12 & 0xFFF);
            }
        }
    }
}

/* The shell's own items are always there, whatever an older store held. */
static void ensure_sys(int sys, const char *label)
{
    for (int i = 0; i < s_n; i++) if (s_it[i].kind == DKI_SYS && s_it[i].sys == sys) return;
    if (s_n >= DK_ICON_MAX) return;
    dk_icon_t *it = &s_it[s_n++];
    memset(it, 0, sizeof(*it));
    it->kind = DKI_SYS;
    it->sys = (uint8_t)sys;
    it->flags = DKI_F_STOCK;
    snprintf(it->label, sizeof(it->label), "%s", label);
}

void dk_items_load(void)
{
    const size_t cap = sizeof(store_hdr_t) + sizeof(s_it) + sizeof(s_place);
    uint8_t *b = dk_psram(cap);
    const size_t got = b ? dk_sys_store_load(b, cap) : 0;
    store_hdr_t h;
    bool ok = false;
    if (got >= sizeof(h)) {
        memcpy(&h, b, sizeof(h));
        ok = h.magic == STORE_MAGIC && h.version == 1 && h.n_icons <= DK_ICON_MAX && h.n_places <= PLACES &&
             got == sizeof(h) + (size_t)h.n_icons * sizeof(dk_icon_t) + (size_t)h.n_places * sizeof(place_t);
    }
    memset(s_place, 0, sizeof(s_place));
    if (ok) {
        memset(s_it, 0, sizeof(s_it));
        s_n = h.n_icons;
        memcpy(s_it, b + sizeof(h), (size_t)s_n * sizeof(dk_icon_t));
        memcpy(s_place, b + sizeof(h) + (size_t)s_n * sizeof(dk_icon_t), (size_t)h.n_places * sizeof(place_t));
        for (int i = 0; i < s_n; i++) {          /* a stored string is never trusted to end */
            s_it[i].target[sizeof(s_it[i].target) - 1] = 0;
            s_it[i].label[sizeof(s_it[i].label) - 1] = 0;
            if (s_it[i].kind < DKI_APP || s_it[i].kind > DKI_SYS) s_it[i].kind = DKI_NONE;
        }
        for (int i = 0; i < PLACES; i++) s_place[i].id[sizeof(s_place[i].id) - 1] = 0;
        s_gadgets = h.gadgets & 15;
        s_dbl_ms = h.dbl_ms >= 200 && h.dbl_ms <= 900 ? h.dbl_ms : 400;
    } else {
        defaults();
        s_gadgets = DKG_CLOCK | DKG_RADIO;
        s_dbl_ms = 400;
    }
    dk_psram_free(b);
    ensure_sys(DKS_CPL, "CONTROL PANEL");
    ensure_sys(DKS_BIN, "RECYCLE BIN");
    s_dirty = false;
    s_serial++;
}

int dk_shell_gadgets(void) { return s_gadgets; }
void dk_shell_set_gadgets(int mask) { if ((mask & 15) != s_gadgets) { s_gadgets = (uint8_t)(mask & 15); touch(); } }
int dk_shell_dbl_ms(void) { return s_dbl_ms; }
void dk_shell_set_dbl_ms(int ms)
{
    ms = ms < 200 ? 200 : ms > 900 ? 900 : ms;
    if (ms != s_dbl_ms) { s_dbl_ms = (uint16_t)ms; touch(); }
}

bool dk_place_get(const char *id, int *x, int *y, int *w, int *h, int *state)
{
    if (!id) return false;
    for (int i = 0; i < PLACES; i++)
        if (s_place[i].used && !strncmp(s_place[i].id, id, sizeof(s_place[i].id) - 1)) {
            *x = s_place[i].x; *y = s_place[i].y; *w = s_place[i].w; *h = s_place[i].h; *state = s_place[i].state;
            return true;
        }
    return false;
}

void dk_place_put(const char *id, int x, int y, int w, int h, int state)
{
    if (!id || !id[0]) return;
    int at = -1, oldest = 0;
    for (int i = 0; i < PLACES; i++) {
        if (s_place[i].used && !strncmp(s_place[i].id, id, sizeof(s_place[i].id) - 1)) { at = i; break; }
        if (!s_place[i].used) { if (at < 0) at = i; continue; }
        if ((uint16_t)(s_stamp - s_place[i].stamp) > (uint16_t)(s_stamp - s_place[oldest].stamp) || !s_place[oldest].used) oldest = i;
    }
    if (at < 0) at = oldest;
    place_t p = {.x = (int16_t)x, .y = (int16_t)y, .w = (int16_t)w, .h = (int16_t)h, .state = (uint8_t)state, .used = 1};
    snprintf(p.id, sizeof(p.id), "%s", id);
    const place_t *o = &s_place[at];
    if (o->used && !strcmp(o->id, p.id) && o->x == p.x && o->y == p.y && o->w == p.w && o->h == p.h && o->state == p.state)
        return;                                   /* as it was: nothing to write */
    p.stamp = ++s_stamp;
    s_place[at] = p;
    s_dirty = true;
    s_dirty_us = esp_timer_get_time();
}

/* ---------------------------------------------------------------- items -- */

static const ls_app_t *item_app(const dk_icon_t *it)
{
    const ls_app_t *a = it->kind == DKI_APP ? ls_app_by_id(it->target) : NULL;
    return a && a->screen ? a : NULL;
}

/* On the desktop: not in the bin, and (an app's) in this build. */
static bool shown(const dk_icon_t *it)
{
    if (it->kind == DKI_NONE || (it->flags & DKI_F_BINNED)) return false;
    return it->kind != DKI_APP || item_app(it) != NULL;
}

int dk_items_count(void)
{
    int n = 0;
    for (int i = 0; i < s_n; i++) n += shown(&s_it[i]);
    return n;
}

dk_icon_t *dk_items_at(int i)
{
    if (i < 0) return NULL;
    for (int j = 0; j < s_n; j++)
        if (shown(&s_it[j]) && i-- == 0) return &s_it[j];
    return NULL;
}

static int per_col(tui_rect work) { return (work.h - 1) / 8 > 0 ? (work.h - 1) / 8 : 1; }

static tui_rect slot_rect(int k)
{
    const tui_rect work = dk_work();
    const int pc = per_col(work);
    return tui_rect_make(1 + (k / pc) * 13, 1 + (k % pc) * 8, DK_ICON_W, DK_ICON_H);
}

static tui_rect moved_rect(const dk_icon_t *it)
{
    const tui_rect work = dk_work();
    int x = it->x, y = it->y;                     /* kept on the work area as the grid changes */
    if (x > work.w - DK_ICON_W) x = work.w - DK_ICON_W;
    if (y > work.h - DK_ICON_H) y = work.h - DK_ICON_H;
    return tui_rect_make(x < 0 ? 0 : x, y < 0 ? 0 : y, DK_ICON_W, DK_ICON_H);
}

/* A slot an icon the user placed sits on (an old saved spot can land on a
   new default one): the icon that owned it moves to a slot past all of them. */
static bool slot_covered(tui_rect s)
{
    for (int j = 0, n = dk_items_count(); j < n; j++) {
        const dk_icon_t *o = dk_items_at(j);
        if (!o || !(o->flags & DKI_F_MOVED)) continue;
        const tui_rect m = moved_rect(o);
        if (m.x < s.x + s.w && s.x < m.x + m.w && m.y < s.y + s.h && s.y < m.y + m.h) return true;
    }
    return false;
}

tui_rect dk_items_rect(int i)
{
    tui_rect r = slot_rect(i);
    const dk_icon_t *it = dk_items_at(i);
    if (it && !(it->flags & DKI_F_MOVED) && slot_covered(r)) {
        const int n = dk_items_count();
        int spill = 0;                            /* earlier icons pushed out take the first spare slots */
        for (int j = 0; j < i; j++) {
            const dk_icon_t *o = dk_items_at(j);
            if (o && !(o->flags & DKI_F_MOVED) && slot_covered(slot_rect(j))) spill++;
        }
        for (int k = n; k < n + 64; k++) {
            const tui_rect s = slot_rect(k);
            if (slot_covered(s)) continue;
            if (spill-- == 0) return s;
        }
        return r;
    }
    if (it && (it->flags & DKI_F_MOVED)) r = moved_rect(it);
    return r;
}

int dk_items_hit(int col, int row)
{
    for (int i = dk_items_count() - 1; i >= 0; i--)
        if (tui_rect_contains(dk_items_rect(i), col, row)) return i;
    return -1;
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path[0] == '/' && !path[1] ? "SD CARD" : path;
}

const char *dk_items_label(const dk_icon_t *it)
{
    if (!it) return "";
    if (it->label[0]) return it->label;
    if (it->kind == DKI_APP) { const ls_app_t *a = item_app(it); return a && a->name ? a->name : it->target; }
    if (it->kind == DKI_FILE || it->kind == DKI_FOLDER) return base_name(it->target);
    switch (it->sys) {
    case DKS_CPL: return "CONTROL PANEL";
    case DKS_BIN: return "RECYCLE BIN";
    case DKS_TASKS: return "TASKS";
    case DKS_STUDIO: return "STUDIO";
    case DKS_CARD: return "SD CARD";
    default: return "?";
    }
}

void dk_items_what(const dk_icon_t *it, char *buf, size_t n)
{
    if (!it) { if (n) buf[0] = 0; return; }
    const ls_app_t *a = item_app(it);
    switch (it->kind) {
    case DKI_APP: snprintf(buf, n, "App: %s", a && a->name ? a->name : it->target); break;
    case DKI_FILE: snprintf(buf, n, "File: %s", it->target); break;
    case DKI_FOLDER: snprintf(buf, n, "Folder: %s", it->target[0] ? it->target : "/"); break;
    default:
        snprintf(buf, n, "%s", it->sys == DKS_CPL ? "Every setting, in one window" : it->sys == DKS_BIN ? "Deleted shortcuts"
                 : it->sys == DKS_TASKS ? "Windows and memory" : it->sys == DKS_STUDIO ? "Write apps on your PC" : "The card");
        break;
    }
}

/* ------------------------------------------------------------- pictures -- */

/* 20 x 18 dots, as the app icons are: a digit or letter is a colour 0-f,
   '*' the item's own colour, '.' clear. Kept to few colours a cell. */
static const char *const PICS[DK_PIC_N][LS_ICON_ROWS * 3] = {
    [DK_PIC_BIN] = {
        "....................", "........7777........", "....777777777777....",
        "....................", "....777777777777....", "....7.8..88..8.7....",
        "....7.8..88..8.7....", "....7.8..88..8.7....", "....7.8..88..8.7....",
        "....7.8..88..8.7....", "....7.8..88..8.7....", "....7.8..88..8.7....",
        "....7.8..88..8.7....", "....7.8..88..8.7....", "....7.8..88..8.7....",
        "....777777777777....", "....................", "....................",
    },
    [DK_PIC_BIN_FULL] = {
        "......ff....bb......", "......ffff..bbbb....", "....ffffff33bbbb....",
        "....ff..3333..bb....", "....777777777777....", "....7.8..88..8.7....",
        "....7.8..88..8.7....", "....7.8..88..8.7....", "....7.8..88..8.7....",
        "....7.8..88..8.7....", "....7.8..88..8.7....", "....7.8..88..8.7....",
        "....7.8..88..8.7....", "....7.8..88..8.7....", "....7.8..88..8.7....",
        "....777777777777....", "....................", "....................",
    },
    [DK_PIC_CPL] = {
        "....................", "..7777777777777777..", "..7..............7..",
        "..7.....aa.......7..", "..7.8888aa888888.7..", "..7.....aa.......7..",
        "..7..............7..", "..7.........bb...7..", "..7.88888888bb88.7..",
        "..7.........bb...7..", "..7..............7..", "..7...ee.........7..",
        "..7.88ee88888888.7..", "..7...ee.........7..", "..7..............7..",
        "..7777777777777777..", "....................", "....................",
    },
    [DK_PIC_DOC] = {
        "....ffffffffff......", "....ff......ffff....", "....ff......ff.ff...",
        "....ff......ffffff..", "....ff..........ff..", "....ff..******..ff..",
        "....ff..........ff..", "....ff..888888..ff..", "....ff..........ff..",
        "....ff..8888....ff..", "....ff..........ff..", "....ff..888888..ff..",
        "....ff..........ff..", "....ff..88......ff..", "....ff..........ff..",
        "....ffffffffffffff..", "....................", "....................",
    },
    [DK_PIC_TASKS] = {
        "....................", "..7777777777777777..", "..7..............7..",
        "..7..........99..7..", "..7..........99..7..", "..7......bb..99..7..",
        "..7......bb..99..7..", "..7..aa..bb..99..7..", "..7..aa..bb..99..7..",
        "..7..aa..bb..99..7..", "..7..aa..bb..99..7..", "..7..............7..",
        "..7777777777777777..", "........7777........", "........7777........",
        "......77777777......", "....................", "....................",
    },
    [DK_PIC_CARD] = {
        "....................", "....cccccccccc......", "....cc33.33.33cc....",
        "....cc33.33.33cccc..", "....cc33.33.33cccc..", "....cccccccccccccc..",
        "....cccccccccccccc..", "....cc..........cc..", "....cc..ffffff..cc..",
        "....cc..........cc..", "....cc..ffff....cc..", "....cc..........cc..",
        "....cccccccccccccc..", "....cccccccccccccc..", "....cccccccccccccc..",
        "....cccccccccccccc..", "....................", "....................",
    },
};

void dk_pic(tui_surface *sf, tui_rect clip, int x, int y, int pic, uint8_t attr)
{
    if (pic < 0 || pic >= DK_PIC_N) return;
    uint8_t px[LS_ICON_ROWS * 3][LS_ICON_COLS * 2];
    for (int r = 0; r < LS_ICON_ROWS * 3; r++)
        for (int c = 0; c < LS_ICON_COLS * 2; c++) {
            const char ch = PICS[pic][r][c];
            px[r][c] = ch == '.' ? 0xFF : ch == '*' ? TUI_ATTR_FG(attr)
                     : (uint8_t)(ch >= 'a' ? ch - 'a' + 10 : ch - '0');
        }
    ls_icon_draw_px(sf, clip, x, y, px, attr);
}

/* A file's colour, by what opens it. */
static uint8_t file_hue(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return TUI_WHITE;
    static const struct { const char *ext; uint8_t hue; } H[] = {
        {".mp3", TUI_MAGENTA}, {".wav", TUI_MAGENTA}, {".flac", TUI_MAGENTA}, {".ogg", TUI_MAGENTA},
        {".txt", TUI_WHITE}, {".log", TUI_WHITE}, {".csv", TUI_GREEN}, {".json", TUI_GREEN},
        {".sub", TUI_YELLOW}, {".ctile", TUI_CYAN}, {".pmtiles", TUI_CYAN}, {".gpx", TUI_CYAN},
        {".bmp", TUI_RED}, {".png", TUI_RED}, {".jpg", TUI_RED}, {".lap", TUI_YELLOW},
    };
    for (unsigned i = 0; i < sizeof(H) / sizeof(H[0]); i++) if (!strcasecmp(e, H[i].ext)) return H[i].hue;
    return TUI_WHITE;
}

void dk_items_draw_one(tui_surface *sf, tui_rect clip, int x, int y, const dk_icon_t *it, uint8_t ground)
{



    uint8_t hue = TUI_WHITE;
    const ls_app_t *a = item_app(it);
    if (it->kind == DKI_APP && a) hue = a->hue & 15;
    else if (it->kind == DKI_FOLDER) hue = TUI_YELLOW;
    else if (it->kind == DKI_FILE) hue = file_hue(it->target);
    else if (it->kind == DKI_SYS) hue = it->sys == DKS_STUDIO ? TUI_YELLOW : TUI_CYAN;
    if (!dk_light_desk() && hue != TUI_WHITE) hue |= TUI_BRIGHT;
    if (dk_light_desk() && (hue & 7) == TUI_CYAN) hue = TUI_BLUE;
    const uint8_t at = TUI_ATTR(hue, ground);
    if (it->kind == DKI_APP && a) ls_icon_draw(sf, clip, x, y, a->icon, at);
    else if (it->kind == DKI_FOLDER) ls_icon_draw(sf, clip, x, y, LS_ICON_FILES, at);
    else if (it->kind == DKI_FILE) dk_pic(sf, clip, x, y, DK_PIC_DOC, at);
    else if (it->sys == DKS_STUDIO) ls_icon_draw(sf, clip, x, y, LS_ICON_TERMINAL, at);
    else if (it->sys == DKS_CPL) dk_pic(sf, clip, x, y, DK_PIC_CPL, at);
    else if (it->sys == DKS_TASKS) dk_pic(sf, clip, x, y, DK_PIC_TASKS, at);
    else if (it->sys == DKS_CARD) dk_pic(sf, clip, x, y, DK_PIC_CARD, at);
    else if (it->sys == DKS_BIN) dk_pic(sf, clip, x, y, dk_items_binned(NULL, 0) ? DK_PIC_BIN_FULL : DK_PIC_BIN, at);
}

void dk_items_draw(tui_surface *sf, tui_rect work, int sel, int hover)
{
    const int n = dk_items_count();
    /* the art first, glass over a picture; then the labels, solid */
    for (int i = 0; i < n; i++) {
        const tui_rect r = dk_items_rect(i);
        dk_items_draw_one(sf, work, r.x + 1, r.y, dk_items_at(i), TUI_ATTR_BG(dk_attr(R_DESK)));
    }
    dk_wall_glass(sf, work);
    for (int i = 0; i < n; i++) {
        const tui_rect r = dk_items_rect(i);
        /* A name wider than the icon breaks at a space onto a second line
           (the row between two icons); one with no space to break at ends
           in a tilde. The tip has all of it either way. */
        char l[2][DK_ICON_W + 2] = {"", ""};
        const char *full = dk_items_label(dk_items_at(i));
        const int flen = (int)strlen(full);
        int cut = -1;
        if (flen > DK_ICON_W)
            for (int k = DK_ICON_W; k > 0 && cut < 0; k--) if (full[k] == ' ') cut = k;
        if (flen <= DK_ICON_W) snprintf(l[0], sizeof(l[0]), "%s", full);
        else if (cut > 0) {
            snprintf(l[0], sizeof(l[0]), "%.*s", cut, full);
            if (flen - cut - 1 > DK_ICON_W) snprintf(l[1], sizeof(l[1]), "%.*s~", DK_ICON_W - 1, full + cut + 1);
            else snprintf(l[1], sizeof(l[1]), "%s", full + cut + 1);
        } else snprintf(l[0], sizeof(l[0]), "%.*s~", DK_ICON_W - 1, full);
        const uint8_t plain = dk_attr(R_DESK_TEXT);
        const bool hov = i == hover && i != sel;
        const uint8_t la = i == sel ? dk_attr(R_SELECT) : hov ? dk_attr(R_SELECT_DIM) : plain;
        for (int line = 0; line < 2 && l[line][0]; line++) {
            const int len = dk_text_len(l[line]), y = r.y + 6 + line;
            const int lx = r.x + (r.w - len) / 2;
            if (dk_prefs()->labels || i == sel || hov)       /* a backing, so any wallpaper reads */
                dk_fill(sf, tui_rect_intersect(work, tui_rect_make(lx - 1, y, len + 2, 1)), hov ? la : plain);
            dk_text(sf, work, lx, y, l[line], hov ? la : plain);
            if (i == sel) {
                /* the selection opens out from the label's middle as it is picked */
                const int k = dk_anim_icon_progress(i), half = (len + 2) * k / 2000 + 1;
                const int mid = lx + len / 2;
                for (int x = lx - 1; x <= lx + len; x++)
                    if (x >= mid - half && x <= mid + half && tui_rect_contains(work, x, y))
                        sf->back[(size_t)y * sf->w + x].attr = la;
            }
        }
    }
}

void dk_items_open(int i)
{
    const dk_icon_t *it = dk_items_at(i);
    if (!it) return;
    switch (it->kind) {
    case DKI_APP: dk_open_app_win(item_app(it)); break;
    case DKI_FOLDER: dk_folder_open(it->target, -1, -1); break;
    case DKI_FILE: {
        char path[96];
        snprintf(path, sizeof(path), "%s%s", dk_sys_card_root(), it->target);
        if (!dk_sys_open_file(path)) dk_msg("Open", it->target, "Nothing on this board opens that file.");
        break;
    }
    default:
        if (it->sys == DKS_CPL) dk_cpl_open(-1);
        else if (it->sys == DKS_BIN) dk_bin_open();
        else if (it->sys == DKS_TASKS) dk_tasks_open();
        else if (it->sys == DKS_STUDIO) dk_open_studio();
        else if (it->sys == DKS_CARD) dk_folder_open(NULL, -1, -1);
        break;
    }
}

int dk_items_add(int kind, int sys, const char *target, const char *label, int x, int y)
{
    /* a place freed by the bin's Delete is used again */
    int at = -1;
    for (int i = 0; i < s_n && at < 0; i++) if (s_it[i].kind == DKI_NONE) at = i;
    if (at < 0) {
        if (s_n >= DK_ICON_MAX) return -1;
        at = s_n++;
    }
    dk_icon_t *it = &s_it[at];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->sys = (uint8_t)sys;
    snprintf(it->target, sizeof(it->target), "%s", target ? target : "");
    snprintf(it->label, sizeof(it->label), "%s", label ? label : "");
    if (x >= 0 && y >= 0) {
        const tui_rect work = dk_work();
        x -= DK_ICON_W / 2;
        y -= DK_ICON_H / 2;
        it->x = (int16_t)(x < 0 ? 0 : x > work.w - DK_ICON_W ? work.w - DK_ICON_W : x);
        it->y = (int16_t)(y < 0 ? 0 : y > work.h - DK_ICON_H ? work.h - DK_ICON_H : y);
        it->flags |= DKI_F_MOVED;
    }
    touch();
    int k = 0;
    for (int i = 0; i < at; i++) k += shown(&s_it[i]);
    return shown(it) ? k : -1;
}

void dk_items_rename(int i, const char *label)
{
    dk_icon_t *it = dk_items_at(i);
    if (!it || !label) return;
    while (*label == ' ') label++;
    snprintf(it->label, sizeof(it->label), "%s", label);
    touch();
}

bool dk_items_delete(int i)
{
    dk_icon_t *it = dk_items_at(i);
    /* the bin and the Control Panel stay: the way back to everything else */
    if (!it || (it->kind == DKI_SYS && (it->sys == DKS_BIN || it->sys == DKS_CPL))) return false;
    it->flags |= DKI_F_BINNED;
    touch();
    return true;
}

void dk_items_place(int i, int x, int y)
{
    dk_icon_t *it = dk_items_at(i);
    if (!it) return;
    it->x = (int16_t)x;
    it->y = (int16_t)y;
    it->flags |= DKI_F_MOVED;
    s_serial++;
}

void dk_items_dropped(int i) { (void)i; touch(); }

void dk_items_arrange(int how)
{
    if (how == DK_ARR_NAME) {
        /* by name, the shell's own last; then back into the columns */
        for (int a = 1; a < s_n; a++)
            for (int b = a; b > 0; b--) {
                const dk_icon_t *p = &s_it[b - 1], *q = &s_it[b];
                const bool ps = p->kind == DKI_SYS, qs = q->kind == DKI_SYS;
                if (ps == qs ? strcasecmp(dk_items_label(p), dk_items_label(q)) <= 0 : !ps) break;
                const dk_icon_t t = s_it[b]; s_it[b] = s_it[b - 1]; s_it[b - 1] = t;
            }
    }
    if (how == DK_ARR_LINEUP) {
        /* each dragged icon to the nearest place of the grid nothing else has */
        const tui_rect work = dk_work();
        const int cols = (work.w - 1) / 13 > 0 ? (work.w - 1) / 13 : 1, rows = per_col(work);
        uint8_t taken[40][16];
        memset(taken, 0, sizeof(taken));
        const int n = dk_items_count();
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n; i++) {
                dk_icon_t *it = dk_items_at(i);
                const bool moved = it->flags & DKI_F_MOVED;
                if (moved != (pass == 1)) continue;        /* those in their columns keep them */
                const tui_rect r = dk_items_rect(i);
                int gx = (r.x + 5) / 13, gy = (r.y + 3) / 8;
                gx = gx < 0 ? 0 : gx >= cols ? cols - 1 : gx;
                gy = gy < 0 ? 0 : gy >= rows ? rows - 1 : gy;
                for (int d = 0; d < cols + rows && gx < 40 && gy < 16 && taken[gx][gy]; d++) {
                    if (++gy >= rows) { gy = 0; if (++gx >= cols) gx = 0; }
                }
                if (gx < 40 && gy < 16) taken[gx][gy] = 1;
                if (moved) { it->x = (int16_t)(1 + gx * 13); it->y = (int16_t)(1 + gy * 8); }
            }
    } else {
        for (int i = 0; i < s_n; i++) s_it[i].flags &= (uint8_t)~DKI_F_MOVED;
    }
    touch();
}

int dk_items_binned(dk_icon_t **out, int cap)
{
    int n = 0;
    for (int i = 0; i < s_n; i++)
        if (s_it[i].kind != DKI_NONE && (s_it[i].flags & DKI_F_BINNED)) {
            if (out && n < cap) out[n] = &s_it[i];
            n++;
        }
    return n;
}

void dk_items_restore(dk_icon_t *it)
{
    if (!it) return;
    it->flags &= (uint8_t)~DKI_F_BINNED;
    touch();
}

void dk_items_purge(dk_icon_t *it)
{
    for (int i = 0; i < s_n; i++)
        if ((!it || it == &s_it[i]) && (s_it[i].flags & DKI_F_BINNED)) {
            memset(&s_it[i], 0, sizeof(s_it[i]));
        }
    while (s_n > 0 && s_it[s_n - 1].kind == DKI_NONE) s_n--;
    touch();
}
