/* See ls_deskui.h. */
#include "ls_deskui.h"

#if LS_TUI_HAS_COMPACT

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "ls_box.h"

/* ------------------------------------------------------ the right button -- */

static bool (*s_context)(int col, int row);
static int s_context_screen = -1;

void ls_dui_context_claim(bool (*fn)(int col, int row))
{
    s_context = fn;
    s_context_screen = ls_tui_screen_current();
}

bool ls_dui_context_fire(int col, int row)
{
    if (!s_context || s_context_screen != ls_tui_screen_current()) return false;
    return s_context(col, row);
}

/* ------------------------------------------------------------ text field -- */

void ls_dui_edit_set(ls_dui_edit_t *e, const char *text, int max)
{
    if (max < 1 || max > LS_DUI_TEXT - 1) max = LS_DUI_TEXT - 1;
    e->max = (uint8_t)max;
    size_t n = text ? strlen(text) : 0;
    if (n > (size_t)max) n = (size_t)max;
    if (n) memcpy(e->text, text, n);
    e->text[n] = 0;
    e->len = e->caret = (uint8_t)n;
}

bool ls_dui_edit_key(ls_dui_edit_t *e, ls_tk_t k, char ch)
{
    switch (k) {
    case LS_TK_CHAR:
        if ((unsigned char)ch < 32 || (unsigned char)ch > 126) return false;
        if (e->len >= e->max) return true;                    /* full: taken, and nothing happens */
        memmove(e->text + e->caret + 1, e->text + e->caret, (size_t)(e->len - e->caret) + 1);
        e->text[e->caret++] = ch;
        e->len++;
        return true;
    case LS_TK_BACKSPACE:
        if (!e->caret) return true;
        memmove(e->text + e->caret - 1, e->text + e->caret, (size_t)(e->len - e->caret) + 1);
        e->caret--;
        e->len--;
        return true;
    case LS_TK_DELETE:
        if (e->caret >= e->len) return true;
        memmove(e->text + e->caret, e->text + e->caret + 1, (size_t)(e->len - e->caret));
        e->len--;
        return true;
    case LS_TK_LEFT: if (e->caret) e->caret--; return true;
    case LS_TK_RIGHT: if (e->caret < e->len) e->caret++; return true;
    case LS_TK_HOME: e->caret = 0; return true;
    case LS_TK_END: e->caret = e->len; return true;
    default: return false;
    }
}

/* The first character shown: the caret is always inside the field. */
static int edit_off(const ls_dui_edit_t *e, int room, bool focus)
{
    if (!focus || room < 1) return 0;
    return e->caret >= room ? e->caret - room + 1 : 0;
}

