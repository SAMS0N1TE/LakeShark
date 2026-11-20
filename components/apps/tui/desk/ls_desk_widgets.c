/* The desk's role table and widget set (DESKTOP_UI_DESIGN.md 2.1, 2.4).

   Faces are black ground in Bay 95; only the lines carry the 3D. Fills are
   one-row bands: title bars, the selection bar, key caps and tooltips. */

#include "ls_desk_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../ls_braille.h"
#include "../ls_theme.h"
#include "esp_attr.h"
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
    {"Classic 95", C, WB, K, W, K, WB, KB, WB, B, W, KB, WB, B, K, WB, TUI_RED, K, YB,
     TUI_GREEN, TUI_YELLOW, TUI_RED, TUI_BLUE, TUI_MAGENTA, CB, B, KB, "Classic"},
    {"High Contrast", K, WB, WB, K, YB, WB, WB, K, CB, WB, B, K, CB, K, WB, YB, K, YB, GB, YB, RB, CB, MB, K, CB, W, "Terminal Bay"},
    {"Teal", B, WB, K, W, K, WB, KB, WB, C, W, KB, WB, C, K, WB, TUI_RED, K, CB,
     TUI_GREEN, TUI_YELLOW, TUI_RED, TUI_BLUE, TUI_MAGENTA, WB, C, KB, "Classic"},
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

/* The light schemes' own palette: Terminal Bay's greys and brights, and six
   plain hues dark enough to be text on silver (4.7 to 1 and better), the way
   the sixteen VGA colours gave Windows 95 its navy and maroon. It is not in
   the TUI's theme cycle: the desk sets it, and takes it away when it goes. */
static const ls_tui_theme_t s_theme_classic = {
    .name = "Classic",
    .desc = "silver faces and dark inks, for the desk's light schemes",
    .palette = {0x0000, 0x80A2, 0x0A43, 0x59E0, 0x1191, 0x68CF, 0x0249, 0xAD97,
                0x3A09, 0xFB4D, 0x6F0F, 0xFE4A, 0x6DBF, 0xDC5F, 0x5F1C, 0xF7BF},
};
const ls_tui_theme_t *dk_scheme_theme(int i)
{
    const char *name = dk_scheme_palette(i);
    if (!name) return NULL;
    return !strcmp(name, s_theme_classic.name) ? &s_theme_classic : ls_tui_theme_by_name(name);
}
bool dk_theme_own(const ls_tui_theme_t *t) { return t == &s_theme_classic; }

static const char *const ITEMS[DK_IT_COUNT] = {
    "Desktop", "Title bar, active", "Title bar, inactive", "Menu", "Selected items", "Window",
    "Button face", "Tooltip",
};
const char *dk_item_name(int it) { return it >= 0 && it < DK_IT_COUNT ? ITEMS[it] : ""; }

/* Light grounds (silver, white, a bright yellow) take dark hues; dark ones
   bright. By the colour itself in the palette in use: the light schemes'
   cyan is a dark teal, and Daylight's black is white. */
static bool cr_build(void);
EXT_RAM_BSS_ATTR static uint16_t s_light;      /* a bit per colour: black reads on it better than white */
static bool light(uint8_t c)
{
    if (cr_build()) return (s_light >> (c & 15)) & 1;
    return c == W || c == WB || c == CB || c == YB || c == Y || c == C;
}

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

static uint8_t s_roles[R_COUNT][2];            /* as the scheme names them */
EXT_RAM_BSS_ATTR static uint8_t s_ink[R_COUNT];   /* as they are drawn: made to read in the palette in use */
static dk_prefs_t s_prefs = {.scheme = 0, .live_drag = true, .anim = true, .labels = true};
static bool s_roles_ok;

/* ------------------------------------------------------------------ inks -- */

/* The role table names colours; whether one reads depends on the palette in
   use (a scheme's own, or Daylight over it) and on the owner's own Window
   colours. So every role is checked against the real colours once per
   palette: text wants 4.5 to 1 on its ground, and where the named colour
   has less, its bright (or plain) twin is taken, else the strong ink, else
   black or white. Everything drawn through dk_attr reads. */
EXT_RAM_BSS_ATTR static uint8_t s_cr[16][16];  /* WCAG contrast x 10 of every pair */
static const ls_tui_theme_t *s_cr_for;

