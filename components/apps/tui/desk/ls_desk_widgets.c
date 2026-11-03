/* The desk's role table and widget set (DESKTOP_UI_DESIGN.md 2.1, 2.4).

   Faces are black ground in Bay 95; only the lines carry the 3D. Fills are
   one-row bands: title bars, the selection bar, key caps and tooltips. */

#include "ls_desk_priv.h"

#include <stdlib.h>
#include <string.h>

#include "../ls_braille.h"
#include "esp_heap_caps.h"

#define K  TUI_BLACK
#define KB (TUI_BLACK | TUI_BRIGHT)
#define W  TUI_WHITE
#define WB (TUI_WHITE | TUI_BRIGHT)
#define B  TUI_BLUE
#define C  TUI_CYAN
#define CB (TUI_CYAN | TUI_BRIGHT)
#define Y  TUI_YELLOW
#define YB (TUI_YELLOW | TUI_BRIGHT)
#define RB (TUI_RED | TUI_BRIGHT)
#define GB (TUI_GREEN | TUI_BRIGHT)
#define MB (TUI_MAGENTA | TUI_BRIGHT)

/* --------------------------------------------------------------- schemes -- */

/* A scheme names a dozen colours; every role follows from them, the way a
   Windows 95 scheme names its items and the controls follow. */
typedef struct {
    const char *name;
    uint8_t desk, desk_text;          /* the ground and the labels on it */
    uint8_t face_fg, face_bg;         /* windows and menus */
    uint8_t strong, hi, lo;           /* values; the bevel's light and dark lines */
    uint8_t ton_fg, ton_bg, toff_fg, toff_bg;
    uint8_t sel_fg, sel_bg;
    uint8_t key_fg, key_bg;           /* title keys, taskbar keys */
    uint8_t accel, tip_fg, tip_bg;
    uint8_t good, warn, bad, info, mesh;
    uint8_t stripe_fg, stripe_bg;
    uint8_t dis;
    const char *palette;              /* the TUI theme it is drawn in */
} scheme_t;

static const scheme_t SCHEMES[] = {
    {"Bay 95", K, W, W, K, WB, WB, KB, WB, B, W, KB, WB, B, WB, KB, YB, K, Y, GB, YB, RB, CB, MB, CB, B, W, "Terminal Bay"},
    {"Classic 95", C, WB, K, W, K, WB, KB, WB, B, W, KB, WB, B, K, WB, TUI_RED, K, Y,
     TUI_GREEN, TUI_RED, TUI_RED, TUI_BLUE, TUI_MAGENTA, CB, B, KB, "Terminal Bay"},
    {"High Contrast", K, WB, WB, K, YB, WB, WB, K, CB, WB, B, K, CB, K, WB, YB, K, YB, GB, YB, RB, CB, MB, K, CB, W, "Terminal Bay"},
    {"Teal", B, WB, K, W, K, WB, KB, WB, C, W, KB, WB, C, K, WB, TUI_RED, K, CB,
     TUI_GREEN, TUI_RED, TUI_RED, TUI_BLUE, TUI_MAGENTA, WB, C, KB, "Terminal Bay"},
    {"Amber phosphor", K, Y, Y, K, YB, YB, Y, K, YB, YB, K, K, Y, K, Y, WB, K, YB, YB, YB, RB, YB, YB, K, Y, Y, "Amber"},
    {"Night", B, WB, W, K, WB, CB, B, WB, TUI_MAGENTA, W, B, WB, TUI_MAGENTA, WB, B, CB, K, CB, GB, YB, RB, CB, MB,
     WB, TUI_MAGENTA, W, "Night"},
    /* The other TUI themes, each on the dark Bay layout with its own title colour. */
    {"Green phosphor", K, W, W, K, WB, WB, KB, K, GB, W, KB, K, GB, WB, KB, YB, K, GB, GB, YB, RB, CB, MB, CB, B, W, "Phosphor"},
    {"Ice", K, W, W, K, WB, WB, KB, WB, B, W, KB, WB, B, WB, KB, CB, K, CB, GB, YB, RB, CB, MB, CB, B, W, "Ice"},
    {"Synthwave", K, W, W, K, WB, WB, KB, WB, TUI_MAGENTA, W, KB, WB, TUI_MAGENTA, WB, KB, CB, K, MB, GB, YB, RB, CB, MB, MB, TUI_MAGENTA, W, "Synthwave"},
    {"Arcade", K, W, W, K, WB, WB, KB, WB, TUI_RED, W, KB, WB, TUI_RED, WB, KB, YB, K, YB, GB, YB, RB, CB, MB, YB, TUI_RED, W, "Arcade"},
    {"VFD", K, CB, CB, K, WB, CB, C, K, CB, C, K, K, CB, K, C, WB, K, CB, GB, YB, RB, CB, MB, K, C, C, "VFD"},
};
#define N_SCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))
_Static_assert(N_SCHEMES == LS_DESK_THEME_COUNT, "a scheme for every LS_DESK_THEME_ value");