void ls_dui_edit_draw(tui_surface *sf, tui_rect clip, int x, int y, int w,
                      const ls_dui_edit_t *e, bool focus, uint8_t ink)
{
    if (!sf || w < 3) return;
    const int room = w - 2, off = edit_off(e, room, focus);
    const uint8_t well = TUI_ATTR(ink, TUI_BLACK);
    tui_put_char(sf, clip, x, y, '[', focus ? TUI_ATTR(ink | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_FAINT);
    tui_put_char(sf, clip, x + w - 1, y, ']', focus ? TUI_ATTR(ink | TUI_BRIGHT, TUI_BLACK) : LS_ATTR_FAINT);
    for (int i = 0; i < room; i++) {
        const int k = off + i;
        const char c = k < e->len ? e->text[k] : ' ';
        /* The caret is an inverse cell, steady: nothing blinks on a timer. */
        tui_put_char(sf, clip, x + 1 + i, y, c, focus && k == e->caret ? TUI_ATTR(TUI_BLACK, ink & 0x07) : well);
    }
}

void ls_dui_edit_click(ls_dui_edit_t *e, int x, int w, int col)
{
    int k = edit_off(e, w - 2, true) + col - (x + 1);
    if (k < 0) k = 0;
    if (k > e->len) k = e->len;
    e->caret = (uint8_t)k;
}

/* ------------------------------------------------------------- clipboard -- */

static EXT_RAM_BSS_ATTR char s_clip[LS_DUI_TEXT];

void ls_dui_clip_set(const char *text) { snprintf(s_clip, sizeof(s_clip), "%s", text ? text : ""); }
const char *ls_dui_clip_get(void) { return s_clip; }

bool ls_dui_edit_paste(ls_dui_edit_t *e)
{
    bool any = false;
    for (const char *c = s_clip; *c; c++) {
        if (e->len >= e->max) break;
        if (ls_dui_edit_key(e, LS_TK_CHAR, *c)) any = true;
    }
    return any;
}

/* ----------------------------------------------------------------- menus -- */

static bool chooses(const ls_dui_item_t *it) { return !(it->flags & (LS_DUI_OFF | LS_DUI_SEP | LS_DUI_HEAD)); }

static int step(const ls_dui_popup_t *p, int from, int d)
{
    for (int k = 0, i = from; k < p->n; k++) {
        i += d;
        if (i < 0) i = p->n - 1;
        if (i >= p->n) i = 0;
        if (chooses(&p->items[i])) return i;
    }
    return from;
}

void ls_dui_popup_open(ls_dui_popup_t *p, const ls_dui_item_t *items, int n, int x, int y, int bar, uint8_t hue)
{
    memset(p, 0, sizeof(*p));
    p->items = items;
    p->n = n;
    p->x = x;
    p->y = y;
    p->bar = bar;
    p->hue = hue;
    p->open = n > 0;
    p->sel = -1;
    /* The ticked one is where a chooser starts; a menu starts on its first. */
    for (int i = 0; i < n && p->sel < 0; i++) if ((items[i].flags & LS_DUI_CHECK) && chooses(&items[i])) p->sel = i;
    if (p->sel < 0) p->sel = step(p, -1, 1);
}

void ls_dui_popup_draw(tui_surface *sf, tui_rect area, ls_dui_popup_t *p)
{
    if (!p->open || !sf) return;
    int lw = 4, kw = 0;
    for (int i = 0; i < p->n; i++) {
        const int a = p->items[i].label ? (int)strlen(p->items[i].label) : 0;
        const int b = p->items[i].keys ? (int)strlen(p->items[i].keys) : 0;
        if (a > lw) lw = a;
        if (b > kw) kw = b;
    }
    int w = 2 + 2 + lw + (kw ? 2 + kw : 0) + 1, h = p->n + 2;
    if (w > area.w) w = area.w;
    if (h > area.h) h = area.h;
    if (h < 3) return;
    int x = p->x, y = p->y;
    if (x + w > area.x + area.w) x = area.x + area.w - w;
    /* No room below: above where it was asked for, when that is better. */
    if (y + h > area.y + area.h) y = p->bar < 0 && p->y - h >= area.y ? p->y - h : area.y + area.h - h;
    if (x < area.x) x = area.x;
    if (y < area.y) y = area.y;
    const tui_rect box = tui_rect_make(x, y, w, h);
    p->box = box;
    const int rows = h - 2;
    if (p->sel >= 0 && p->sel < p->first) p->first = p->sel;
    if (p->sel >= p->first + rows) p->first = p->sel - rows + 1;
    if (p->first > p->n - rows) p->first = p->n - rows > 0 ? p->n - rows : 0;
    const uint8_t edge = TUI_ATTR(p->hue | TUI_BRIGHT, TUI_BLACK), low = TUI_ATTR(p->hue, TUI_BLACK);
    tui_fill(sf, box, ' ', TUI_ATTR(TUI_WHITE, TUI_BLACK));
    for (int i = 1; i < w - 1; i++) {
        ls_box_put(sf, box, x + i, y, LS_BOX_H, edge);
        ls_box_put(sf, box, x + i, y + h - 1, LS_BOX_H, low);
    }
    for (int i = 1; i < h - 1; i++) {
        ls_box_put(sf, box, x, y + i, LS_BOX_V, edge);
        ls_box_put(sf, box, x + w - 1, y + i, LS_BOX_V, low);
    }
    ls_box_put(sf, box, x, y, LS_BOX_DR, edge);
    ls_box_put(sf, box, x + w - 1, y, LS_BOX_DL, edge);
    ls_box_put(sf, box, x, y + h - 1, LS_BOX_UR, low);
    ls_box_put(sf, box, x + w - 1, y + h - 1, LS_BOX_UL, low);
    /* More above or below than the box shows. */
    if (p->first) ls_box_put(sf, box, x + w - 3, y, LS_SHAPE_UP, edge);
    if (p->first + rows < p->n) ls_box_put(sf, box, x + w - 3, y + h - 1, LS_SHAPE_DOWN, edge);
    const tui_rect in = tui_rect_make(x + 1, y + 1, w - 2, rows);
    for (int r = 0; r < rows && p->first + r < p->n; r++) {
        const int i = p->first + r, ry = in.y + r;
        const ls_dui_item_t *it = &p->items[i];
        if (it->flags & LS_DUI_SEP) {
            for (int k = 0; k < in.w; k++) ls_box_put(sf, in, in.x + k, ry, LS_BOX_H, LS_ATTR_FAINT);
            continue;
        }
        const bool on = i == p->sel && chooses(it);
        const uint8_t hue = it->hue ? it->hue : p->hue;
        uint8_t ink = it->flags & LS_DUI_OFF ? LS_ATTR_FAINT
                    : it->flags & LS_DUI_HEAD ? TUI_ATTR(hue, TUI_BLACK) : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        uint8_t key = it->flags & LS_DUI_OFF ? LS_ATTR_FAINT : TUI_ATTR(hue, TUI_BLACK);
        if (on) {
            /* The bar under the pointer: the one solid thing in a menu. */
            ink = key = TUI_ATTR(TUI_BLACK, p->hue & 0x07);
            tui_fill(sf, tui_rect_make(in.x, ry, in.w, 1), ' ', ink);
        }
        if (it->flags & LS_DUI_CHECK) ls_box_put(sf, in, in.x + 1, ry, LS_SHAPE_CHECK, on ? ink : TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        if (it->label) tui_put_str(sf, tui_rect_make(in.x, ry, in.w - (kw ? kw + 2 : 0), 1), in.x + 3 - ((it->flags & LS_DUI_HEAD) ? 2 : 0), ry, it->label, ink);
        if (it->keys) tui_put_str(sf, in, in.x + in.w - 1 - (int)strlen(it->keys), ry, it->keys, key);
    }
}

int ls_dui_popup_key(ls_dui_popup_t *p, ls_tk_t k, char ch)
{
    if (!p->open) return LS_DUI_NONE;
    const int rows = p->box.h > 2 ? p->box.h - 2 : 1;
    switch (k) {
    case LS_TK_UP: case LS_TK_WHEEL_UP: p->sel = step(p, p->sel, -1); return LS_DUI_NONE;
    case LS_TK_DOWN: case LS_TK_WHEEL_DOWN: p->sel = step(p, p->sel, 1); return LS_DUI_NONE;
    case LS_TK_HOME: p->sel = step(p, -1, 1); return LS_DUI_NONE;
    case LS_TK_END: p->sel = step(p, p->n, -1); return LS_DUI_NONE;
    case LS_TK_PGUP: case LS_TK_PGDN:
        for (int i = 1; i < rows; i++) {
            const int to = step(p, p->sel, k == LS_TK_PGUP ? -1 : 1);
            if ((k == LS_TK_PGUP) == (to > p->sel)) break;       /* not round the end */
            p->sel = to;
        }
        return LS_DUI_NONE;
    case LS_TK_LEFT: return p->bar >= 0 ? LS_DUI_PREV : LS_DUI_NONE;
    case LS_TK_RIGHT: return p->bar >= 0 ? LS_DUI_NEXT : LS_DUI_NONE;
    case LS_TK_ESC: p->open = false; return LS_DUI_CLOSED;
    case LS_TK_ENTER:
        if (p->sel < 0 || p->sel >= p->n || !chooses(&p->items[p->sel])) return LS_DUI_NONE;
        p->open = false;
        return p->items[p->sel].id;
    case LS_TK_CHAR: {
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        for (int n = 0, i = p->sel; n < p->n; n++) {
            i = (i + 1) % p->n;
            char c = p->items[i].label ? p->items[i].label[0] : 0;
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c == ch && chooses(&p->items[i])) { p->sel = i; break; }
        }
        return LS_DUI_NONE;
    }
    default: return LS_DUI_NONE;
    }
}

int ls_dui_popup_tap(ls_dui_popup_t *p, int col, int row)
{
    if (!p->open) return LS_DUI_NONE;
    if (!tui_rect_contains(p->box, col, row)) { p->open = false; return LS_DUI_CLOSED; }
    const int i = p->first + row - (p->box.y + 1);
    if (row <= p->box.y || row >= p->box.y + p->box.h - 1 || i < 0 || i >= p->n || !chooses(&p->items[i]))
        return LS_DUI_NONE;
    p->sel = i;
    p->open = false;
    return p->items[i].id;
}

void ls_dui_bar_draw(tui_surface *sf, tui_rect row, const char *const *titles, int n, int open,
                     uint8_t hue, int16_t *x)
{
    int at = row.x;
    for (int i = 0; i < n; i++) {
        const int w = (int)strlen(titles[i]) + 2;
        x[i] = (int16_t)at;
        const bool on = i == open;
        const uint8_t ink = on ? TUI_ATTR(TUI_BLACK, hue & 0x07) : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        if (on) tui_fill(sf, tui_rect_intersect(row, tui_rect_make(at, row.y, w, 1)), ' ', ink);
        tui_put_str(sf, row, at + 1, row.y, titles[i], ink);
        /* The letter Alt takes, picked out. */
        if (!on) tui_put_char(sf, row, at + 1, row.y, titles[i][0], TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK));
        at += w;
    }
    x[n] = (int16_t)at;
}

