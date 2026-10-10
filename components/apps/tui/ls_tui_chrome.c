/* See ls_tui_chrome.h for why this is arithmetic and not rendering. */
#include "ls_tui_chrome.h"
#include "ls_tui_screen.h"

#define GAP 2   /* columns between two neighbouring fields */

ls_tui_status_layout_t ls_tui_status_layout(int cols, int brand_len,
                                            int name_len, int left_len,
                                            int right_len)
{
    ls_tui_status_layout_t l = { -1, -1, -1, -1, -1, -1, 0 };
    if (cols < 4) return l;

    int first = 1;
    if (cols >= LS_TUI_CLOCK_W + 1 + LS_TUI_HELP_W + 6) {
        l.clock_x = 0;
        l.help_x  = LS_TUI_CLOCK_W + 1;
        first = LS_TUI_CLOCK_W + 1 + LS_TUI_HELP_W + 1;
    } else if (cols >= LS_TUI_HELP_W + 4) {
        l.help_x = 0;
        first = LS_TUI_HELP_W + 1;
    }

    /* The right field is placed first because it is anchored to the right
       edge; everything else then fills leftward from column 1 up to it. */
    int limit = cols - 1;                 /* exclusive left edge of the tail */
    if (right_len > 0 && right_len + 2 <= cols) {
        l.right_x = cols - 1 - right_len;
        limit = l.right_x - GAP;
    }

    int x = first;
    if (brand_len > 0 && x + brand_len <= limit) {
        l.brand_x = x;
        x += brand_len + GAP;
    }
    if (name_len > 0 && x + name_len <= limit) {
        l.name_x = x;
        x += name_len + GAP;
    }
    if (left_len > 0 && x + left_len <= limit) {
        l.left_x = x;
    }

    /* At 48 columns the wordmark plus a screen name plus a status string does not fit beside the right field, and the wordmark is the one nobody needs: it is the same on every screen. */

    if ((l.name_x < 0 || (left_len > 0 && l.left_x < 0)) && l.brand_x >= 0) {
        l.brand_x = -1;
        x = first;
        if (name_len > 0 && x + name_len <= limit) {
            l.name_x = x;
            x += name_len + GAP;
        }
        if (left_len > 0 && x + left_len <= limit) l.left_x = x;
    }
    return l;
}

/* See ls_tui_chrome.h. A shift and nothing else: the layout above is
   tested at every width, and a second copy of it for a padded row would be a
   second thing to keep in step. */
ls_tui_status_layout_t ls_tui_status_layout_at(int x0, int width,
                                               int brand_len, int name_len,
                                               int left_len, int right_len)
{
    ls_tui_status_layout_t l = ls_tui_status_layout(width, brand_len,
                                                    name_len, left_len,
                                                    right_len);
    if (x0 <= 0) return l;
    int *const field[] = { &l.clock_x, &l.help_x, &l.brand_x,
                           &l.name_x, &l.left_x, &l.right_x };
    for (unsigned i = 0; i < sizeof(field) / sizeof(field[0]); i++)
        if (*field[i] >= 0) *field[i] += x0;
    return l;
}

ls_tui_status_layout_t ls_tui_status_layout_split(int x0, int g0, int g1,
                                                  int x1, int brand_len,
                                                  int name_len, int left_len,
                                                  int right_len,
                                                  const char *left)
{
    /* Clock and [?] exactly where the unsplit row puts them. */
    ls_tui_status_layout_t l = ls_tui_status_layout_at(x0, g0 - x0, 0, 0, 0, 0);
    const int first = l.help_x >= 0 ? l.help_x + LS_TUI_HELP_W + 1 : x0 + 1;

    /* The right field is anchored to the right edge, as before. */
    int limit = x1 - 1;
    if (right_len > 0 && g1 + right_len + 1 <= x1) {
        l.right_x = x1 - 1 - right_len;
        limit = l.right_x - GAP;
    }

    /* Wordmark left of the hole, name and left text right of it. */
    const bool brand_fits = brand_len > 0 && first + brand_len <= g0;
    const int name_end = g1 + name_len;
    if (name_len > 0 && name_end <= limit &&
        (left_len <= 0 || name_end + GAP + left_len <= limit)) {
        if (brand_fits) l.brand_x = first;
        l.name_x = g1;
        if (left_len > 0) l.left_x = name_end + GAP;
        return l;
    }

    /* Too long for that: the name left, the left text right, cut at a word
       when the whole of it does not fit. */
    int x = g1;
    if (name_len > 0 && first + name_len <= g0) l.name_x = first;
    else if (name_len > 0 && g1 + name_len <= limit) {
        l.name_x = g1;
        x = g1 + name_len + GAP;
    }
    /* Leading spaces are the caller's own separator; beside the hole they
       would only push the words further from it. */
    int lead = 0;
    while (left && lead < left_len && left[lead] == ' ') lead++;
    if (left_len > lead && x < limit) {
        const int words = left_len - lead;
        const int n = words <= limit - x ? words
                                         : ls_tui_hint_fit(left + lead, limit - x);
        if (n > 0) {
            l.left_x = x - lead;
            l.left_n = n < words ? lead + n : 0;
        }
    }
    return l;
}