static float lin(int v8)
{
    const float c = (float)v8 / 255.0f;
    return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static const ls_tui_theme_t *palette_now(void)
{
    const ls_tui_theme_t *t = ls_tui_active_theme();
    return t ? t : ls_tui_get_theme();
}

static bool cr_build(void)
{
    const ls_tui_theme_t *t = palette_now();
    if (!t) return false;
    if (t == s_cr_for) return true;
    s_cr_for = t;
    float lum[16];
    for (int i = 0; i < 16; i++) {
        const uint16_t p = t->palette[i];
        lum[i] = 0.2126f * lin((p >> 11) * 255 / 31) + 0.7152f * lin(((p >> 5) & 63) * 255 / 63) + 0.0722f * lin((p & 31) * 255 / 31);
    }
    s_light = 0;
    for (int i = 0; i < 16; i++) if (lum[i] > 0.18f) s_light |= (uint16_t)(1u << i);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++) {
            const float hi = lum[i] > lum[j] ? lum[i] : lum[j], lo = lum[i] > lum[j] ? lum[j] : lum[i];
            const float r = (hi + 0.05f) / (lo + 0.05f) * 10.0f;
            s_cr[i][j] = (uint8_t)(r > 250.0f ? 250 : (int)r);
        }
    return true;
}

int dk_contrast(uint8_t fg, uint8_t bg) { return cr_build() ? s_cr[fg & 15][bg & 15] : 0; }

uint8_t dk_fit(uint8_t fg, uint8_t bg, int min)
{
    fg &= 15; bg &= 15;
    if (!cr_build() || s_cr[fg][bg] >= min) return TUI_ATTR(fg, bg);
    const uint8_t twin = (uint8_t)(fg ^ TUI_BRIGHT);
    if (s_cr[twin][bg] >= min) return TUI_ATTR(twin, bg);
    const uint8_t strong = s_roles[R_STRONG][0] & 15;
    if ((s_roles[R_STRONG][1] & 15) == bg && s_cr[strong][bg] >= min) return TUI_ATTR(strong, bg);
    return TUI_ATTR(s_cr[WB][bg] >= s_cr[K][bg] ? WB : K, bg);
}

static void fit_roles(void)
{
    /* what each role's ink must have against its own ground, x 10; 0: a line or a ground, left alone */
    static const uint8_t NEED[R_COUNT] = {
        [R_DESK] = 45, [R_DESK_TEXT] = 45, [R_FACE] = 45, [R_STRONG] = 45, [R_TITLE_ON] = 45, [R_TITLE_OFF] = 45,
        [R_SELECT] = 45, [R_SELECT_DIM] = 45, [R_KEY] = 45, [R_KEY_DOWN] = 45, [R_ACCEL] = 45, [R_CLOSE] = 30,
        [R_TIP] = 45, [R_GOOD] = 45, [R_WARN] = 45, [R_BAD] = 45, [R_INFO] = 45, [R_MESH] = 45, [R_STRIPE] = 45,
        [R_TASKKEY] = 45, [R_TASKHOVER] = 45, [R_TRAY] = 45, [R_MENU] = 45,
    };
    for (int r = 0; r < R_COUNT; r++)
        s_ink[r] = NEED[r] ? dk_fit(s_roles[r][0], s_roles[r][1], NEED[r]) : TUI_ATTR(s_roles[r][0], s_roles[r][1]);
    if (!cr_build()) return;
    /* A band no ink reads on (a dim palette's title colour: Night's is red on
       red): the best ink there is, on the band or on its other weight. */
    for (int r = 0; r < R_COUNT; r++) {
        uint8_t bg = TUI_ATTR_BG(s_ink[r]), best = TUI_ATTR_FG(s_ink[r]);
        if (!NEED[r] || s_cr[best][bg] >= NEED[r] || bg == (s_roles[R_FACE][1] & 15)) continue;
        for (int pass = 0; pass < 2 && s_cr[best][bg] < NEED[r]; pass++) {
            const uint8_t g = pass ? (uint8_t)(bg ^ TUI_BRIGHT) : bg;
            uint8_t top = 0;
            for (uint8_t k = 1; k < 16; k++) if (s_cr[k][g] > s_cr[top][g]) top = k;
            if (s_cr[top][g] > s_cr[best][bg]) { best = top; bg = g; }
        }
        s_ink[r] = TUI_ATTR(best, bg);
    }
    /* Greyed text: apart from the text and the values, and still there. A
       scheme of one ink has no such colour: its dashes and brackets say it. */
    const uint8_t bg = s_roles[R_FACE][1] & 15, face = TUI_ATTR_FG(s_ink[R_FACE]), strong = TUI_ATTR_FG(s_ink[R_STRONG]);
    const uint8_t tries[] = {s_roles[R_DISABLED][0], s_roles[R_SHADOW][0], KB, W};
    s_ink[R_DISABLED] = TUI_ATTR(face, bg);
    for (unsigned i = 0; i < sizeof(tries); i++) {
        const uint8_t d = tries[i] & 15;
        if (d == face || d == strong || s_cr[d][bg] < 18 || s_cr[d][bg] >= s_cr[face][bg]) continue;
        s_ink[R_DISABLED] = TUI_ATTR(d, bg);
        break;
    }
}