int ls_dui_bar_hit(const int16_t *x, int n, int col)
{
    for (int i = 0; i < n; i++) if (col >= x[i] && col < x[i + 1]) return i;
    return -1;
}

/* ------------------------------------------------- a menu, built by rows -- */

void ls_dui_menu_begin(ls_dui_menu_t *m) { m->n = 0; }

ls_dui_item_t *ls_dui_menu_item(ls_dui_menu_t *m, const char *label, const char *keys, int id, int flags)
{
    if (m->n >= LS_DUI_MENU_ITEMS) return NULL;
    ls_dui_item_t *it = &m->items[m->n];
    memset(it, 0, sizeof(*it));
    if (label) { snprintf(m->label[m->n], sizeof(m->label[0]), "%s", label); it->label = m->label[m->n]; }
    if (keys) { snprintf(m->keys[m->n], sizeof(m->keys[0]), "%s", keys); it->keys = m->keys[m->n]; }
    it->id = (int16_t)id;
    it->flags = (uint8_t)flags;
    m->n++;
    return it;
}

void ls_dui_menu_rule(ls_dui_menu_t *m) { ls_dui_menu_item(m, NULL, NULL, -1, LS_DUI_SEP); }

void ls_dui_menu_open(ls_dui_menu_t *m, int x, int y, int bar, uint8_t hue)
{
    ls_dui_popup_open(&m->pop, m->items, m->n, x, y, bar, hue);
}