bool ls_tui_tab_split(int cols, int ntab, int g0, int g1, int *x, int *w)
{
    const int nl = ntab / 2, nr = ntab - nl;
    if (nl < 1 || g0 < 1 || g1 >= cols) return false;
    const int span[2] = { g0, cols - g1 }, n[2] = { nl, nr };
    int step[2];
    for (int s = 0; s < 2; s++) {
        step[s] = (span[s] - 1) / n[s];      /* shared borders: n*step + 1 */
        if (step[s] + 1 < 5) return false;
    }
    /* Left side flush with column 0; right side flush with the last column.
       What does not divide evenly is left in the gap. */
    const int right0 = cols - (nr * step[1] + 1);
    for (int i = 0; i < ntab; i++) {
        const int s = i < nl ? 0 : 1, k = s ? i - nl : i;
        x[i] = (s ? right0 : 0) + k * step[s];
        w[i] = step[s] + 1;
    }
    return true;
}

/* ------------------------------------------------------------- runaround */

static bool is_line(int16_t ch) { return ch == '|' || ch == '+'; }
static bool is_text(int16_t ch) { return ch > ' ' && ch < 0x7F && !is_line(ch); }

static tui_cell *at(tui_surface *sf, int c, int r)
{
    return &sf->back[(size_t)r * sf->w + c];
}

#define MAX_RUNS 24

/* Move the text on row r inward (dir +1 rightward, -1 leftward) until none
   of it is at or beyond `clear`, scanning inward from `from`. A run is words
   joined by single spaces; a run that moves keeps two spaces to the next,
   which is pushed in turn. A line or a picture is a wall. False, touching
   nothing, when it does not fit. */
static bool push_row(tui_surface *sf, int r, int clear, int from, int dir,
                     bool apply)
{
    int near[MAX_RUNS], far[MAX_RUNS], n = 0;
    int wall = dir > 0 ? sf->w : -1;
    for (int c = from; c >= 0 && c < sf->w; c += dir) {
        const int16_t ch = at(sf, c, r)->ch;
        if (ch == ' ') continue;
        if (!is_text(ch)) { wall = c; break; }
        if (n && (c - far[n - 1]) * dir <= 2) { far[n - 1] = c; continue; }
        if (n == MAX_RUNS) return false;
        near[n] = far[n] = c;
        n++;
    }
    /* A wall inside the cleared span cannot be moved. */
    if ((clear - wall) * dir >= 0) return false;

    int shift[MAX_RUNS], moved = 0;
    for (int i = 0; i < n; i++) {
        const int need = (clear - near[i]) * dir + 1;
        if (need <= 0) break;
        if ((wall - (far[i] + dir * need)) * dir <= 1) return false;
        shift[i] = need;
        clear = far[i] + dir * (need + 2);
        moved = i + 1;
    }
    if (!apply) return true;
    /* Innermost first, each from its inward end, so nothing lands on text
       that has not moved yet. */
    for (int i = moved - 1; i >= 0; i--) {
        const int s = shift[i];
        for (int k = far[i];; k -= dir) {
            *at(sf, k + dir * s, r) = *at(sf, k, r);
            if (k == near[i]) break;
        }
        for (int k = near[i]; (k - far[i]) * dir <= 0 &&
                              (k - (near[i] + dir * s)) * dir < 0; k += dir)
            at(sf, k, r)->ch = ' ';
    }
    return true;
}

/* The whole row's text moved `s` cells inward as one block, so columns of
   values stay columns. How far the first run has to go comes back in *need.
   False, touching nothing, when the block would reach a line or a picture. */