static void roles_build(void)
{
    fill_roles(s_roles, &s_prefs);
    fit_roles();
    s_roles_ok = true;
}

/* Once a frame: the palette may have changed under the roles (Daylight, a
   theme chosen in Settings). */
void dk_roles_sync(void)
{
    if (!s_roles_ok || palette_now() != s_cr_for) roles_build();
}

const dk_prefs_t *dk_prefs(void) { return &s_prefs; }

void dk_prefs_set(const dk_prefs_t *p)
{
    s_prefs = *p;
    if (s_prefs.scheme >= N_SCHEMES) s_prefs.scheme = 0;
    roles_build();
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
    if (!s_roles_ok) roles_build();
    if (role < 0 || role >= R_COUNT) role = R_FACE;
    return s_ink[role];
}

uint8_t dk_face_bg(void) { return TUI_ATTR_BG(dk_attr(R_FACE)); }

bool dk_light_face(void) { return light(dk_face_bg()); }
bool dk_light_desk(void) { return light(TUI_ATTR_BG(dk_attr(R_DESK))); }

/* A TUI colour as ink on the face: bright on a dark face and plain on a
   light one, then made to read like any role. White and black are the
   strong ink. dk_hue is text; dk_art is line art, a lamp, a meter's fill. */
static uint8_t hue_on_face(uint8_t hue, int min)
{
    const uint8_t fg = hue & 7;
    if (fg == TUI_BLACK || fg == TUI_WHITE) return dk_attr(R_STRONG);
    return dk_fit(dk_light_face() ? fg : (uint8_t)(fg | TUI_BRIGHT), dk_face_bg(), min);
}
uint8_t dk_hue(uint8_t hue) { return hue_on_face(hue, 45); }
uint8_t dk_art(uint8_t hue) { return hue_on_face(hue, 30); }

/* The accelerator letter on a ground: the scheme's colour for it when that
   reads there and differs from the text, else the letter in reverse. */