/* ------------------------------------------------------- keys on a toolbar -- */

int ls_dui_tool(tui_surface *sf, tui_rect row, int x, const char *label, int cmd, bool on, bool off, uint8_t hue,
                ls_dui_hits_t *hits)
{
    tui_rect r;
    if (x + (int)strlen(label) + 2 > row.x + row.w) return 0;
    const int w = ls_dui_key(sf, row, x, row.y, label, on, off, hue, &r);
    if (!off && hits && hits->n < LS_DUI_HITS) { hits->r[hits->n] = r; hits->cmd[hits->n] = (int16_t)cmd; hits->n++; }
    return w + 1;
}

int ls_dui_hits_at(const ls_dui_hits_t *hits, int col, int row)
{
    for (int i = 0; i < hits->n; i++)
        if (tui_rect_contains(hits->r[i], col, row)) return hits->cmd[i];
    return -1;
}

/* ----------------------------------------------------------------- table -- */

void ls_dui_table_layout(ls_dui_table_t *t, tui_rect r, const ls_dui_col_t *cols, int n, uint32_t shown)
{
    if (n > LS_DUI_COLS) n = LS_DUI_COLS;
    t->head = tui_rect_make(r.x, r.y, r.w > 2 ? r.w - 2 : 0, r.h > 0 ? 1 : 0);
    t->body = tui_rect_make(r.x, r.y + 1, r.w > 2 ? r.w - 2 : 0, r.h > 1 ? r.h - 1 : 0);
    t->strip = tui_rect_make(r.x + r.w - 2, r.y + 1, 2, r.h > 1 ? r.h - 1 : 0);
    t->rows = t->body.h > 0 ? t->body.h : 1;
    const int room = t->body.w - 1;                 /* a cell of margin at the left for the cursor mark */
    /* Drop what does not fit, the least wanted first. A flexing column that
       is never dropped keeps all it wants while there is another to drop:
       a name cut to make room for an empty notes column helps nobody. */
    for (;;) {
        int need = 0, worst = -1;
        for (int i = 0; i < n; i++) {
            if (!(shown >> i & 1)) continue;
            need += ((cols[i].flags & LS_DUI_FLEX) && cols[i].drop ? cols[i].min : cols[i].w) + 1;
            if (cols[i].drop && (worst < 0 || cols[i].drop > cols[worst].drop)) worst = i;
        }
        if (need <= room || worst < 0) break;
        shown &= ~(1u << worst);
    }
    int used = 0, last_flex = -1;
    for (int i = 0; i < n; i++) {
        t->x[i] = -1;
        t->w[i] = 0;
        if (!(shown >> i & 1)) continue;
        t->w[i] = (cols[i].flags & LS_DUI_FLEX) && cols[i].drop ? cols[i].min : cols[i].w;
        used += t->w[i] + 1;
        if (cols[i].flags & LS_DUI_FLEX) last_flex = i;
    }
    /* Too narrow even so: the flexing ones give way, down to their least.
       Slack: each flexing column up to what it wants, the rest to the last
       of them. */
    int slack = room - used;
    for (int i = n - 1; i >= 0 && slack < 0; i--) {
        if (!(shown >> i & 1) || !(cols[i].flags & LS_DUI_FLEX)) continue;
        int less = t->w[i] - cols[i].min;
        if (less > -slack) less = -slack;
        if (less > 0) { t->w[i] = (uint8_t)(t->w[i] - less); slack += less; }
    }
    for (int i = 0; i < n && slack > 0; i++) {
        if (!(shown >> i & 1) || !(cols[i].flags & LS_DUI_FLEX)) continue;
        int more = cols[i].w - t->w[i];
        if (more > slack) more = slack;
        if (more > 0) { t->w[i] = (uint8_t)(t->w[i] + more); slack -= more; }
    }
    if (slack > 0 && last_flex >= 0) t->w[last_flex] = (uint8_t)(t->w[last_flex] + slack > 250 ? 250 : t->w[last_flex] + slack);
    int at = t->body.x + 1;
    for (int i = 0; i < n; i++) {
        if (!(shown >> i & 1)) continue;
        t->x[i] = (int16_t)at;
        at += t->w[i] + 1;
    }
}