static bool shift_row(tui_surface *sf, int r, int clear, int from, int dir,
                      int s, int *need, bool apply)
{
    int first = -1, last = -1, wall = dir > 0 ? sf->w : -1;
    for (int c = from; c >= 0 && c < sf->w; c += dir) {
        const int16_t ch = at(sf, c, r)->ch;
        if (ch == ' ') continue;
        if (!is_text(ch)) { wall = c; break; }
        if (first < 0) first = c;
        last = c;
    }
    if (need) *need = first < 0 ? 0 : (clear - first) * dir + 1;
    if ((clear - wall) * dir >= 0) return false;
    if (first < 0 || s <= 0) return true;
    if ((clear - (first + dir * s)) * dir >= 0) return false;
    if ((wall - (last + dir * s)) * dir <= 1) return false;
    if (!apply) return true;
    for (int k = last;; k -= dir) {
        *at(sf, k + dir * s, r) = *at(sf, k, r);
        at(sf, k, r)->ch = ' ';
        if (k == first) break;
    }
    return true;
}

/* Text in rows [r0, r1] out of the way: one block shift for every row when
   that fits, otherwise run by run. */
static bool clear_rows(tui_surface *sf, int r0, int r1, int clear, int from,
                       int dir, bool apply)
{
    int s = 0;
    bool block = true;
    for (int r = r0; r <= r1; r++) {
        int need;
        if (!shift_row(sf, r, clear, from, dir, 0, &need, false)) block = false;
        if (need > s) s = need;
    }
    for (int r = r0; r <= r1 && block; r++)
        block = shift_row(sf, r, clear, from, dir, s, NULL, false);
    for (int r = r0; r <= r1; r++) {
        if (block) { if (apply) shift_row(sf, r, clear, from, dir, s, NULL, true); }
        else if (!push_row(sf, r, clear, from, dir, apply)) return false;
    }
    return true;
}

void ls_tui_cutout_runaround(tui_surface *sf, tui_rect hole)
{
    if (!sf || !sf->back || hole.w <= 0 || hole.h <= 0) return;
    const int W = sf->w, H = sf->h;
    const int top = hole.y, bot = hole.y + hole.h - 1;
    const bool right = hole.x + hole.w >= W, left = hole.x <= 0;
    const int dir = right ? -1 : 1;                      /* inward */
    const int outer = right ? W - 1 : 0;
    const int inner = right ? hole.x : hole.x + hole.w - 1;  /* inward hole edge */

    if (right != left && top >= 1 && bot + 1 < H) {
        /* A border straight through the hole; the innermost if several. */
        int line = -1;
        for (int c = outer; (c - inner) * dir <= 0; c += dir) {
            if (!is_line(at(sf, c, top - 1)->ch) ||
                !is_line(at(sf, c, bot + 1)->ch)) continue;
            bool through = true;
            for (int r = top; r <= bot; r++)
                if (at(sf, c, r)->ch != '|') through = false;
            if (through) line = c;
        }
        /* It goes one clear cell inside the hole, text one more inside. */
        const int bend = inner + 2 * dir;
        if (line >= 0 && bend > 0 && bend < W - 1) {
            /* A jog row that is itself a border has a corner to keep. */
            bool ok = at(sf, line + dir, top - 1)->ch != '-' &&
                      at(sf, line + dir, bot + 1)->ch != '-' &&
                      clear_rows(sf, top - 1, bot + 1, bend + dir, line + dir,
                                 dir, false);
            if (ok) {
                const uint8_t a = at(sf, line, top - 1)->attr;
                clear_rows(sf, top - 1, bot + 1, bend + dir, line + dir, dir, true);
                for (int r = top; r <= bot; r++) {
                    for (int c = line; c != bend; c += dir) at(sf, c, r)->ch = ' ';
                    *at(sf, bend, r) = (tui_cell){ '|', a };
                }
                for (int r = top - 1; r <= bot + 1; r += bot - top + 2) {
                    for (int c = line + dir; c != bend; c += dir)
                        *at(sf, c, r) = (tui_cell){ '-', a };
                    *at(sf, bend, r) = (tui_cell){ '+', a };
                    *at(sf, line, r) = (tui_cell){ '+', a };
                }
            }
        } else if (line < 0) {
            /* No border: text beside the hole moves inward if it can, one
               clear cell from the hole. */
            if (clear_rows(sf, top, bot, inner + dir, outer, dir, false))
                clear_rows(sf, top, bot, inner + dir, outer, dir, true);
        }
    }

    /* Whatever is still under the hole keeps its ground and loses its ink. */
    for (int r = top; r <= bot && r < H; r++)
        for (int c = hole.x; c < hole.x + hole.w && c < W; c++) {
            tui_cell *cell = at(sf, c, r);
            if (is_text(cell->ch) || is_line(cell->ch)) cell->ch = ' ';
        }
}