uint8_t dk_accel_on(uint8_t attr)
{
    const uint8_t fg = TUI_ATTR_FG(attr), bg = TUI_ATTR_BG(attr), a = TUI_ATTR_FG(dk_attr(R_ACCEL));
    if (!cr_build()) return TUI_ATTR(a, bg);
    if (a != fg && s_cr[a][bg] >= 45) return TUI_ATTR(a, bg);
    const uint8_t twin = (uint8_t)(a ^ TUI_BRIGHT);
    if (twin != fg && s_cr[twin][bg] >= 45) return TUI_ATTR(twin, bg);
    return TUI_ATTR(bg, fg);
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

void dk_radio(tui_surface *sf, tui_rect clip, int x, int y, bool on, const char *label)
{
    tui_put_char(sf, clip, x, y, '(', dk_attr(R_SHADOW));
    if (on) ls_box_put(sf, clip, x + 1, y, LS_SHAPE_DOT, dk_attr(R_STRONG));
    else tui_put_char(sf, clip, x + 1, y, ' ', dk_attr(R_FACE));
    tui_put_char(sf, clip, x + 2, y, ')', dk_attr(R_HILITE));
    if (label) dk_label(sf, clip, x + 4, y, label, dk_attr(R_FACE), true);
}

void dk_meter(tui_surface *sf, tui_rect clip, int x, int y, int w, int permille, uint8_t hue)
{
    if (w < 3) return;
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    const int in = w - 2, on = (in * permille + 500) / 1000;
    tui_put_char(sf, clip, x, y, '[', dk_attr(R_FACE));
    for (int i = 0; i < in; i++)
        tui_put_char(sf, clip, x + 1 + i, y, i < on ? '=' : '-', i < on ? dk_art(hue) : dk_attr(R_SHADOW));
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

/* ------------------------------------------------- desktop-pass widgets -- */

/* Everything below is stateless: the caller keeps the selection, the scroll
   position and the focus, draws with these and asks the _hit functions what
   a click landed on. Hover comes from dk_hover(rect) (ls_desk.c). */

static uint8_t on_bg(uint8_t ground, int role) { return (uint8_t)((ground & 0xF0) | (dk_attr(role) & 0x0F)); }

static void put_cp(tui_surface *sf, tui_rect clip, int x, int y, int cp, uint8_t attr)
{
    if (cp < 0x80) tui_put_char(sf, clip, x, y, (char)cp, attr);
    else ls_box_put(sf, clip, x, y, cp, attr);
}

/* "&Open", "Save &as": the letter after & is the accelerator; "&&" and
   "& " are an ampersand. */
static bool label_hot(const char *s) { return s[0] == '&' && s[1] && s[1] != ' ' && s[1] != '&'; }

int dk_label_len(const char *s)
{
    int n = 0;
    for (; s && *s; s++) {
        if (label_hot(s)) continue;
        if (s[0] == '&' && s[1] == '&') s++;
        n++;
    }
    return n;
}

char dk_label_key(const char *s)
{
    for (; s && *s; s++) {
        if (label_hot(s)) return (char)(s[1] >= 'a' && s[1] <= 'z' ? s[1] - 32 : s[1]);
        if (s[0] == '&' && s[1] == '&') s++;
    }
    return 0;
}

int dk_label(tui_surface *sf, tui_rect clip, int x, int y, const char *s, uint8_t attr, bool mark)
{
    int n = 0;
    for (; s && *s; s++) {
        bool key = false;
        if (label_hot(s)) { s++; key = mark; }
        else if (s[0] == '&' && s[1] == '&') s++;
        tui_put_char(sf, clip, x + n, y, *s, key ? dk_accel_on(attr) : attr);
        n++;
    }
    return n;
}

/* The push button, one row, sized for a mouse: a band of the key face with
   a bevel line at each end, heavy ends on the default one. Focus and a held
   press show as the selection colours, the pointer over it as the hover
   face (both found here; the flags force them). Greyed: no band, its ends
   and dashes in the disabled ink, so it reads as off in a one-ink scheme. */
void dk_pushbtn(tui_surface *sf, tui_rect clip, tui_rect r, const char *label, unsigned state)
{
    if (r.w < 4) return;
    r.h = 1;
    const bool dis = (state & DK_BS_DISABLED) != 0;
    const bool down = !dis && ((state & DK_BS_PRESSED) || dk_pressed(r));
    const bool hov = !dis && ((state & DK_BS_HOVER) || dk_hover(r));
    const tui_rect row = tui_rect_intersect(clip, r);
    const int n = dk_label_len(label), x = r.x + (r.w - n) / 2 + (down ? 1 : 0);
    if (dis) {
        const uint8_t d = dk_attr(R_DISABLED);
        tui_put_char(sf, row, r.x, r.y, '[', d);
        tui_put_char(sf, row, r.x + r.w - 1, r.y, ']', d);
        for (int i = r.x + 1; i < x - 1; i++) ls_box_put(sf, row, i, r.y, LS_BOX_DASH_H, d);
        for (int i = x + n + 1; i < r.x + r.w - 1; i++) ls_box_put(sf, row, i, r.y, LS_BOX_DASH_H, d);
        dk_label(sf, row, x, r.y, label, d, false);
        return;
    }
    const bool lit = down || (state & (DK_BS_FOCUS | DK_BS_ON));
    const uint8_t face = lit ? dk_attr(R_SELECT) : hov ? dk_attr(R_TASKHOVER) : dk_attr(R_KEY);
    dk_fill(sf, row, face);
    if (state & DK_BS_DEFAULT) {
        ls_box_put(sf, row, r.x, r.y, LS_BOX_DBL_V, face);
        ls_box_put(sf, row, r.x + r.w - 1, r.y, LS_BOX_DBL_V, face);
    } else {
        ls_box_put(sf, row, r.x, r.y, LS_EDGE_L, on_bg(face, down ? R_SHADOW : R_HILITE));
        ls_box_put(sf, row, r.x + r.w - 1, r.y, LS_EDGE_R, on_bg(face, down ? R_HILITE : R_SHADOW));
    }
    dk_label(sf, tui_rect_intersect(row, tui_rect_make(r.x + 1, r.y, r.w - 2, 1)), x, r.y, label, face, !lit && !hov);
}

/* A sunken one-row field: brackets in the bevel colours, the text strong,
   the caret a line under its cell while the field has the keys. */
static void field_ends(tui_surface *sf, tui_rect clip, int x, int y, int w, bool focus)
{
    tui_put_char(sf, clip, x, y, '[', focus ? dk_attr(R_STRONG) : dk_attr(R_SHADOW));
    tui_put_char(sf, clip, x + w - 1, y, ']', focus ? dk_attr(R_STRONG) : dk_attr(R_HILITE));
}

void dk_field(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *text, int caret, bool focus)
{
    if (w < 3) return;
    field_ends(sf, clip, x, y, w, focus);
    const tui_rect in = tui_rect_intersect(tui_rect_intersect(clip, tui_surface_rect(sf)), tui_rect_make(x + 1, y, w - 2, 1));
    dk_fill(sf, in, dk_attr(R_FACE));
    const int n = dk_text_len(text), room = w - 3;
    const int skip = caret >= 0 && n > room ? n - room : 0;         /* the tail stays in view while typing */
    const char *s = text ? text : "";
    for (int k = 0; k < skip && *s; k++) s++;
    dk_text(sf, in, x + 1, y, s, dk_attr(R_STRONG));
    if (focus && caret >= 0) {
        const int cx = x + 1 + (caret > n ? n : caret) - skip;
        if (tui_rect_contains(in, cx, y)) {
            tui_cell *c = &sf->back[(size_t)y * sf->w + cx];
            if (c->ch == ' ' || c->ch == 0) { c->ch = (int16_t)LS_EDGE_B; c->attr = dk_attr(R_ACCEL); }
            else c->attr = dk_attr(R_SELECT);
        }
    }
}

/* A vertical scrollbar, one column: an arrow key at each end, a dashed
   track, and a thumb as long as the part in view. */
static void vscroll_geom(int h, int total, int page, int top, int *ty, int *th)
{
    const int len = h - 2;
    if (total < 1) total = 1;
    if (page > total) page = total;
    int t = len * page / total;
    if (t < 1) t = 1;
    if (t > len) t = len;
    const int span = total - page;
    int at = span > 0 ? (int)(((long)(len - t) * top + span / 2) / span) : 0;
    if (at > len - t) at = len - t;
    if (at < 0) at = 0;
    *ty = at;
    *th = t;
}

void dk_vscroll(tui_surface *sf, tui_rect clip, int x, int y, int h, int total, int page, int top)
{
    if (h < 3) return;
    const bool live = total > page;
    const tui_rect up = tui_rect_make(x, y, 1, 1), dn = tui_rect_make(x, y + h - 1, 1, 1);
    const uint8_t key = dk_attr(R_KEY), dead = on_bg(key, R_DISABLED);
    ls_box_put(sf, clip, x, y, LS_SHAPE_UP, !live ? dead : dk_hover(up) ? dk_attr(R_TASKHOVER) : key);
    ls_box_put(sf, clip, x, y + h - 1, LS_SHAPE_DOWN, !live ? dead : dk_hover(dn) ? dk_attr(R_TASKHOVER) : key);
    int ty = 0, th = 0;
    vscroll_geom(h, total, page, top, &ty, &th);
    for (int k = 0; k < h - 2; k++) {
        const bool thumb = live && k >= ty && k < ty + th;
        if (thumb) {
            const tui_rect cell = tui_rect_intersect(clip, tui_rect_make(x, y + 1 + k, 1, 1));
            dk_fill(sf, cell, key);
            if (th >= 3 && k == ty + th / 2) ls_box_put(sf, cell, x, y + 1 + k, LS_BOX_H, on_bg(key, R_SHADOW));
        } else {
            ls_box_put(sf, clip, x, y + 1 + k, LS_BOX_DASH_V, dk_attr(R_SHADOW));
        }
    }
}

int dk_vscroll_hit(int y, int h, int total, int page, int top, int row)
{
    if (h < 3 || row < y || row >= y + h) return DK_SB_NONE;
    if (row == y) return DK_SB_UP;
    if (row == y + h - 1) return DK_SB_DOWN;
    int ty = 0, th = 0;
    vscroll_geom(h, total, page, top, &ty, &th);
    const int k = row - y - 1;
    return k < ty ? DK_SB_PGUP : k >= ty + th ? DK_SB_PGDN : DK_SB_THUMB;
}

/* The top row that puts the middle of the thumb at `row`: a drag of the thumb. */
int dk_vscroll_drag(int y, int h, int total, int page, int row)
{
    const int len = h - 2, span = total - page;
    if (len < 1 || span <= 0) return 0;
    int ty = 0, th = 0;
    vscroll_geom(h, total, page, 0, &ty, &th);
    int at = row - y - 1 - th / 2;
    if (at < 0) at = 0;
    if (at > len - th) at = len - th;
    return len - th > 0 ? (int)(((long)at * span + (len - th) / 2) / (len - th)) : 0;
}

/* A click on the bar at col,row: the arrows step a row, the track pages,
   the thumb goes where it is put. True when the click was on the bar. */
bool dk_vscroll_click(int x, int y, int h, int total, int page, int *top, int col, int row)
{
    if (col != x) return false;
    const int part = dk_vscroll_hit(y, h, total, page, *top, row);
    if (part == DK_SB_NONE) return false;
    int t = *top;
    switch (part) {
    case DK_SB_UP: t--; break;
    case DK_SB_DOWN: t++; break;
    case DK_SB_PGUP: t -= page > 1 ? page - 1 : 1; break;
    case DK_SB_PGDN: t += page > 1 ? page - 1 : 1; break;
    default: t = dk_vscroll_drag(y, h, total, page, row); break;
    }
    if (t > total - page) t = total - page;
    if (t < 0) t = 0;
    *top = t;
    return true;
}

/* A list view: a sunken frame, a header row of column titles on key faces,
   the rows, and a scrollbar when there are more rows than fit. A column of
   width 0 takes what the others leave. */
tui_rect dk_listview_bar(tui_rect r) { return tui_rect_make(r.x + r.w - 2, r.y + 2, 1, r.h - 3); }
int dk_listview_rows(tui_rect r) { return r.h - 3 > 0 ? r.h - 3 : 0; }

static int lv_col_w(const dk_col_t *cols, int ncols, int i, int room)
{
    int fixed = 0;
    for (int k = 0; k < ncols; k++) fixed += cols[k].w > 0 ? cols[k].w : 0;
    const int rest = room - fixed;
    return cols[i].w > 0 ? cols[i].w : rest > 4 ? rest : 4;
}

void dk_listview(tui_surface *sf, tui_rect clip, tui_rect r, const dk_col_t *cols, int ncols, int n, int top,
                 int sel, bool focus, dk_cell_fn cell, void *ctx)
{
    if (r.w < 8 || r.h < 4) return;
    dk_frame(sf, clip, r, false);
    const tui_rect in = tui_rect_intersect(clip, dk_inner(r));
    const int rows = dk_listview_rows(r);
    const bool bar = n > rows;
    const int x0 = r.x + 1, room = r.w - 2 - (bar ? 1 : 0);
    /* the header: one band of key face, a line between the titles */
    const uint8_t head = dk_attr(R_KEY);
    dk_fill(sf, tui_rect_intersect(in, tui_rect_make(x0, r.y + 1, r.w - 2, 1)), head);
    int cx = x0;
    for (int k = 0; k < ncols; k++) {
        const int cw = lv_col_w(cols, ncols, k, room);
        const tui_rect hc = tui_rect_intersect(in, tui_rect_make(cx, r.y + 1, cw, 1));
        const int tl = dk_text_len(cols[k].title);
        dk_text(sf, hc, cols[k].right ? cx + cw - 1 - tl : cx + 1, r.y + 1, cols[k].title, head);
        if (k) ls_box_put(sf, hc, cx, r.y + 1, LS_EDGE_L, on_bg(head, R_SHADOW));
        cx += cw;
    }
    const tui_rect body = tui_rect_make(x0, r.y + 2, room, rows);
    int ptr_col = -1, ptr_row = -1;
    const bool hov = dk_hover(body);
    if (hov) dk_pointer(&ptr_col, &ptr_row);
    for (int k = 0; k < rows && top + k < n; k++) {
        const int i = top + k, y = r.y + 2 + k;
        const bool s = i == sel, hot = !s && hov && ptr_row == y;
        const uint8_t face = s ? (focus ? dk_attr(R_SELECT) : dk_attr(R_SELECT_DIM)) : hot ? dk_attr(R_TASKHOVER) : dk_attr(R_FACE);
        const tui_rect rr = tui_rect_intersect(in, tui_rect_make(x0, y, room, 1));
        if (s || hot) dk_fill(sf, rr, face);
        cx = x0;
        for (int c = 0; c < ncols; c++) {
            const int cw = lv_col_w(cols, ncols, c, room);
            char buf[64];
            uint8_t hue = 0;
            int glyph = 0;
            buf[0] = 0;
            cell(i, c, buf, sizeof(buf), &hue, &glyph, ctx);
            const tui_rect cc = tui_rect_intersect(rr, tui_rect_make(cx, y, cw - (c + 1 < ncols ? 1 : 0), 1));
            const uint8_t plain = c ? dk_attr(R_FACE) : dk_attr(R_STRONG);
            int tx = cx + 1;
            if (glyph) { put_cp(sf, cc, tx, y, glyph, s || hot ? face : dk_art(hue ? hue : TUI_WHITE)); tx += 2; }
            if (cols[c].right) tx = cx + cw - 1 - dk_text_len(buf);
            dk_text(sf, cc, tx, y, buf, s || hot ? face : glyph || !hue ? plain : dk_hue(hue));
            cx += cw;
        }
    }
    if (bar) { const tui_rect b = dk_listview_bar(r); dk_vscroll(sf, in, b.x, b.y, b.h, n, rows, top); }
}

int dk_listview_hit(tui_rect r, int n, int top, int col, int row)
{
    if (col <= r.x || col >= r.x + r.w - 1 || row <= r.y || row >= r.y + r.h - 1) return DK_LV_NONE;
    if (row == r.y + 1) return DK_LV_HEADER;
    if (n > dk_listview_rows(r) && col == r.x + r.w - 2) return DK_LV_BAR;
    const int i = top + row - r.y - 2;
    return i < n ? i : DK_LV_NONE;
}

/* The header column under a cell, or -1. */
int dk_listview_col(tui_rect r, const dk_col_t *cols, int ncols, int n, int col)
{
    const int room = r.w - 2 - (n > dk_listview_rows(r) ? 1 : 0);
    int cx = r.x + 1;
    for (int k = 0; k < ncols; k++) {
        const int cw = lv_col_w(cols, ncols, k, room);
        if (col >= cx && col < cx + cw) return k;
        cx += cw;
    }
    return -1;
}

/* A menu bar: one row of titles on the menu face; the open one on the
   selection colours, the one under the pointer on the hover face. */
static int bar_x(int x, const char *const *names, int i)
{
    for (int k = 0; k < i; k++) x += dk_label_len(names[k]) + 2;
    return x;
}

void dk_menubar(tui_surface *sf, tui_rect clip, int x, int y, int w, const char *const *names, int n, int open, bool keys)
{
    const uint8_t face = dk_attr(R_MENU);
    const tui_rect row = tui_rect_intersect(clip, tui_rect_make(x, y, w, 1));
    dk_fill(sf, row, face);
    for (int i = 0; i < n; i++) {
        const int tx = bar_x(x, names, i), tw = dk_label_len(names[i]) + 2;
        const tui_rect t = tui_rect_intersect(row, tui_rect_make(tx, y, tw, 1));
        const uint8_t a = i == open ? dk_attr(R_SELECT) : keys && dk_hover(t) ? dk_attr(R_TASKHOVER) : face;
        if (a != face) dk_fill(sf, t, a);
        dk_label(sf, t, tx + 1, y, names[i], a, a == face && keys);
    }
}

int dk_menubar_hit(int x, int y, const char *const *names, int n, int col, int row)
{
    if (row != y) return -1;
    for (int i = 0; i < n; i++) {
        const int tx = bar_x(x, names, i);
        if (col >= tx && col < tx + dk_label_len(names[i]) + 2) return i;
    }
    return -1;
}

int dk_menubar_x(int x, const char *const *names, int i) { return bar_x(x, names, i); }
