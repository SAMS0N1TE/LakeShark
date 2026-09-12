/* See ls_quick.h for what a control is and why it is a description. */
#include "ls_quick.h"

#include <stdio.h>
#include <string.h>

#include "ls_tui_ui.h"
#include "ls_value.h"

#define FLASH_FRAMES 6
static int s_flash_target = -1;
static int s_flash_ttl;

/* What the last control said, when it said no. */

#define SAY_FRAMES 40
static char s_say[48];
static int  s_say_ttl;

ls_cap_t ls_quick_grant_builtin(void)
{
    return (ls_cap_t)(LS_CAP_READ | LS_CAP_TUNE | LS_CAP_UI |
                      LS_CAP_STORE | LS_CAP_POWER);
}

/* ------------------------------------------------------------- reading -- */

static bool read_value(const ls_quick_t *it, ls_val_t *out)
{
    if (!it || !it->value) return false;
    return ls_value_read(it->value, out, NULL);
}

const char *ls_quick_state(const ls_quick_t *item)
{
    static char buf[24];
    ls_val_t v;
    if (!read_value(item, &v)) return NULL;

    switch (v.kind) {
    case LS_VAL_INT:   snprintf(buf, sizeof(buf), "%ld", v.i); break;
    case LS_VAL_FLOAT: snprintf(buf, sizeof(buf), "%.1f", (double)v.f); break;
    case LS_VAL_BOOL:  snprintf(buf, sizeof(buf), "%s", v.i ? "on" : "off"); break;
    case LS_VAL_TEXT:  snprintf(buf, sizeof(buf), "%.20s", v.s ? v.s : "-"); break;
    default: return NULL;
    }
    return buf;
}

/* Case-insensitive whole-string compare, because the value a control reads is
   what the screen displays and the choice it writes is what the action
   parses, and those are not the same spelling: fm.mode reads back "POCSAG"
   while fm.submode takes "pocsag". The action itself uses strcasecmp; this
   agrees with it. */
static bool same_loose(const char *a, const char *b)
{
    while (*a && *b) {
        const char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
        a++; b++;
    }
    return !*a && !*b;
}

static int cycle_now(const ls_quick_t *it)
{
    if (!it->choices || it->nchoices <= 0) return -1;

    ls_val_t v;
    if (!read_value(it, &v)) return -1;

    char now[24];
    switch (v.kind) {
    case LS_VAL_INT:   snprintf(now, sizeof(now), "%ld", v.i); break;
    case LS_VAL_BOOL:  snprintf(now, sizeof(now), "%s", v.i ? "true" : "false"); break;
    case LS_VAL_FLOAT: snprintf(now, sizeof(now), "%.0f", (double)v.f); break;
    case LS_VAL_TEXT:  snprintf(now, sizeof(now), "%.20s", v.s ? v.s : ""); break;
    default: return -1;
    }
    for (int i = 0; i < it->nchoices; i++)
        if (it->choices[i] && same_loose(it->choices[i], now)) return i;
    return -1;
}

/* ------------------------------------------------------------- firing --- */

/* Where in the action table this control's action lives, so the argument can
   be parsed against the signature the action actually declares. */
static int action_index(const char *path)
{
    if (!path) return -1;
    for (int i = 0; i < ls_action_count(); i++) {
        const char *n = ls_action_name(i);
        if (n && strcmp(n, path) == 0) return i;
    }
    return -1;
}

static ls_act_status_t call_with_text(const char *path, const char *text,
                                      ls_cap_t granted)
{
    const int idx = action_index(path);
    if (idx < 0) return LS_ACT_UNKNOWN;

    ls_args_t args;
    memset(&args, 0, sizeof(args));

    const char *sig = ls_action_sig(idx);
    if (sig && sig[0]) {
        if (!text) return LS_ACT_BADARG;
        if (!ls_action_parse_arg(idx, 0, text, &args.v[0])) return LS_ACT_BADARG;
        args.n = 1;
    }
    ls_val_t out;
    return ls_action_call(path, &args, &out, granted);
}

