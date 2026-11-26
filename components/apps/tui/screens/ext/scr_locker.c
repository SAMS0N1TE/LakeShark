/* LOCKER: files between this board and a computer over Wi-Fi, through terminalbay.com. Two
   locker doors fill with what each side has put there (the board's and the website's bytes of
   the 200 MB); LINK shows a 6-digit code big enough to read across a desk, counting down from the
   real clock; the list says which way each file went, its size, age and days left. Uploads come
   from the card through a folder picker; downloads land in /sdcard/inbox. Network: core/ls_hub.h. */
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_deskui.h"
#include "../../ls_drag_scroll.h"
#include "../../ls_picker.h"
#include "../../ls_glyph.h"
#include "../../ls_text.h"
#include "ls_hub.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_timer.h"

#define LOCKER_WEB "terminalbay.com/locker"
#define QUOTA (200u * 1024u * 1024u)
#define TTL_D 14
#define PICK_MAX 40

EXT_RAM_BSS_ATTR static struct {
    int sel, top, rows;
    tui_rect list;
    ls_drag_scroll_t drag;
    int64_t del_until, next_list;
    char del_name[64];
    char dir[160];                          /* the picker's folder */
    char pick[PICK_MAX][64];
    bool pick_dir[PICK_MAX];
    int npick;
    char inbox_have[LS_LOCKER_LIST_MAX];    /* 1: already in /sdcard/inbox at that size */
    char btn_v[2][12];
} L;

#define A(fg) TUI_ATTR((fg), TUI_BLACK)

static int count(void) { return ls_locker_count(); }

static bool picked(ls_locker_file_t *f) { return ls_locker_file(L.sel, f); }

static bool is_lock_op(const char *w)
{
    static const char *const OPS[] = {"pair", "list", "upload", "pull", "unpair", "delete", "get", "locker"};
    for (unsigned i = 0; i < sizeof(OPS) / sizeof(OPS[0]); i++) if (!strcmp(w, OPS[i])) return true;
    return false;
}

static void size_str(char *o, size_t n, uint32_t b)
{
    if (b < 1024) snprintf(o, n, "%lu B", (unsigned long)b);
    else if (b < 1024 * 1024) snprintf(o, n, "%lu KB", (unsigned long)((b + 1023) / 1024));
    else snprintf(o, n, "%lu.%lu MB", (unsigned long)(b >> 20), (unsigned long)((b & 0xFFFFF) * 10 >> 20));
}

static void refresh_inbox(void)
{
    for (int i = 0; i < count() && i < LS_LOCKER_LIST_MAX; i++) {
        ls_locker_file_t f;
        char p[96];
        struct stat st;
        L.inbox_have[i] = 0;
        if (!ls_locker_file(i, &f) || !f.from_web) continue;
        snprintf(p, sizeof(p), "/sdcard/inbox/%s", f.name);
        L.inbox_have[i] = !stat(p, &st) && (uint32_t)st.st_size == f.size;
    }
}

static void enter(void)
{
    if (L.rows < 1) L.rows = 6;
    if (!L.dir[0]) snprintf(L.dir, sizeof(L.dir), "/sdcard");
    if (ls_hub_online()) ls_locker_list();
    L.next_list = esp_timer_get_time() + 15000000;
}

/* --------------------------------------------------- the card picker -- */

static void pick_open(void);

static void pick_done(int i)
{
    if (i < 0 || i >= L.npick) return;
    char p[224];
    if (!strcmp(L.pick[i], "..")) {
        char *s = strrchr(L.dir, '/');
        if (s && s != L.dir && strcmp(L.dir, "/sdcard")) *s = 0;
        pick_open();
        return;
    }
    snprintf(p, sizeof(p), "%s/%s", L.dir, L.pick[i]);
    if (L.pick_dir[i]) {
        snprintf(L.dir, sizeof(L.dir), "%.159s", p);
        pick_open();
    } else ls_locker_upload(p);
}