void ls_dui_table_head(tui_surface *sf, const ls_dui_table_t *t, const ls_dui_col_t *cols, int n,
                       int sort_col, bool desc, uint8_t hue)
{
    for (int i = 0; i < n && i < LS_DUI_COLS; i++) {
        if (t->x[i] < 0) continue;
        const bool on = i == sort_col;
        const int len = (int)strlen(cols[i].title), w = t->w[i];
        const tui_rect clip = tui_rect_intersect(t->head, tui_rect_make(t->x[i], t->head.y, w, 1));
        const uint8_t ink = on ? TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK) : TUI_ATTR(TUI_CYAN, TUI_BLACK);
        int x = t->x[i];
        if ((cols[i].flags & LS_DUI_RIGHT) && len + (on ? 1 : 0) < w) x += w - len - (on ? 1 : 0);
        tui_put_str(sf, clip, x, t->head.y, cols[i].title, ink);
        if (on && len < w) ls_box_put(sf, clip, x + len, t->head.y, desc ? LS_SHAPE_DOWN : LS_SHAPE_UP, ink);
    }
}

void ls_dui_table_cell(tui_surface *sf, const ls_dui_table_t *t, const ls_dui_col_t *cols, int col,
                       int r, const char *text, uint8_t attr)
{
    if (col < 0 || col >= LS_DUI_COLS || t->x[col] < 0 || r < 0 || r >= t->body.h) return;
    const int w = t->w[col], len = (int)strlen(text);
    const tui_rect clip = tui_rect_intersect(t->body, tui_rect_make(t->x[col], t->body.y + r, w, 1));
    int x = t->x[col];
    if ((cols[col].flags & LS_DUI_RIGHT) && len < w) x += w - len;
    tui_put_str(sf, clip, x, t->body.y + r, text, attr);
    /* Cut short: the last cell says there is more. */
    if (len > w && w > 1) tui_put_char(sf, clip, t->x[col] + w - 1, t->body.y + r, '~', LS_ATTR_FAINT);
}