ls_act_status_t ls_quick_fire(const ls_quick_t *item, ls_cap_t granted)
{
    if (!item || !item->action) return LS_ACT_UNKNOWN;

    char text[24];
    ls_val_t v;

    switch (item->kind) {
    case LS_QUICK_STEP: {
        if (!read_value(item, &v)) return LS_ACT_UNAVAILABLE;
        float now = (v.kind == LS_VAL_FLOAT) ? v.f : (float)v.i;
        now += item->delta;

        if (item->hi > item->lo) {
            if (now < item->lo) now = item->lo;
            if (now > item->hi) now = item->hi;
        }
        if (v.kind == LS_VAL_FLOAT) snprintf(text, sizeof(text), "%.2f", (double)now);
        else                        snprintf(text, sizeof(text), "%d", (int)now);
        return call_with_text(item->action, text, granted);
    }

    case LS_QUICK_TOGGLE: {
        if (!read_value(item, &v)) return LS_ACT_UNAVAILABLE;
        const bool on = (v.kind == LS_VAL_BOOL || v.kind == LS_VAL_INT)
                        ? (v.i != 0) : (v.f != 0.0f);
        snprintf(text, sizeof(text), "%s", on ? "false" : "true");
        return call_with_text(item->action, text, granted);
    }

    case LS_QUICK_CYCLE: {
        if (!item->choices || item->nchoices <= 0) return LS_ACT_BADARG;
        const int now = cycle_now(item);

        int next;
        if (now < 0)                next = 0;
        else if (item->cycle_back)  next = (now + item->nchoices - 1) % item->nchoices;
        else                        next = (now + 1) % item->nchoices;
        return call_with_text(item->action, item->choices[next], granted);
    }

    case LS_QUICK_ACTION:
    default:
        return call_with_text(item->action, item->choices && item->nchoices
                                            ? item->choices[0] : NULL, granted);
    }
}

/* ------------------------------------------------------------- drawing -- */

#define MAX_ITEMS 12

/* One box per control, with its name and value in the frame. */

#define BOX_H      7      /* the minimum: border, five rows, border  */
#define BOX_MAX_H 11      /* past this a button is not improved      */

/* LANDSCAPE GETS A STRIP, NOT A PANEL. */

#define BOX_H_WIDE 3

static int box_min(bool wide) { return wide ? BOX_H_WIDE : BOX_H; }
static int box_max(bool wide) { return wide ? BOX_H_WIDE : BOX_MAX_H; }

#define ONE_W      9      /* a single-direction box: 6.8 mm across    */
#define ACROSS_MAX 5      /* past five in a row it is a toolbar       */
#define MIN_HALF   8      /* below this a two-direction box will not fit */

/* The hit rect is BIGGER than the thing drawn, deliberately. */

typedef struct { int16_t which, x0, y0, x1, y1; } qhit_t;
static qhit_t s_hit[MAX_ITEMS * 2];
static int    s_hit_n;

static void hit_reset(void) { s_hit_n = 0; }

static void hit_add(int which, int x0, int y0, int x1, int y1)
{
    if (s_hit_n >= (int)(sizeof(s_hit) / sizeof(s_hit[0]))) return;
    qhit_t *h = &s_hit[s_hit_n++];
    h->which = (int16_t)which;
    h->x0 = (int16_t)x0; h->y0 = (int16_t)y0;
    h->x1 = (int16_t)x1; h->y1 = (int16_t)y1;
}

static int hit_at(int col, int row)
{
    for (int i = 0; i < s_hit_n; i++)
        if (col >= s_hit[i].x0 && col <= s_hit[i].x1 &&
            row >= s_hit[i].y0 && row <= s_hit[i].y1)
            return s_hit[i].which;
    return -1;
}

static bool two_way(const ls_quick_t *it)
{
    return it->kind == LS_QUICK_STEP || it->kind == LS_QUICK_CYCLE;
}