int dk_scheme_count(void) { return N_SCHEMES; }
const char *dk_scheme_name(int i) { return i >= 0 && i < N_SCHEMES ? SCHEMES[i].name : "Custom"; }
const char *dk_scheme_palette(int i) { return i >= 0 && i < N_SCHEMES ? SCHEMES[i].palette : NULL; }

static const char *const ITEMS[DK_IT_COUNT] = {
    "Desktop", "Title bar, active", "Title bar, inactive", "Menu", "Selected items", "Window",
    "Button face", "Tooltip",
};
const char *dk_item_name(int it) { return it >= 0 && it < DK_IT_COUNT ? ITEMS[it] : ""; }

/* Light grounds (silver, white, yellow, cyan) take dark hues; dark ones bright. */
static bool light(uint8_t c) { return c == W || c == WB || c == CB || c == YB || c == Y || c == C; }

static void fill_roles(uint8_t r[R_COUNT][2], const dk_prefs_t *p)
{
    const scheme_t *s = &SCHEMES[p->scheme < N_SCHEMES ? p->scheme : 0];
    uint8_t face_fg = s->face_fg, face_bg = s->face_bg;
    if (p->mask & (1u << DK_IT_WINDOW)) { face_fg = p->fg[DK_IT_WINDOW]; face_bg = p->bg[DK_IT_WINDOW]; }
    #define SET(role, f, b) do { r[role][0] = (f); r[role][1] = (b); } while (0)
    SET(R_DESK, s->desk_text, s->desk);          SET(R_DESK_TEXT, s->desk_text, s->desk);
    SET(R_FACE, face_fg, face_bg);               SET(R_STRONG, s->strong, face_bg);
    SET(R_HILITE, s->hi, face_bg);               SET(R_SHADOW, s->lo, face_bg);
    SET(R_TITLE_ON, s->ton_fg, s->ton_bg);       SET(R_TITLE_OFF, s->toff_fg, s->toff_bg);
    SET(R_SELECT, s->sel_fg, s->sel_bg);         SET(R_SELECT_DIM, s->sel_fg, s->toff_bg);
    SET(R_KEY, s->key_fg, s->key_bg);            SET(R_KEY_DOWN, s->strong, face_bg);
    SET(R_ACCEL, s->accel, face_bg);             SET(R_CLOSE, RB, s->key_bg);
    SET(R_TIP, s->tip_fg, s->tip_bg);
    SET(R_GOOD, s->good, face_bg); SET(R_WARN, s->warn, face_bg); SET(R_BAD, s->bad, face_bg);
    SET(R_INFO, s->info, face_bg); SET(R_MESH, s->mesh, face_bg);
    SET(R_DISABLED, s->dis, face_bg);            SET(R_STRIPE, s->stripe_fg, s->stripe_bg);
    SET(R_TASKKEY, s->key_fg, s->key_bg);
    SET(R_TASKHOVER, light(s->key_bg) ? WB : K, light(s->key_bg) ? KB : W);
    SET(R_TRAY, face_fg, face_bg);               SET(R_MENU, face_fg, face_bg);
    /* The owner's own colours for each item, over the scheme. */
    if (p->mask & (1u << DK_IT_DESKTOP)) {
        r[R_DESK][1] = r[R_DESK_TEXT][1] = p->bg[DK_IT_DESKTOP];
        r[R_DESK][0] = r[R_DESK_TEXT][0] = p->fg[DK_IT_DESKTOP];
    }
    if (p->mask & (1u << DK_IT_TITLE_ON)) SET(R_TITLE_ON, p->fg[DK_IT_TITLE_ON], p->bg[DK_IT_TITLE_ON]);
    if (p->mask & (1u << DK_IT_TITLE_OFF)) SET(R_TITLE_OFF, p->fg[DK_IT_TITLE_OFF], p->bg[DK_IT_TITLE_OFF]);
    if (p->mask & (1u << DK_IT_MENU)) SET(R_MENU, p->fg[DK_IT_MENU], p->bg[DK_IT_MENU]);
    if (p->mask & (1u << DK_IT_SELECT)) SET(R_SELECT, p->fg[DK_IT_SELECT], p->bg[DK_IT_SELECT]);
    if (p->mask & (1u << DK_IT_BUTTON)) {
        SET(R_TASKKEY, p->fg[DK_IT_BUTTON], p->bg[DK_IT_BUTTON]);
        SET(R_KEY, p->fg[DK_IT_BUTTON], p->bg[DK_IT_BUTTON]);
        r[R_CLOSE][1] = p->bg[DK_IT_BUTTON];
    }
    if (p->mask & (1u << DK_IT_TIP)) SET(R_TIP, p->fg[DK_IT_TIP], p->bg[DK_IT_TIP]);
    #undef SET
}