static void pick_open(void)
{
    char title[48], det[40];
    snprintf(title, sizeof(title), "UPLOAD FROM %.30s", L.dir + (strlen(L.dir) > 7 ? 7 : 0));
    ls_picker_open(strlen(L.dir) > 7 ? title : "UPLOAD FROM THE CARD", pick_done);
    L.npick = 0;
    if (strcmp(L.dir, "/sdcard")) {
        snprintf(L.pick[0], sizeof(L.pick[0]), "..");
        L.pick_dir[0] = true;
        ls_picker_add(".. UP", "the folder above");
        L.npick = 1;
    }
    DIR *d = opendir(L.dir);
    for (int pass = 0; d && pass < 2; pass++) {        /* folders first, then files */
        rewinddir(d);
        struct dirent *e;
        while ((e = readdir(d)) && L.npick < PICK_MAX) {
            if (e->d_name[0] == '.' || strlen(e->d_name) > 63) continue;
            char p[224];
            struct stat st = {0};
            snprintf(p, sizeof(p), "%s/%s", L.dir, e->d_name);
#ifdef DT_DIR
            const bool dir = e->d_type == DT_DIR;       /* a stat per name is a directory scan each on FAT */
            if (dir != (pass == 0) || (!dir && stat(p, &st))) continue;
#else
            if (stat(p, &st)) continue;
            const bool dir = S_ISDIR(st.st_mode);
            if (dir != (pass == 0)) continue;
#endif
            if (dir) snprintf(det, sizeof(det), "folder");
            else {
                size_str(det, sizeof(det), (uint32_t)st.st_size);
                if ((uint32_t)st.st_size > 25u * 1024 * 1024) strncat(det, "  too big", sizeof(det) - strlen(det) - 1);
            }
            snprintf(L.pick[L.npick], sizeof(L.pick[0]), "%s", e->d_name);
            L.pick_dir[L.npick++] = dir;
            ls_picker_add(e->d_name, det);
        }
    }
    if (d) closedir(d);
    ls_picker_empty_reason(d ? "Nothing in this folder" : "No card, or this folder is gone");
}

/* -------------------------------------------------------------- more -- */

static void del_done(int i)
{
    if (i == 0) ls_locker_delete(L.del_name);
}

static void more_done(int i)
{
    ls_locker_file_t f;
    switch (i) {
    case 0: ls_locker_pull(); break;
    case 1:
        if (!picked(&f)) break;
        snprintf(L.del_name, sizeof(L.del_name), "%s", f.name);
        ls_picker_open("DELETE FROM THE LOCKER?", del_done);
        ls_picker_add("DELETE", f.name);
        ls_picker_add("CANCEL", "keep it");
        ls_picker_select(1);
        break;
    case 2: ls_locker_list(); break;
    case 3: ls_locker_unpair(); break;
    default: break;
    }
}

static void more_open(void)
{
    ls_locker_file_t f;
    const bool have = picked(&f);
    ls_picker_open("LOCKER", more_done);
    ls_picker_add("GET ALL NEW", "website files -> /sdcard/inbox");
    ls_picker_add("DELETE", have ? f.name : "pick a file first");
    ls_picker_add("REFRESH", "ask terminalbay.com again");
    ls_picker_add("UNLINK COMPUTERS", "end every website/desktop session");
}

/* ------------------------------------------------------------ action -- */