static void box(tui_surface *sf, tui_rect area, int x, int y, int w, int h,
                const char *title, const char *tail, uint8_t edge,
                uint8_t lab, uint8_t val)
{
    for (int i = 0; i < w; i++) {
        tui_put_char(sf, area, x + i, y, '-', edge);
        tui_put_char(sf, area, x + i, y + h - 1, '-', edge);
    }
    tui_put_char(sf, area, x, y, '+', edge);
    tui_put_char(sf, area, x + w - 1, y, '+', edge);
    tui_put_char(sf, area, x, y + h - 1, '+', edge);
    tui_put_char(sf, area, x + w - 1, y + h - 1, '+', edge);

    for (int r = 1; r < h - 1; r++) {
        tui_put_char(sf, area, x, y + r, '|', edge);
        tui_put_char(sf, area, x + w - 1, y + r, '|', edge);
    }

    if (title && *title) {
        const int n = (int)strlen(title);
        if (n + 4 <= w) {
            tui_put_char(sf, area, x + 1, y, ' ', edge);
            tui_put_str(sf, area, x + 2, y, title, lab);
            tui_put_char(sf, area, x + 2 + n, y, ' ', edge);
        }
    }
    if (tail && *tail) {
        const int n = (int)strlen(tail);
        if (n + 4 <= w) {
            tui_put_char(sf, area, x + w - n - 3, y, ' ', edge);
            tui_put_str(sf, area, x + w - n - 2, y, tail, val);
            tui_put_char(sf, area, x + w - 2, y, ' ', edge);
        }
    }
}

/* The direction, drawn at the size of the thing it is drawn on. */

#define FIG_W 7
#define FIG_H 5

/* Odd in both directions, so there is a true middle row and a true middle
   column to centre on. The even-sided version of these sat one row high in
   every box and that is most of what "nothing is centred" meant. */
static const char *const FIG_PLUS[FIG_H] = {
    "   X   ", "   X   ", "XXXXXXX", "   X   ", "   X   " };
static const char *const FIG_MINUS[FIG_H] = {
    "       ", "       ", "XXXXXXX", "       ", "       " };
/* Solid triangles, not arrows with a stem. */

static const char *const FIG_UP[FIG_H] = {
    "   X   ", "  XXX  ", " XXXXX ", "XXXXXXX", "       " };
static const char *const FIG_DOWN[FIG_H] = {
    "       ", "XXXXXXX", " XXXXX ", "  XXX  ", "   X   " };

/* Which figure a direction gets, or NULL when the sign is a word. */
static const char *const *figure_for(const ls_quick_t *it, bool up)
{
    switch (it->kind) {
    case LS_QUICK_STEP:  return up ? FIG_PLUS : FIG_MINUS;
    case LS_QUICK_CYCLE: return up ? FIG_UP   : FIG_DOWN;
    default:             return NULL;
    }
}

/* The half is a field, and the figure sits on it. */

static void draw_field(tui_surface *sf, tui_rect area, int x, int y,
                       int w, int h, uint8_t hue)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            tui_put_char(sf, area, x + c, y + r, LS_TUI_SHADE_25,
                         TUI_ATTR(hue, TUI_BLACK));
}

/* Draw the figure centred in the rect, scaled to fill it without spilling. */
static void draw_figure(tui_surface *sf, tui_rect area, int x, int y,
                        int w, int h, const char *const *fig, uint8_t at)
{
    if (!fig || w <= 0 || h <= 0) return;

    /* A figure that will not fit is drawn as a CHARACTER, not centred anyway. */

    if (h < FIG_H) {
        const char c = (fig == FIG_PLUS)  ? '+'
                     : (fig == FIG_MINUS) ? '-'
                     : (fig == FIG_UP)    ? '^'
                                          : 'v';
        tui_put_char(sf, area, x + (w - 1) / 2, y + (h - 1) / 2, c, at);
        return;
    }

    int sx = w / (FIG_W + 3);
    if (sx < 1) sx = 1;
    if (sx > 3) sx = 3;
    /* Vertical scale stays odd so the figure keeps a middle row; doubling it
       is only worth doing where there is room to spare either side. */
    int sy = (h >= FIG_H * 3) ? 3 : 1;
    if (FIG_H * sy > h) sy = 1;

    const int fw = FIG_W * sx, fh = FIG_H * sy;
    const int ox = x + (w - fw) / 2, oy = y + (h - fh) / 2;

    for (int r = 0; r < fh; r++)
        for (int c = 0; c < fw; c++)
            if (fig[r / sy][c / sx] == 'X')
                tui_put_char(sf, area, ox + c, oy + r, LS_TUI_SHADE_FULL, at);
}