void ls_dui_table_clamp(ls_dui_table_t *t, int total)
{
    if (t->first > total - t->rows) t->first = total - t->rows;
    if (t->first < 0) t->first = 0;
}

void ls_dui_table_follow(ls_dui_table_t *t, int sel, int total)
{
    if (sel < t->first) t->first = sel;
    if (sel >= t->first + t->rows) t->first = sel - t->rows + 1;
    ls_dui_table_clamp(t, total);
}

void ls_dui_table_strip(tui_surface *sf, const ls_dui_table_t *t, int total, uint8_t hue)
{
    const tui_rect s = t->strip;
    if (s.h < 2) return;
    const uint8_t lit = TUI_ATTR(hue | TUI_BRIGHT, TUI_BLACK);
    const int x = s.x + 1;
    ls_box_put(sf, s, x, s.y, LS_SHAPE_UP, t->first ? lit : LS_ATTR_FAINT);
    ls_box_put(sf, s, x, s.y + s.h - 1, LS_SHAPE_DOWN, t->first + t->rows < total ? lit : LS_ATTR_FAINT);
    const int span = s.h - 2;
    if (span < 1) return;
    for (int i = 0; i < span; i++) ls_box_put(sf, s, x, s.y + 1 + i, LS_BOX_DASH_V, LS_ATTR_FAINT);
    if (total <= t->rows) return;
    /* The thumb: a doubled line, as long as the window is of the list. */
    int len = t->rows * span / total, top = (int)((long)t->first * span / total);
    if (len < 1) len = 1;
    if (top + len > span) top = span - len;
    for (int i = 0; i < len; i++) ls_box_put(sf, s, x, s.y + 1 + top + i, LS_BOX_DBL_V, TUI_ATTR(hue, TUI_BLACK));
}

int ls_dui_table_hit(const ls_dui_table_t *t, int n, int col, int row, int *what)
{
    if (tui_rect_contains(t->strip, col, row)) {
        *what = row < t->strip.y + t->strip.h / 2 ? -1 : 1;
        return LS_DUI_T_STRIP;
    }
    if (tui_rect_contains(t->head, col, row)) {
        for (int i = 0; i < n && i < LS_DUI_COLS; i++)
            if (t->x[i] >= 0 && col >= t->x[i] && col < t->x[i] + t->w[i]) { *what = i; return LS_DUI_T_HEAD; }
        return LS_DUI_T_NONE;
    }
    if (tui_rect_contains(t->body, col, row)) {
        *what = t->first + row - t->body.y;
        return LS_DUI_T_ROW;
    }
    return LS_DUI_T_NONE;
}

/* -------------------------------------------------------------- one key -- */

int ls_dui_key(tui_surface *sf, tui_rect clip, int x, int y, const char *label, bool on, bool off,
               uint8_t hue, tui_rect *hit)
{
    const int w = (int)strlen(label) + 2;
    const uint8_t edge = off ? LS_ATTR_FAINT : TUI_ATTR(hue, TUI_BLACK);
    const uint8_t ink = off ? LS_ATTR_FAINT : on ? TUI_ATTR(TUI_BLACK, hue & 0x07) : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    tui_put_char(sf, clip, x, y, '[', edge);
    tui_put_str(sf, clip, x + 1, y, label, ink);
    tui_put_char(sf, clip, x + w - 1, y, ']', edge);
    if (hit) *hit = tui_rect_intersect(clip, tui_rect_make(x, y, w, 1));
    return w;
}

#endif /* LS_TUI_HAS_COMPACT */