static int buttons(ls_btn_t *b)
{
    ls_locker_file_t f;
    const bool have = picked(&f), ready = ls_locker_ready() == 1;
    uint32_t left = 0;
    char code[8];
    const bool live = ls_locker_code(code, &left);
    snprintf(L.btn_v[0], sizeof(L.btn_v[0]), "%lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
    int n = 0;
    b[n++] = (ls_btn_t){.label = "U UPLOAD", .value = "CARD", .key = 'u', .dim = !ready};
    b[n++] = (ls_btn_t){.label = "G GET", .value = have && L.sel < LS_LOCKER_LIST_MAX && L.inbox_have[L.sel] ? "AGAIN" : "INBOX",
                        .key = 'g', .dim = !have};
    b[n++] = (ls_btn_t){.label = "L LINK", .value = live ? L.btn_v[0] : "CODE", .key = 'l', .on = live, .badge = ls_locker_ready() == 0};
    b[n++] = (ls_btn_t){.label = "M MORE", .value = "...", .key = 'm'};
    return n;
}

static bool act(char k)
{
    ls_locker_file_t f;
    const bool have = picked(&f);
    switch (k) {
    case 'u': if (ls_locker_ready() == 1) pick_open(); return true;
    case 'g': case '\r': if (have) ls_locker_get(f.name); return true;
    case 'x':
        if (!have) return true;
        if (L.del_until > esp_timer_get_time() && !strcmp(L.del_name, f.name)) { L.del_until = 0; ls_locker_delete(f.name); }
        else { L.del_until = esp_timer_get_time() + 4000000; snprintf(L.del_name, sizeof(L.del_name), "%s", f.name); }
        return true;
    case 'l': ls_locker_pair(); return true;
    case 'm': more_open(); return true;
    case 'a': ls_locker_pull(); return true;
    case 'r': ls_locker_list(); return true;
    default: return false;
    }
}

/* ---------------------------------------------------------------- draw -- */

/* Two doors, the board's and the website's, each filled from the floor by its share of the quota. */
static void draw_doors(tui_surface *sf, tui_rect c, int x0, int y0, uint32_t dev, uint32_t web)
{
    const uint32_t part[2] = {dev, web};
    for (int d = 0; d < 2; d++) {
        const int x = x0 + d * 8;
        const uint8_t hue = d ? TUI_MAGENTA : TUI_CYAN;
        int fill = (int)((uint64_t)part[d] * 28 / QUOTA);         /* 4 rows of 7 */
        if (part[d] && !fill) fill = 1;
        for (int y = 0; y < 7; y++) {
            const bool edge = y == 0 || y == 6;
            tui_put_char(sf, c, x, y0 + y, edge ? (y ? '\'' : '.') : '|', A(TUI_WHITE));
            for (int k = 0; k < 7; k++) {
                char ch = edge ? '-' : ' ';
                uint8_t at = A(TUI_WHITE);
                if (y == 1 && k >= 1 && k <= 5) { ch = '='; at = LS_ATTR_FAINT; }             /* the vents */
                else if (y >= 2 && y <= 5) {
                    const int cell = (5 - y) * 7 + k;
                    if (cell < fill) { ch = (y & 1) ? '#' : '='; at = A(hue | (y & 1 ? TUI_BRIGHT : 0)); }
                    if (y == 3 && k == (d ? 0 : 6)) { ch = 'o'; at = A(TUI_YELLOW | TUI_BRIGHT); }   /* handles */
                }
                tui_put_char(sf, c, x + 1 + k, y0 + y, ch, at);
            }
        }
    }
    tui_put_char(sf, c, x0 + 16, y0, '.', A(TUI_WHITE));
    for (int y = 1; y < 6; y++) tui_put_char(sf, c, x0 + 16, y0 + y, '|', A(TUI_WHITE));
    tui_put_char(sf, c, x0 + 16, y0 + 6, '\'', A(TUI_WHITE));
    tui_put_str(sf, c, x0 + 2, y0 + 7, "BOARD", A(TUI_CYAN));
    tui_put_str(sf, c, x0 + 11, y0 + 7, "WEB", A(TUI_MAGENTA));
}

static void big_digit(tui_surface *sf, tui_rect c, int x, int y, char d, uint8_t at)
{
    const char *const *f = ls_glyph_figure(d);
    if (!f) return;
    for (int r = 0; r < 5; r++)
        for (int k = 0; k < 6; k++) {
            const int col = k / 2;
            if (f[r][col] != 'X') continue;
            const bool hz = (col && f[r][col - 1] == 'X') || (col < 2 && f[r][col + 1] == 'X');
            const bool vt = (r && f[r - 1][col] == 'X') || (r < 4 && f[r + 1][col] == 'X');
            tui_put_char(sf, c, x + k, y + r, hz && vt ? '#' : hz ? '=' : '#', at);
        }
}

/* The code, the place to type it and the time it has left, all from the clock. */
static int draw_code(tui_surface *sf, tui_rect r, const char *code, uint32_t left)
{
    ls_panel_box(sf, r, "LINK A COMPUTER", TUI_YELLOW);
    const tui_rect in = tui_rect_make(r.x + 2, r.y + 1, r.w - 4, r.h - 2);
    tui_put_str(sf, in, in.x, in.y, "On a computer open", LS_ATTR_DIM);
    tui_put_str(sf, in, in.x + 19, in.y, LOCKER_WEB, ls_ink(LS_INK_ACCENT, A(TUI_CYAN | TUI_BRIGHT)));
    tui_put_str(sf, in, in.x, in.y + 1, "and type this code:", LS_ATTR_DIM);
    const int dw = 7, w = 6 * dw + 1, x0 = in.x + (in.w - w) / 2;
    if (ls_tui_compact()) {     /* a desk window: the code on a VFD */
        char v[8];
        snprintf(v, sizeof(v), "%.3s %.3s", code, strlen(code) > 3 ? code + 3 : "");
        ls_dui_vfd(sf, tui_rect_make(in.x, in.y + 2, in.w, 7), v, LS_DUI_VFD_AMBER);
    } else
    for (int i = 0; i < 6 && code[i]; i++)
        big_digit(sf, in, x0 + i * dw + (i >= 3 ? 2 : 0), in.y + 3, code[i], ls_ink(LS_INK_STRONG, A(TUI_YELLOW | TUI_BRIGHT)));
    /* a fuse that burns down with the real seconds left */
    char t[24];
    const int fw = in.w - 16 > 30 ? 30 : in.w - 16;
    snprintf(t, sizeof(t), "good for %lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
    tui_put_str(sf, in, in.x, in.y + 9, t, ls_ink_hue(A(left < 60 ? TUI_YELLOW | TUI_BRIGHT : TUI_GREEN)));
    const int lit = (int)((uint64_t)left * (uint64_t)fw / 600);
    for (int k = 0; k < fw; k++)
        tui_put_char(sf, in, in.x + 15 + k, in.y + 9, k < lit ? '~' : k == lit ? '*' : '.',
                     k < lit ? A(TUI_YELLOW) : k == lit ? A(TUI_RED | TUI_BRIGHT) : LS_ATTR_FAINT);
    return 12;
}

static void draw_head(tui_surface *sf, tui_rect r, uint32_t dev, uint32_t web, int ndev, int nweb)
{
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    const bool art = r.w >= 44;
    if (art) draw_doors(sf, r, r.x + 1, r.y, dev, web);
    const tui_rect in = tui_rect_make(r.x + (art ? 21 : 0), r.y, r.w - (art ? 21 : 0), r.h);
    char t[48], a[16], b[16];
    int y = in.y;
    tui_put_str(sf, in, in.x, y++, "FILE LOCKER", ls_ink(LS_INK_STRONG, A(TUI_WHITE | TUI_BRIGHT)));
    tui_put_str(sf, in, in.x, y++, LOCKER_WEB, ls_ink(LS_INK_ACCENT, A(TUI_CYAN | TUI_BRIGHT)));
    const bool on = ls_hub_online();
    const int rd = ls_locker_ready();
    tui_put_str(sf, in, in.x, y++, !on ? "NO WI-FI" : rd == 1 ? "ONLINE  linked" : rd == 0 ? "ONLINE  not linked" : "ONLINE",
                ls_ink_hue(A(!on ? TUI_YELLOW | TUI_BRIGHT : rd == 1 ? TUI_GREEN | TUI_BRIGHT : TUI_YELLOW)));
    size_str(a, sizeof(a), dev + web);
    snprintf(t, sizeof(t), "%s of 200 MB", a);
    tui_put_str(sf, in, in.x, y, t, A(TUI_WHITE));
    ls_meter_ascii(sf, in, in.x, y + 1, in.w - 1 < 26 ? in.w - 1 : 26, (float)(dev + web) / (float)QUOTA, TUI_CYAN);
    y += 2;
    size_str(a, sizeof(a), dev);
    size_str(b, sizeof(b), web);
    snprintf(t, sizeof(t), "^ %d up %s", ndev, a);
    tui_put_str(sf, in, in.x, y++, t, A(TUI_CYAN));
    snprintf(t, sizeof(t), "v %d down %s", nweb, b);
    tui_put_str(sf, in, in.x, y++, t, A(TUI_MAGENTA));
}

static void draw_list(tui_surface *sf, tui_rect r)
{
    tui_fill(sf, r, ' ', TUI_DEFAULT_ATTR);
    L.list = r;
    L.rows = r.h / 2 > 0 ? r.h / 2 : 1;
    const int n = count();
    ls_drag_scroll(&L.drag, r, 2, &L.top, n, L.rows);
    if (L.sel >= n) L.sel = n - 1;
    if (L.sel < 0) L.sel = 0;
    if (L.sel < L.top) L.top = L.sel;
    if (L.sel >= L.top + L.rows) L.top = L.sel - L.rows + 1;
    if (L.top > n - L.rows) L.top = n - L.rows;
    if (L.top < 0) L.top = 0;
    if (!n) {
        const int rd = ls_locker_ready();
        if (!ls_hub_online()) ls_panel_notice(sf, r, "NO WI-FI", "Connect Wi-Fi in Settings,", "then come back here.");
        else if (rd == 0) ls_panel_notice(sf, r, "NOT LINKED YET", "LINK shows a code to type at", LOCKER_WEB " on a computer.");
        else if (rd < 0) ls_panel_notice(sf, r, "LOCKER", "Asking terminalbay.com...", "");
        else ls_panel_notice(sf, r, "EMPTY", "UPLOAD puts a file from the card here;", "the website and desktop can too.");
        return;
    }
    const int rw = r.w - (n > L.rows ? 2 : 0);
    for (int k = 0; k < L.rows && L.top + k < n; k++) {
        const int i = L.top + k;
        ls_locker_file_t f;
        if (!ls_locker_file(i, &f)) break;
        const bool on = i == L.sel;
        const int y = r.y + k * 2;
        const tui_rect row = tui_rect_make(r.x, y, rw, 1);
        const uint8_t band = ls_ink(LS_INK_SELECT, TUI_ATTR(TUI_BLACK, f.from_web ? TUI_MAGENTA : TUI_CYAN));
        if (on) tui_fill(sf, row, ' ', band);
        tui_put_char(sf, row, r.x, y, f.from_web ? 'v' : '^', on ? band : A((f.from_web ? TUI_MAGENTA : TUI_CYAN) | TUI_BRIGHT));
        tui_put_str(sf, row, r.x + 2, y, f.name, on ? band : A(TUI_WHITE | TUI_BRIGHT));
        char sz[16], age[8], t[48];
        size_str(sz, sizeof(sz), f.size);
        tui_put_str(sf, row, r.x + rw - (int)strlen(sz), y, sz, on ? band : A(TUI_WHITE));
        const tui_rect l2 = tui_rect_make(r.x, y + 1, rw, 1);
        ls_age_str(age, sizeof(age), 1, f.age_s + 1);
        const int dl = TTL_D - (int)(f.age_s / 86400);
        snprintf(t, sizeof(t), "%s  %s ago", f.from_web ? "from web" : "from board", age[0] == ' ' ? age + 1 : age);
        tui_put_str(sf, l2, r.x + 2, y + 1, t, LS_ATTR_DIM);
        const bool have = i < LS_LOCKER_LIST_MAX && L.inbox_have[i];
        if (f.from_web || have) snprintf(t, sizeof(t), "%s", have ? "in inbox" : "NEW");
        else snprintf(t, sizeof(t), "%dd left", dl < 0 ? 0 : dl);
        tui_put_str(sf, l2, r.x + rw - (int)strlen(t), y + 1, t,
                    have ? ls_ink_hue(A(TUI_GREEN)) : f.from_web ? ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT)) : LS_ATTR_FAINT);
    }
    char code[8];
    if (r.h - n * 2 >= 7 && !ls_locker_code(code, NULL) && !(ls_tui_compact() && r.w >= 90)) {   /* room under a short list: how the other end works */
        const int y = r.y + r.h - 4;
        tui_hline(sf, r, r.x + 2, y - 1, r.w - 4, '.', LS_ATTR_FAINT);
        tui_put_str(sf, r, r.x + 2, y, "On a computer: " LOCKER_WEB, A(TUI_CYAN));
        tui_put_str(sf, r, r.x + 2, y + 1, "LINK here, type the code there, drop files", LS_ATTR_DIM);
        tui_put_str(sf, r, r.x + 2, y + 2, "in; GET brings them to /sdcard/inbox.", LS_ATTR_DIM);
    }
    if (n > L.rows) {
        const int h = r.h, y0 = L.top * h / n, y1 = (L.top + L.rows) * h / n;
        for (int y = 0; y < h; y++)
            tui_put_char(sf, r, r.x + r.w - 1, r.y + y, y >= y0 && y <= y1 ? '|' : ':',
                         y >= y0 && y <= y1 ? A(TUI_CYAN | TUI_BRIGHT) : LS_ATTR_FAINT);
    }
}