ls_tui_hint_layout_t ls_tui_hint_layout(int cols)
{
    /* Two spellings of the same reminder. The long one is what a landscape
       row has room for; the short one keeps the two keys that are not
       discoverable any other way. */
    static const char LONG[]  = "F10 help  F11 turn";
    static const char SHORT[] = "F10 F11";
    const int long_len  = (int)sizeof(LONG) - 1;
    const int short_len = (int)sizeof(SHORT) - 1;

    ls_tui_hint_layout_t l;
    l.tail = LONG;
    l.tail_x = -1;
    l.hint_end_x = cols > 1 ? cols - 1 : 1;

    /* A tail is only worth drawing if some hint text can sit beside it.
       "1" is the hint's start column, so it needs at least a few columns of
       its own before the tail earns its space. */
    const int MIN_HINT = 6;

    if (1 + MIN_HINT + GAP + long_len <= cols - 1) {
        l.tail = LONG;
        l.tail_x = cols - 1 - long_len;
    } else if (1 + MIN_HINT + GAP + short_len <= cols - 1) {
        l.tail = SHORT;
        l.tail_x = cols - 1 - short_len;
    } else {
        /* No room for either: the hint gets the whole row. F10 is still on
           the keyboard and the tab strip is still tappable. */
        l.tail = SHORT;
        l.tail_x = -1;
        return l;
    }

    l.hint_end_x = l.tail_x - GAP;
    if (l.hint_end_x < 1) l.hint_end_x = 1;
    return l;
}

int ls_tui_hint_fit(const char *hint, int width)
{
    if (!hint || width <= 0) return 0;

    int len = 0;
    while (hint[len]) len++;
    if (len <= width) return len;

    /* Two boundaries, both tracked in one pass: where a group ends, and
       where a word ends. A group is preferred - dropping a whole "KEY label"
       leaves a legend that still reads - but not every hint is a run of
       groups. DIAG's is a sentence with single spaces, so a group-only rule
       found no boundary at all and truncated it to "counters are red once
       they", which is the cut this exists to prevent. */
    int group = -1, word = -1;
    for (int i = 0; i <= width && i < len; i++) {
        if (hint[i] != ' ') continue;
        word = i;
        if (hint[i + 1] == ' ') group = i;
    }

    int n = group >= 0 ? group : word;

    /* One word wider than the row. Nothing to drop, so half of it beats
       nothing, and this is the only path that cuts inside a word. */
    if (n < 0) return width;

    /* Separators run to three spaces in places, so a boundary found at the
       second of them would keep an invisible trailing column - and at the
       wrong width that shifts everything after it by one. */
    while (n > 0 && hint[n - 1] == ' ') n--;
    return n;
}

void ls_tui_split_at(tui_rect area, int want, tui_rect *first,
                     tui_rect *second)
{
    /* Same orientation rule as the even split, so a screen that uses both
       cannot end up with them disagreeing about which way it went. */
    if (area.w >= 60) {
        int take = want;
        if (take < 1) take = 1;
        if (take > area.w - 1) take = area.w - 1;
        if (first)  *first  = tui_rect_make(area.x, area.y, take, area.h);
        if (second) *second = tui_rect_make(area.x + take, area.y,
                                            area.w - take, area.h);
    } else {
        int take = want;
        if (take < 1) take = 1;
        if (take > area.h - 1) take = area.h - 1;
        if (first)  *first  = tui_rect_make(area.x, area.y, area.w, take);
        if (second) *second = tui_rect_make(area.x, area.y + take,
                                            area.w, area.h - take);
    }
}

void ls_tui_split(tui_rect area, tui_rect *first, tui_rect *second)
{
    /* 60 columns is the width at which two panes each still hold a label and
       a value without truncating - below it, stacking beats splitting. */
    if (area.w >= 60) {
        int half = area.w / 2;
        if (first)  *first  = tui_rect_make(area.x, area.y, half, area.h);
        if (second) *second = tui_rect_make(area.x + half, area.y,
                                            area.w - half, area.h);
    } else {
        int half = area.h / 2;
        if (first)  *first  = tui_rect_make(area.x, area.y, area.w, half);
        if (second) *second = tui_rect_make(area.x, area.y + half,
                                            area.w, area.h - half);
    }
}
