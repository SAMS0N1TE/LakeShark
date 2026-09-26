#include "ls_textbuf.h"
#include <string.h>

/* Lines are laid out one cell narrower than the pane, so the cursor always
   has a cell to sit in at the end of a full line. */
static int layout_width(int width) { return width > 1 ? width - 1 : 1; }

static bool keep(char c) { return c == '\n' || (c >= 0x20 && c < 0x7f); }

void ls_textbuf_init(ls_textbuf_t *b, char *storage, int cap, const char *initial)
{
    memset(b, 0, sizeof(*b));
    b->text = storage; b->cap = cap; b->want_col = -1;
    if (!storage || cap < 1) { b->cap = 0; return; }
    storage[0] = 0;
    if (initial) ls_textbuf_insert(b, initial);
    b->cursor = 0; b->dirty = false;
}

bool ls_textbuf_insert(ls_textbuf_t *b, const char *s)
{
    if (!b || !b->text || !s) return false;
    int need = 0;
    for (const char *p = s; *p; p++) need += *p == '\t' ? 2 : keep(*p);
    if (!need) return true;
    if (b->len + need >= b->cap) return false;
    memmove(b->text + b->cursor + need, b->text + b->cursor, (size_t)(b->len - b->cursor + 1));
    int at = b->cursor;
    for (const char *p = s; *p; p++) {
        if (*p == '\t') { b->text[at++] = ' '; b->text[at++] = ' '; }
        else if (keep(*p)) b->text[at++] = *p;
    }
    b->len += need; b->cursor = at; b->want_col = -1; b->dirty = true;
    return true;
}

bool ls_textbuf_insert_char(ls_textbuf_t *b, char c)
{
    const char s[2] = { c, 0 };
    return ls_textbuf_insert(b, s);
}

static void remove_at(ls_textbuf_t *b, int at)
{
    memmove(b->text + at, b->text + at + 1, (size_t)(b->len - at));
    b->len--; b->want_col = -1; b->dirty = true;
}

bool ls_textbuf_backspace(ls_textbuf_t *b)
{
    if (!b || !b->text || b->cursor == 0) return false;
    b->cursor--; remove_at(b, b->cursor); return true;
}

bool ls_textbuf_delete(ls_textbuf_t *b)
{
    if (!b || !b->text || b->cursor >= b->len) return false;
    remove_at(b, b->cursor); return true;
}

void ls_textbuf_mark_clean(ls_textbuf_t *b) { if (b) b->dirty = false; }

int ls_textbuf_line(const char *text, int len, int start, int width, int *end)
{
    if (width < 1) width = 1;
    const int lim = start + width;
    int i = start;
    for (; i < len && i < lim; i++)
        if (text[i] == '\n') { *end = i; return i + 1; }
    if (i >= len) { *end = len; return len; }
    if (text[lim] == '\n' || text[lim] == ' ') { *end = lim; return lim + 1; }
    for (int j = lim - 1; j > start; j--)
        if (text[j] == ' ') { *end = j; return j + 1; }
    *end = lim; return lim;
}

typedef struct { int start, end, next; } row_t;

static void row_first(const ls_textbuf_t *b, int w, row_t *r)
{
    r->start = 0;
    r->next = ls_textbuf_line(b->text, b->len, 0, w, &r->end);
}

/* The last row is followed by an empty one when the text ends in the
   character that broke it, or fills it exactly: that is where the cursor
   goes after typing the newline or the last letter. */
static bool row_next(const ls_textbuf_t *b, int w, row_t *r)
{
    if (r->next < b->len) {
        r->start = r->next;
        r->next = ls_textbuf_line(b->text, b->len, r->start, w, &r->end);
        return true;
    }
    if (r->start == b->len) return false;
    if (r->next > r->end || r->end - r->start >= w) {
        r->start = r->end = r->next = b->len;
        return true;
    }
    return false;
}

int ls_textbuf_rows(const ls_textbuf_t *b, int width)
{
    if (!b || !b->text) return 0;
    const int w = layout_width(width);
    row_t r; row_first(b, w, &r);
    int n = 1;
    while (row_next(b, w, &r)) n++;
    return n;
}