/* The hub's word on the last job; `frac` gets the share done while bytes move, else -1. */
static const char *message(const ls_hub_status_t *st, int64_t now, char *t, size_t n, uint8_t *at, float *frac)
{
    ls_locker_file_t f;
    *frac = -1.0f;
    if (picked(&f) && L.del_until > now && !strcmp(L.del_name, f.name)) {
        *at = ls_ink_hue(A(TUI_YELLOW | TUI_BRIGHT));
        return ls_tui_compact() ? "Click DELETE again: gone from the locker for good." : "Tap DELETE again: gone from the locker for good.";
    }
    if (is_lock_op(st->what) && st->state == LS_HUB_BUSY && st->total) {
        char a[16], b[16];
        size_str(a, sizeof(a), st->done);
        size_str(b, sizeof(b), st->total);
        snprintf(t, n, "%s %s / %s", !strcmp(st->what, "upload") ? "up" : "down", a, b);
        *at = ls_ink_hue(A(TUI_GREEN | TUI_BRIGHT));
        *frac = (float)st->done / (float)st->total;
        return t;
    }
    if (is_lock_op(st->what) && st->state != LS_HUB_IDLE && !(st->state == LS_HUB_OK && !strcmp(st->what, "list"))) {
        snprintf(t, n, "%s: %s", st->what, st->text);
        *at = ls_ink_hue(A(st->state == LS_HUB_FAIL ? TUI_YELLOW | TUI_BRIGHT : st->state == LS_HUB_OK ? TUI_GREEN | TUI_BRIGHT : TUI_CYAN));
        return t;
    }
    *at = A(TUI_CYAN);
    return "Files stay 14 days. Downloads go to /sdcard/inbox.";
}