static uint8_t s_roles[R_COUNT][2];
static dk_prefs_t s_prefs = {.scheme = 0, .live_drag = true, .anim = true, .labels = true};
static bool s_roles_ok;

const dk_prefs_t *dk_prefs(void) { return &s_prefs; }

void dk_prefs_set(const dk_prefs_t *p)
{
    s_prefs = *p;
    if (s_prefs.scheme >= N_SCHEMES) s_prefs.scheme = 0;
    fill_roles(s_roles, &s_prefs);
    s_roles_ok = true;
}

void dk_item_colors(const dk_prefs_t *p, int item, uint8_t *fg, uint8_t *bg)
{
    uint8_t r[R_COUNT][2];
    fill_roles(r, p);
    static const uint8_t ROLE_OF[DK_IT_COUNT] = {R_DESK, R_TITLE_ON, R_TITLE_OFF, R_MENU, R_SELECT, R_FACE, R_TASKKEY, R_TIP};
    const int role = item >= 0 && item < DK_IT_COUNT ? ROLE_OF[item] : R_FACE;
    *fg = r[role][0];
    *bg = r[role][1];
}

uint8_t dk_attr_with(const dk_prefs_t *p, int role)
{
    uint8_t r[R_COUNT][2];
    fill_roles(r, p);
    if (role < 0 || role >= R_COUNT) role = R_FACE;
    return TUI_ATTR(r[role][0], r[role][1]);
}

uint8_t dk_attr(int role)
{
    if (!s_roles_ok) {
        fill_roles(s_roles, &s_prefs);
        s_roles_ok = true;
    }
    if (role < 0 || role >= R_COUNT) role = R_FACE;
    return TUI_ATTR(s_roles[role][0], s_roles[role][1]);
}

uint8_t dk_face_bg(void) { return TUI_ATTR_BG(dk_attr(R_FACE)); }

bool dk_light_face(void) { return light(dk_face_bg()); }
bool dk_light_desk(void) { return light(TUI_ATTR_BG(dk_attr(R_DESK))); }

uint8_t dk_hue(uint8_t hue)
{
    uint8_t fg = hue & 15;
    if (!dk_light_face()) {
        if (fg != TUI_BLACK && fg != TUI_WHITE) fg |= TUI_BRIGHT;   /* hues read on dark faces when bright */
        if (fg == TUI_WHITE) fg = WB;
    } else {
        fg &= 7;                                                   /* and on light ones when not */
        if (fg == TUI_WHITE || fg == TUI_YELLOW) fg = fg == TUI_YELLOW ? TUI_RED : TUI_BLACK;
    }
    return TUI_ATTR(fg, dk_face_bg());
}

void *dk_psram(size_t bytes)
{
    return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void dk_psram_free(void *p) { heap_caps_free(p); }

/* ---------------------------------------------------------------- text -- */

static int utf8_cp(const char *s, int *len)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *len = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80) { *len = 2; return (u[0] & 31) << 6 | (u[1] & 63); }
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80) {
        *len = 3;
        return (u[0] & 15) << 12 | (u[1] & 63) << 6 | (u[2] & 63);
    }
    *len = 1;
    return -1;
}