void ls_textbuf_locate(const ls_textbuf_t *b, int width, int *row, int *col)
{
    int rr = 0, cc = 0;
    if (b && b->text) {
        const int w = layout_width(width);
        row_t r; row_first(b, w, &r);
        for (int n = 0;; n++) {
            row_t here = r;
            bool more = row_next(b, w, &r);
            /* A hard-wrapped row ends where the next begins, so its last
               index already belongs to the next row. */
            if (b->cursor < here.next || !more) {
                rr = n; cc = b->cursor - here.start;
                if (cc > here.end - here.start) cc = here.end - here.start;
                break;
            }
        }
    }
    if (row) *row = rr;
    if (col) *col = cc;
}

int ls_textbuf_index_at(const ls_textbuf_t *b, int width, int row, int col)
{
    if (!b || !b->text) return 0;
    if (row < 0) return 0;
    if (col < 0) col = 0;
    const int w = layout_width(width);
    row_t r; row_first(b, w, &r);
    for (int n = 0; n < row; n++)
        if (!row_next(b, w, &r)) return b->len;
    const int span = r.end - r.start;
    return r.start + (col < span ? col : span);
}

int ls_textbuf_row_start(const ls_textbuf_t *b, int width, int row)
{
    if (!b || !b->text) return 0;
    const int w = layout_width(width);
    row_t r; row_first(b, w, &r);
    for (int n = 0; n < row; n++)
        if (!row_next(b, w, &r)) return b->len;
    return r.start;
}

static bool space(char c) { return c == ' ' || c == '\n'; }

void ls_textbuf_move(ls_textbuf_t *b, ls_textbuf_move_t how, int width, int rows)
{
    if (!b || !b->text) return;
    int row, col;
    ls_textbuf_locate(b, width, &row, &col);
    const bool vertical = how == LS_TB_UP || how == LS_TB_DOWN ||
                          how == LS_TB_PAGE_UP || how == LS_TB_PAGE_DOWN;
    if (vertical && b->want_col < 0) b->want_col = col;
    const int page = rows > 1 ? rows - 1 : 1;
    switch (how) {
    case LS_TB_LEFT:  if (b->cursor > 0) b->cursor--; break;
    case LS_TB_RIGHT: if (b->cursor < b->len) b->cursor++; break;
    case LS_TB_UP:
        if (row > 0) b->cursor = ls_textbuf_index_at(b, width, row - 1, b->want_col);
        else b->cursor = 0;
        break;
    case LS_TB_DOWN: {
        const int total = ls_textbuf_rows(b, width);
        if (row + 1 < total) b->cursor = ls_textbuf_index_at(b, width, row + 1, b->want_col);
        else b->cursor = b->len;
        break; }
    case LS_TB_PAGE_UP:
        b->cursor = ls_textbuf_index_at(b, width, row > page ? row - page : 0, b->want_col);
        break;
    case LS_TB_PAGE_DOWN: {
        const int total = ls_textbuf_rows(b, width);
        const int to = row + page < total ? row + page : total - 1;
        b->cursor = ls_textbuf_index_at(b, width, to, b->want_col);
        break; }
    case LS_TB_HOME: b->cursor = ls_textbuf_index_at(b, width, row, 0); break;
    case LS_TB_END:  b->cursor = ls_textbuf_index_at(b, width, row, 1 << 30); break;
    case LS_TB_TOP:    b->cursor = 0; break;
    case LS_TB_BOTTOM: b->cursor = b->len; break;
    case LS_TB_WORD_LEFT:
        while (b->cursor > 0 && space(b->text[b->cursor - 1])) b->cursor--;
        while (b->cursor > 0 && !space(b->text[b->cursor - 1])) b->cursor--;
        break;
    case LS_TB_WORD_RIGHT:
        while (b->cursor < b->len && !space(b->text[b->cursor])) b->cursor++;
        while (b->cursor < b->len && space(b->text[b->cursor])) b->cursor++;
        break;
    }
    if (!vertical) b->want_col = -1;
}

void ls_textbuf_keep_visible(ls_textbuf_t *b, int width, int rows)
{
    if (!b || rows < 1) return;
    int row;
    ls_textbuf_locate(b, width, &row, NULL);
    if (row < b->scroll) b->scroll = row;
    if (row >= b->scroll + rows) b->scroll = row - rows + 1;
    const int total = ls_textbuf_rows(b, width);
    if (b->scroll > total - rows) b->scroll = total - rows;
    if (b->scroll < 0) b->scroll = 0;
}