static void draw_cap(tui_surface *sf, tui_rect area, int x, int y,
                     int w, int h, const char *word, uint8_t field,
                     uint8_t text)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            tui_put_char(sf, area, x + c, y + r, LS_TUI_SHADE_25, field);

    const int n = (int)strlen(word);
    const int tx = x + (w - n) / 2;
    const int ty = y + (h - 1) / 2;

    /* A space either side, so the lettering is not sitting in the texture. */
    if (tx - 1 >= x) tui_put_char(sf, area, tx - 1, ty, ' ', text);
    if (tx + n < x + w) tui_put_char(sf, area, tx + n, ty, ' ', text);
    tui_put_str(sf, area, tx, ty, word, text);
}

static const char *cap_word(const ls_quick_t *it, uint8_t *out_attr,
                            uint8_t on_at, uint8_t off_at, uint8_t plain,
                            int face_w)
{
    if (it->kind == LS_QUICK_TOGGLE) {
        const char *st = ls_quick_state(it);
        const bool on = st && (st[0] == 'o' || st[0] == 'O') &&
                        (st[1] == 'n' || st[1] == 'N');
        *out_attr = on ? on_at : off_at;
        return on ? "ON" : "OFF";
    }
    *out_attr = plain;

    /* An action's key says what the action IS. */

    if (it->label && *it->label && (int)strlen(it->label) + 2 <= face_w)
        return it->label;
    return "GO";
}

int ls_quick_rows(const ls_quick_t *items, int n, int width, bool wide)
{
    if (!items || n <= 0) return 0;
    if (n > MAX_ITEMS) n = MAX_ITEMS;

    /* The same walk the draw does. */

    int across = width / ONE_W;
    if (across < 1) across = 1;
    if (across > ACROSS_MAX) across = ACROSS_MAX;

    const int bmin = box_min(wide);

    /* In the compact strip a two-direction control does NOT own its
       row. It does in portrait, where 48 columns will not hold a label, a
       value and two halves beside anything else - but landscape has 115, and
       three controls each owning a three-row row is nine rows of a
       twenty-seven row screen for what fits in three. */
    int rows = 0, i = 0;
    while (i < n) {
        if (!wide && two_way(&items[i])) { rows += bmin; i++; continue; }
        int placed = 0;
        while (i < n && placed < across && (wide || !two_way(&items[i]))) {
            i++; placed++;
        }
        rows += bmin;
    }
    return rows;
}

static int group_count(const ls_quick_t *items, int n, int across, bool wide)
{
    int g = 0, i = 0;
    while (i < n) {
        if (!wide && two_way(&items[i])) { g++; i++; continue; }
        int placed = 0;
        while (i < n && placed < across && (wide || !two_way(&items[i]))) {
            i++; placed++;
        }
        g++;
    }
    return g;
}