/* A desk window: the keys are the one toolbar; the locker's doors and
   numbers and the link code in group boxes; the files in a white well; the
   hub's word in the window's status bar, a meter under the list while
   bytes move. */
static void draw_desk(tui_surface *sf, tui_rect area, const ls_hub_status_t *st, int64_t now, const ls_btn_t *btn, int nb,
                      uint32_t dev, uint32_t web, int ndev, int nweb)
{
    tui_fill(sf, area, ' ', TUI_DEFAULT_ATTR);
    ls_btn_bar_raised_row(sf, tui_rect_make(area.x, area.y, area.w, 1), btn, nb, -1);
    tui_rect body = tui_rect_make(area.x + 1, area.y + 1, area.w - 2, area.h - 1);
    char code[8], t[96];
    uint32_t left = 0;
    const bool linking = ls_locker_code(code, &left);
    const bool wide = body.w >= 96;
    if (body.h >= 22 || linking) {
        const int hh = linking ? 12 : 10;
        const int hw = wide || !linking ? (wide ? body.w / 2 : body.w) : 0;
        if (hw && body.h >= 22) {
            const tui_rect g = ls_dui_group(sf, tui_rect_make(body.x, body.y, hw, hh), "Locker");
            draw_head(sf, tui_rect_make(g.x + 1, g.y, g.w - 1, g.h), dev, web, ndev, nweb);
        }
        if (linking) draw_code(sf, tui_rect_make(hw && body.h >= 22 ? body.x + hw + 1 : body.x, body.y,
                                                 hw && body.h >= 22 ? body.w - hw - 1 : body.w, 12), code, left);
        else if (wide && body.h >= 22) {      /* how the other end works */
            const tui_rect g = ls_dui_group(sf, tui_rect_make(body.x + hw + 1, body.y, body.w - hw - 1, hh), "On a computer");
            tui_put_str(sf, g, g.x + 1, g.y + 1, LOCKER_WEB, ls_ink(LS_INK_ACCENT, A(TUI_CYAN | TUI_BRIGHT)));
            tui_put_str(sf, g, g.x + 1, g.y + 3, "LINK here, type the code there, drop", LS_ATTR_DIM);
            tui_put_str(sf, g, g.x + 1, g.y + 4, "files in; GET brings them to", LS_ATTR_DIM);
            tui_put_str(sf, g, g.x + 1, g.y + 5, "/sdcard/inbox. Files stay 14 days.", LS_ATTR_DIM);
        }
        if (body.h >= 22 || linking) { body.y += hh; body.h -= hh; }
    }
    float frac;
    uint8_t at;
    const char *m = message(st, now, t, sizeof(t), &at, &frac);
    ls_tui_status_set(m, NULL);
    if (frac >= 0.0f && body.h > 4) {
        ls_meter_ascii(sf, body, body.x + 1, body.y + body.h - 1, body.w - 2, frac, TUI_GREEN);
        body.h--;
    }
    if (body.h > 3) draw_list(sf, ls_dui_well(sf, body, NULL));
}