/* A codepoint the desk writes in its own strings, as the cell that draws it. */
static int box_cell(int cp)
{
    if (cp == 0x00D7) return LS_SHAPE_TIMES;
    if (cp == 0x25E2) return 0x10000 | (uint8_t)LS_TUI_SEXT(0x38);   /* the resize grip */
    if (ls_box_is(cp)) return cp;
    return 0;
}

int dk_text(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t attr)
{
    if (!s) return 0;
    int n = 0;
    while (*s) {
        int len = 1;
        const int cp = utf8_cp(s, &len);
        const int bc = cp >= 0x80 ? box_cell(cp) : 0;
        if (bc & 0x10000) {
            tui_put_char(sf, clip, x + n, y, (char)(bc & 0xFF), attr);
            s += len;
        } else if (bc) {
            ls_box_put(sf, clip, x + n, y, bc, attr);
            s += len;
        } else {
            const char c = tui_str_cell(&s);
            tui_put_char(sf, clip, x + n, y, c, attr);
        }
        n++;
    }
    return n;
}

int dk_text_len(const char *s)
{
    if (!s) return 0;
    int n = 0;
    while (*s) {
        int len = 1;
        utf8_cp(s, &len);
        s += len;
        n++;
    }
    return n;
}

int dk_text_accel(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t attr, uint8_t accel)
{
    if (!s || !*s) return 0;
    const char first[2] = {s[0], 0};
    dk_text(sf, clip, x, y, first, (uint8_t)((attr & 0xF0) | (accel & 0x0F)));
    return 1 + dk_text(sf, clip, x + 1, y, s + 1, attr);
}

void dk_fill(tui_surface *sf, tui_rect r, uint8_t attr)
{
    tui_fill(sf, tui_rect_intersect(r, tui_surface_rect(sf)), ' ', attr);
}

void dk_hline(tui_surface *sf, tui_rect clip, int x, int y, int len, int cp, uint8_t attr)
{
    for (int i = 0; i < len; i++) ls_box_put(sf, clip, x + i, y, cp, attr);
}

void dk_vline(tui_surface *sf, tui_rect clip, int x, int y, int len, int cp, uint8_t attr)
{
    for (int i = 0; i < len; i++) ls_box_put(sf, clip, x, y + i, cp, attr);
}

/* An edge glyph in a border cell: the line in `role`'s colour on the ground
   already there, which is whatever lies outside the frame (the desk, or the
   window behind), so no face colour shows past the line. */
void dk_edge_put(tui_surface *sf, tui_rect clip, int x, int y, int cp, int role)
{
    clip = tui_rect_intersect(clip, tui_surface_rect(sf));
    if (!tui_rect_contains(clip, x, y)) return;
    tui_cell *c = &sf->back[(size_t)y * sf->w + x];
    c->ch = (int16_t)cp;
    c->attr = (uint8_t)((c->attr & 0xF0) | (dk_attr(role) & 0x0F));
}

/* The frame of r on the OUTER edges of its border cells, so the panel is
   solid face out to its line and touches whatever is beside it: no face
   past the line, and no strip of ground inside it either. Raised: top and
   left HILITE, bottom and right SHADOW (the top-right and bottom-right
   corners dark, the bottom-left light, as Windows 95 draws them); sunken
   the reverse. The caller fills all of r with the face first: border cells
   keep that ground and take only the line's colour. */
static void edge_frame(tui_surface *sf, tui_rect clip, tui_rect r, int hi, int lo)
{
    if (r.w < 2 || r.h < 2) return;
    for (int i = 1; i < r.w - 1; i++) {
        dk_edge_put(sf, clip, r.x + i, r.y, LS_EDGE_T, hi);
        dk_edge_put(sf, clip, r.x + i, r.y + r.h - 1, LS_EDGE_B, lo);
    }
    for (int i = 1; i < r.h - 1; i++) {
        dk_edge_put(sf, clip, r.x, r.y + i, LS_EDGE_L, hi);
        dk_edge_put(sf, clip, r.x + r.w - 1, r.y + i, LS_EDGE_R, lo);
    }
    dk_edge_put(sf, clip, r.x, r.y, LS_EDGE_TL, hi);
    dk_edge_put(sf, clip, r.x + r.w - 1, r.y, LS_EDGE_TR, lo);
    dk_edge_put(sf, clip, r.x, r.y + r.h - 1, LS_EDGE_BL, hi);
    dk_edge_put(sf, clip, r.x + r.w - 1, r.y + r.h - 1, LS_EDGE_BR, lo);
}

