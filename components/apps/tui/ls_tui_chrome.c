/* See ls_tui_chrome.h for why this is arithmetic and not rendering. */
#include "ls_tui_chrome.h"
#include "ls_tui_screen.h"

#define GAP 2   /* columns between two neighbouring fields */

ls_tui_status_layout_t ls_tui_status_layout(int cols, int brand_len,
                                            int name_len, int left_len,
                                            int right_len)
{
    ls_tui_status_layout_t l = { -1, -1, -1, -1, -1, -1 };
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