static void draw(tui_surface *sf, tui_rect area)
{
    ls_hub_status_t st;
    ls_hub_status(&st);
    static uint32_t seen;
    if (st.seq != seen) { seen = st.seq; refresh_inbox(); }
    const int64_t now = esp_timer_get_time();
    if (now > L.next_list && st.state != LS_HUB_BUSY && ls_hub_online() && ls_locker_ready() == 1) {
        L.next_list = now + 15000000;           /* while open: what the website put there shows up */
        ls_locker_list();
    }
    uint32_t dev = 0, web = 0;
    int ndev = 0, nweb = 0;
    for (int i = 0; i < count(); i++) {
        ls_locker_file_t f;
        if (!ls_locker_file(i, &f)) continue;
        if (f.from_web) { web += f.size; nweb++; } else { dev += f.size; ndev++; }
    }
    ls_btn_t btn[4];
    const int nb = buttons(btn);
    if (ls_tui_compact()) { draw_desk(sf, area, &st, now, btn, nb, dev, web, ndev, nweb); return; }
    const int bar_h = ls_btn_raised_height(area, nb);
    const tui_rect bar = tui_rect_make(area.x, area.y + area.h - bar_h, area.w, bar_h);
    const tui_rect msg = tui_rect_make(area.x, bar.y - 2, area.w, 2);
    const bool wide = area.w >= 96;
    const int head_h = area.h >= 40 || wide ? 8 : 0;
    tui_rect body = tui_rect_make(area.x, area.y + head_h + (head_h ? 1 : 0), area.w, msg.y - area.y - head_h - (head_h ? 1 : 0));
    if (head_h) draw_head(sf, tui_rect_make(area.x, area.y, wide ? area.w / 2 : area.w, head_h), dev, web, ndev, nweb);
    char code[8];
    uint32_t left = 0;
    if (ls_locker_code(code, &left)) {
        tui_rect cr = wide ? tui_rect_make(area.x + area.w / 2, area.y, area.w - area.w / 2, 12)
                           : tui_rect_make(body.x, body.y, body.w, 12);
        if (cr.h > body.h + (wide ? head_h : 0)) cr.h = body.h;
        draw_code(sf, cr, code, left);
        if (!wide) { body.y += 13; body.h -= 13; }
        else if (head_h < 13) { body.y = area.y + 13; body.h = msg.y - body.y; }
    }
    tui_hline(sf, area, area.x, body.y - 1 >= area.y ? body.y - 1 : area.y, area.w, '-', ls_ink(LS_INK_RULE, LS_ATTR_FAINT));
    if (body.h > 1) draw_list(sf, body);

    /* the hub's word on the last job, with the bytes while one runs */
    tui_fill(sf, msg, ' ', TUI_DEFAULT_ATTR);
    char t[96];
    uint8_t mat;
    float frac;
    const char *m = message(&st, now, t, sizeof(t), &mat, &frac);
    ls_safe_line(sf, msg, msg.y, m, mat);
    if (frac >= 0.0f) ls_meter_ascii(sf, msg, msg.x + 1, msg.y + 1, msg.w - 2, frac, TUI_GREEN);
    ls_btn_bar_raised_row(sf, bar, btn, nb, -1);
}