void dk_frame(tui_surface *sf, tui_rect clip, tui_rect r, bool raised)
{
    edge_frame(sf, clip, r, raised ? R_HILITE : R_SHADOW, raised ? R_SHADOW : R_HILITE);
}

tui_rect dk_inner(tui_rect r) { return tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2); }

void dk_frame_double(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t attr)
{
    if (r.w < 2 || r.h < 2) return;
    dk_hline(sf, clip, r.x + 1, r.y, r.w - 2, LS_BOX_DBL_H, attr);
    dk_hline(sf, clip, r.x + 1, r.y + r.h - 1, r.w - 2, LS_BOX_DBL_H, attr);
    dk_vline(sf, clip, r.x, r.y + 1, r.h - 2, LS_BOX_DBL_V, attr);
    dk_vline(sf, clip, r.x + r.w - 1, r.y + 1, r.h - 2, LS_BOX_DBL_V, attr);
    ls_box_put(sf, clip, r.x, r.y, LS_BOX_DBL_DR, attr);
    ls_box_put(sf, clip, r.x + r.w - 1, r.y, LS_BOX_DBL_DL, attr);
    ls_box_put(sf, clip, r.x, r.y + r.h - 1, LS_BOX_DBL_UR, attr);
    ls_box_put(sf, clip, r.x + r.w - 1, r.y + r.h - 1, LS_BOX_DBL_UL, attr);
}

/* The outline drag and resize: drawn over whatever is there, ground kept. */
void dk_frame_dashed(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t attr)
{
    if (r.w < 2 || r.h < 2) return;
    clip = tui_rect_intersect(clip, tui_surface_rect(sf));
    const uint8_t fg = attr & 0x0F;
    #define DPUT(xx, yy, cp) do { if (tui_rect_contains(clip, (xx), (yy))) { \
        tui_cell *c_ = &sf->back[(yy) * sf->w + (xx)]; \
        c_->ch = (int16_t)(cp); c_->attr = (uint8_t)((c_->attr & 0xF0) | fg); } } while (0)
    for (int i = 1; i < r.w - 1; i++) { DPUT(r.x + i, r.y, LS_BOX_DASH_H); DPUT(r.x + i, r.y + r.h - 1, LS_BOX_DASH_H); }
    for (int i = 1; i < r.h - 1; i++) { DPUT(r.x, r.y + i, LS_BOX_DASH_V); DPUT(r.x + r.w - 1, r.y + i, LS_BOX_DASH_V); }
    DPUT(r.x, r.y, LS_BOX_DR);
    DPUT(r.x + r.w - 1, r.y, LS_BOX_DL);
    DPUT(r.x, r.y + r.h - 1, LS_BOX_UR);
    DPUT(r.x + r.w - 1, r.y + r.h - 1, LS_BOX_UL);
    #undef DPUT
}

/* The drag outline and the zoom trail: dashes over whatever is there, the
   ground kept, their gaps stepped along by `phase` so the line marches as
   the pointer (or the animation) moves, and stands still when it stops. */
void dk_frame_march(tui_surface *sf, tui_rect clip, tui_rect r, uint8_t attr, unsigned phase)
{
    if (r.w < 2 || r.h < 2) return;
    clip = tui_rect_intersect(clip, tui_surface_rect(sf));
    const uint8_t fg = attr & 0x0F;
    const int per = 2 * (r.w + r.h) - 4;
    for (int i = 0; i < per; i++) {
        int x, y, cp;
        if (i < r.w) { x = r.x + i; y = r.y; cp = i == 0 ? LS_BOX_DR : i == r.w - 1 ? LS_BOX_DL : LS_BOX_DASH_H; }
        else if (i < r.w + r.h - 1) { x = r.x + r.w - 1; y = r.y + i - r.w + 1; cp = y == r.y + r.h - 1 ? LS_BOX_UL : LS_BOX_DASH_V; }
        else if (i < 2 * r.w + r.h - 2) { x = r.x + r.w - 1 - (i - r.w - r.h + 2); y = r.y + r.h - 1; cp = x == r.x ? LS_BOX_UR : LS_BOX_DASH_H; }
        else { x = r.x; y = r.y + r.h - 1 - (i - 2 * r.w - r.h + 3); cp = LS_BOX_DASH_V; }
        if (((unsigned)i + phase) % 6 == 5 && cp != LS_BOX_DR && cp != LS_BOX_DL && cp != LS_BOX_UR && cp != LS_BOX_UL) continue;
        if (!tui_rect_contains(clip, x, y)) continue;
        tui_cell *c = &sf->back[(size_t)y * sf->w + x];
        c->ch = (int16_t)cp;
        c->attr = (uint8_t)((c->attr & 0xF0) | fg);
    }
}