int ls_quick_draw_posture(tui_surface *sf, tui_rect area, bool wide,
                          const ls_quick_t *items, int n)
{
    if (!sf || !items || n <= 0) return 0;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    if (area.h < box_min(wide) || area.w < ONE_W) return 0;

    hit_reset();

    const uint8_t lab  = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t dim = LS_ATTR_DIM;
    const uint8_t lit  = TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
    const uint8_t edge = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t on   = TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t off  = TUI_ATTR(TUI_RED, TUI_BLACK);

    int across = area.w / ONE_W;
    if (across < 1) across = 1;
    if (across > ACROSS_MAX) across = ACROSS_MAX;

    const int groups = group_count(items, n, across, wide);
    const int bmin = box_min(wide), bmax = box_max(wide);
    int bh = groups ? (area.h / groups) : bmin;
    if (bh < bmin) bh = bmin;
    if (bh > bmax) bh = bmax;

    /* Odd, so the interior is odd and whatever goes in it has one true
       middle row. An even interior has no centre and everything drawn in it
       sits a row above or a row below the middle - which is most of what
       "nothing is centred" was describing. */
    if (!(bh & 1)) {
        if (bh > bmin) bh--;
        else if (groups * (bh + 1) <= area.h) bh++;
    }

    int gap = 0;
    if (groups > 1) {
        gap = (area.h - groups * bh) / (groups - 1);
        if (gap < 0) gap = 0;
        if (gap > 1) gap = 1;
    }

    int y = area.y;
    int i = 0;

    /* ONE CONTROL PER ITERATION, and the row decides the slot. */

    while (i < n && y + bh <= area.y + area.h) {
        int here = 0;
        if (wide) {
            for (int k = i; k < n && here < across; k++) here++;
        } else if (two_way(&items[i])) {
            /* Portrait: 48 columns will not hold a label, a value and two
               halves beside anything else, so it owns the row. */
            here = 1;
        } else {
            for (int k = i; k < n && here < across && !two_way(&items[k]); k++)
                here++;
        }
        if (here < 1) break;

        const int step = (area.w - 1) / here;
        if (step < 4) break;

        for (int placed = 0; placed < here && i < n; placed++, i++) {
            const bool have = (action_index(items[i].action) >= 0);
            const uint8_t face = have ? lab : dim;
            const uint8_t frame = have ? edge : dim;

            const int x = area.x + placed * step;
            const int w = (placed == here - 1) ? (area.w - placed * step)
                                               : (step + 1);
            /* The drawn boxes share a border column, so the hit
               rects must NOT: two rects overlapping by a column would hand
               that column to whichever was added first, which is the left
               one always. Each takes its own step and the last takes the
               remainder, so the row divides exactly. */
            const int hx1 = (placed == here - 1) ? (area.x + area.w - 1)
                                                 : (x + step - 1);

            if (two_way(&items[i])) {
                const int half = (w - 1) / 2;
                if (half < MIN_HALF) { i = n; break; }

                box(sf, area, x, y, w, bh, items[i].label,
                    ls_quick_state(&items[i]), frame, face, have ? val : dim);

                /* The divider between the halves, so which one a thumb is
                   over is not a matter of judging the middle of a wide
                   box. */
                for (int r = 1; r < bh - 1; r++)
                    tui_put_char(sf, area, x + half, y + r, '|', frame);

                const bool lit_lo = (s_flash_target == i * 2)     && s_flash_ttl > 0;
                const bool lit_hi = (s_flash_target == i * 2 + 1) && s_flash_ttl > 0;

                const uint8_t f_lo = !have ? dim : lit_lo ? lit
                                     : TUI_ATTR(TUI_GREEN, TUI_BLACK);
                const uint8_t f_hi = !have ? dim : lit_hi ? lit
                                     : TUI_ATTR(TUI_GREEN, TUI_BLACK);
                draw_field(sf, area, x + 1, y + 1, half - 1, bh - 2, f_lo);
                draw_field(sf, area, x + half + 1, y + 1, w - half - 2,
                           bh - 2, f_hi);

                draw_figure(sf, area, x + 1, y + 1, half - 1, bh - 2,
                            figure_for(&items[i], false),
                            !have ? dim : lit_lo ? lit : face);
                draw_figure(sf, area, x + half + 1, y + 1, w - half - 2,
                            bh - 2, figure_for(&items[i], true),
                            !have ? dim : lit_hi ? lit : face);

                /* Edge to edge and top to bottom, splitting at the drawn
                   divider. The gap below belongs to this control too: the
                   next one's rect starts at its own top row, so there is
                   nothing to collide with and nothing left over. */
                hit_add(i * 2,     x, y, x + half - 1, y + bh - 1 + gap);
                hit_add(i * 2 + 1, x + half, y, hx1, y + bh - 1 + gap);
                continue;
            }

            uint8_t cap_at = have ? lab : dim;
            const char *word = cap_word(&items[i], &cap_at,
                                        have ? on : dim,
                                        have ? off : dim,
                                        have ? lab : dim,
                                        w - 2);

            /* A toggle's state is the big thing on the key, so repeating it
               in the border would be the same number twice on one control -
               which is the fault the first version of this panel had. */
            const char *tail = (items[i].kind == LS_QUICK_TOGGLE)
                             ? NULL : ls_quick_state(&items[i]);

            /*...and the same rule for a name the key has taken.
               Pointer equality, not strcmp: cap_word returns the label
               itself when it used it, and a copy of the word would be a
               second place for the two to drift apart. */
            const char *title = (word == items[i].label) ? NULL
                                                         : items[i].label;

            box(sf, area, x, y, w, bh, title, tail, frame, face,
                have ? val : dim);

            const bool k_lit = (s_flash_target == i * 2 + 1) && s_flash_ttl > 0;
            const uint8_t field = !have ? dim
                                : (items[i].kind == LS_QUICK_TOGGLE)
                                  ? cap_at : edge;
            draw_cap(sf, area, x + 1, y + 1, w - 2, bh - 2, word,
                     field, k_lit ? lit : cap_at);

            hit_add(i * 2 + 1, x, y, hx1, y + bh - 1 + gap);
        }

        y += bh + gap;
    }

    if (s_flash_ttl > 0 && --s_flash_ttl == 0) s_flash_target = -1;

    int used = y - area.y;

    if (s_say_ttl > 0) {
        if (--s_say_ttl == 0) s_say[0] = 0;
        if (s_say[0] && area.y + used < area.y + area.h) {
            tui_put_str(sf, area, area.x + 1, area.y + used, s_say,
                        TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
            used++;
        }
    }

    return used;
}

/* ------------------------------------------------------------- routing -- */

static bool fire(int which, const ls_quick_t *items, int n,
                 ls_cap_t granted, ls_act_status_t *out_status)
{
    if (which < 0) return false;
    const int i = which / 2;
    const bool up = (which & 1) != 0;
    if (i >= n) return false;

    ls_quick_t item = items[i];
    if (item.kind == LS_QUICK_STEP && !up) item.delta = -item.delta;
    if (item.kind == LS_QUICK_CYCLE && !up) item.cycle_back = true;

    const ls_act_status_t s = ls_quick_fire(&item, granted);
    if (out_status) *out_status = s;

    s_flash_target = which;
    s_flash_ttl = FLASH_FRAMES;

    /* Silence on success: a control that worked has already said so by the
       value in its frame changing. Only a refusal needs words. */
    if (s == LS_ACT_OK) {
        s_say[0] = 0;
        s_say_ttl = 0;
    } else {
        snprintf(s_say, sizeof(s_say), "%s: %s",
                 item.label ? item.label : "control", ls_act_status_str(s));
        s_say_ttl = SAY_FRAMES;
    }
    return true;
}

bool ls_quick_touch(int col, int row, const ls_quick_t *items, int n,
                    ls_cap_t granted, ls_act_status_t *out_status)
{
    if (!items || n <= 0) return false;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    return fire(hit_at(col, row), items, n, granted, out_status);
}

bool ls_quick_key(char ch, const ls_quick_t *items, int n,
                  ls_cap_t granted, ls_act_status_t *out_status)
{
    if (!items || n <= 0 || !ch) return false;
    if (n > MAX_ITEMS) n = MAX_ITEMS;

    const char want = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    for (int i = 0; i < n; i++) {
        if (items[i].key) {
            const char k = (items[i].key >= 'A' && items[i].key <= 'Z')
                           ? (char)(items[i].key + 32) : items[i].key;
            if (k == want) return fire(i * 2 + 1, items, n, granted, out_status);
        }
        if (items[i].key_down) {
            const char k = (items[i].key_down >= 'A' && items[i].key_down <= 'Z')
                           ? (char)(items[i].key_down + 32) : items[i].key_down;
            if (k == want) return fire(i * 2, items, n, granted, out_status);
        }
    }
    return false;
}