/* --------------------------------------------------------------- input -- */

static bool key(ls_tk_t k, char ch)
{
    switch (k) {
    case LS_TK_UP:         L.sel--; break;
    case LS_TK_DOWN:       L.sel++; break;
    case LS_TK_WHEEL_UP:   L.sel -= 2; break;
    case LS_TK_WHEEL_DOWN: L.sel += 2; break;
    case LS_TK_PGUP:       L.sel -= L.rows; break;
    case LS_TK_PGDN:       L.sel += L.rows; break;
    case LS_TK_ENTER:      return act('\r');
    case LS_TK_CHAR:       return act((char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch));
    default:               return false;
    }
    if (L.sel >= count()) L.sel = count() - 1;
    if (L.sel < 0) L.sel = 0;
    return true;
}

static bool touch(int col, int row)
{
    if (ls_drag_scroll_tap_is_lift(&L.drag)) return true;
    ls_btn_t btn[4];
    const int nb = buttons(btn);
    const int b = ls_btn_hit(col, row);
    if (b >= 0 && b < nb) return act(btn[b].key);
    if (tui_rect_contains(L.list, col, row)) {
        const int k = L.top + (row - L.list.y) / 2;
        if (k < count()) L.sel = k;
        return true;
    }
    return false;
}

const ls_tui_screen_t ls_scr_locker = {
    .name = "LOCKER", .hint = "up/down pick  u upload  g get  x delete  l link code  a get all new  r refresh  m more",
    .enter = enter, .draw = draw, .key = key, .touch = touch, .diff_switch = true, .wheel = true,
};