void dk_key(tui_surface *sf, tui_rect clip, int x, int y, int cp, uint8_t glyph_attr)
{
    const uint8_t key = dk_attr(R_KEY);
    tui_put_char(sf, clip, x, y, '[', key);
    if (cp < 0x80) tui_put_char(sf, clip, x + 1, y, (char)cp, glyph_attr ? glyph_attr : key);
    else ls_box_put(sf, clip, x + 1, y, cp, glyph_attr ? glyph_attr : key);
    tui_put_char(sf, clip, x + 2, y, ']', key);
}

void dk_button(tui_surface *sf, tui_rect clip, tui_rect r, const char *label, bool is_default, bool pressed)
{
    if (r.w < 4 || r.h < 3) return;
    dk_fill(sf, tui_rect_intersect(r, clip), dk_attr(R_FACE));
    /* The default button's frame is the strong colour all round, as the
       dark outline of a Windows 95 default button. */
    if (is_default) edge_frame(sf, clip, r, R_STRONG, R_STRONG);
    else dk_frame(sf, clip, r, !pressed);
    const int n = dk_text_len(label);
    const int x = r.x + (r.w - n) / 2 + (pressed ? 1 : 0);
    dk_text_accel(sf, clip, x, r.y + r.h / 2, label, dk_attr(is_default ? R_STRONG : R_FACE), dk_attr(R_ACCEL));
}

void dk_check(tui_surface *sf, tui_rect clip, int x, int y, bool on, const char *label)
{
    tui_put_char(sf, clip, x, y, '[', dk_attr(R_SHADOW));
    tui_put_char(sf, clip, x + 1, y, on ? 'x' : ' ', dk_attr(R_STRONG));
    tui_put_char(sf, clip, x + 2, y, ']', dk_attr(R_HILITE));
    if (label) dk_text_accel(sf, clip, x + 4, y, label, dk_attr(R_FACE), dk_attr(R_ACCEL));
}

void dk_radio(tui_surface *sf, tui_rect clip, int x, int y, bool on, const char *label)
{
    tui_put_char(sf, clip, x, y, '(', dk_attr(R_SHADOW));
    if (on) ls_box_put(sf, clip, x + 1, y, LS_SHAPE_DOT, dk_attr(R_STRONG));
    else tui_put_char(sf, clip, x + 1, y, ' ', dk_attr(R_FACE));
    tui_put_char(sf, clip, x + 2, y, ')', dk_attr(R_HILITE));
    if (label) dk_text_accel(sf, clip, x + 4, y, label, dk_attr(R_FACE), dk_attr(R_ACCEL));
}

void dk_meter(tui_surface *sf, tui_rect clip, int x, int y, int w, int permille, uint8_t hue)
{
    if (w < 3) return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    const int in = w - 2, on = (in * permille + 500) / 1000;
    tui_put_char(sf, clip, x, y, '[', dk_attr(R_FACE));
    for (int i = 0; i < in; i++)
        tui_put_char(sf, clip, x + 1 + i, y, i < on ? '=' : '-', i < on ? dk_hue(hue) : dk_attr(R_SHADOW));
    tui_put_char(sf, clip, x + w - 1, y, ']', dk_attr(R_FACE));
}

/* Two samples a cell, each a column of 0..4 dots from the bottom. */
void dk_spark(tui_surface *sf, tui_rect clip, int x, int y, const int *v, int n, int max, uint8_t attr)
{
    if (max <= 0) max = 1;
    for (int i = 0; i + 1 < n + 1; i += 2) {
        unsigned bits = 0x2800;
        for (int side = 0; side < 2 && i + side < n; side++) {
            int lv = (v[i + side] * 4 + max - 1) / max;
            if (lv < 1 && v[i + side] > 0) lv = 1;
            if (lv > 4) lv = 4;
            bits |= ls_braille_column((unsigned)lv, side);
        }
        ls_braille_put(sf, clip, x + i / 2, y, (int16_t)bits, attr);
    }
}
