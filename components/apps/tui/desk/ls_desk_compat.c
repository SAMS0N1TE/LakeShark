/* Compatibility windows: every app, unchanged, in a window.

   The router draws, as it always has, into an off-screen surface the size of
   the front window's client area (full mode: no status row, tab strip or hint
   row; the taskbar and the window's status bar carry those). That surface is
   copied into the grid. When the window loses the front its last frame is
   kept as a snapshot, so a background window shows what it last showed and
   nothing in it moves. Everything here lives in PSRAM. */

#include "ls_desk_priv.h"
#include "esp_attr.h"

#include <string.h>

#include "../ls_tui_screen.h"
#include "../ls_tui_touch.h"
#include "../ls_theme.h"
#include "../ls_tui_ui.h"
#include "../ls_deskui.h"

static tui_surface s_live;
static tui_cell *s_live_back, *s_live_front;
static int s_live_w, s_live_h, s_live_id = -1;
static void pending_tap(dk_win_t *w);
static int s_snaps;
static const ls_app_t *s_selected;
static int s_selected_id = -1;              /* the window s_selected was chosen for */

void dk_compat_release(void)
{
    ls_tui_touch_client_clear();
    dk_psram_free(s_live_back);
    dk_psram_free(s_live_front);
    s_live_back = s_live_front = NULL;
    s_live_w = s_live_h = 0;
    s_live_id = -1;
    s_selected = NULL;
    s_selected_id = -1;
}

static bool live_ensure(int w, int h)
{
    if (s_live_back && w == s_live_w && h == s_live_h) return true;
    const int keep = s_live_id;
    dk_compat_release();
    const size_t n = (size_t)w * (size_t)h * sizeof(tui_cell);
    s_live_back = dk_psram(n);
    s_live_front = dk_psram(n);
    if (!s_live_back || !s_live_front) { dk_compat_release(); return false; }
    tui_surface_setup(&s_live, s_live_back, s_live_front, w, h);
    s_live_w = w;
    s_live_h = h;
    s_live_id = keep;
    return true;
}

int dk_compat_live_id(void) { return s_live_id; }
int dk_compat_blocks(void) { return (s_live_back ? 2 : 0) + s_snaps; }

/* An app draws on the terminal's black ground, whatever the scheme. In a
   window it takes the scheme's Window colours instead: its ground becomes
   the face, its text the face's text, each hue the nearest one that reads on
   that face (by the palette's own luminance), and a light band that would
   vanish on a light face turns dark. A black face maps every colour to
   itself, so Bay 95 and the other dark schemes look as they always did. */
int dk_compat_shell_open = -1;               /* the desk menu down from the app's bar (0..2), set by the desk */

/* The desk's title in the live app's own bar under a window cell: 0 Desk,
   1 Window, 2 Help; -1 none. *x gets where it starts. */
int dk_compat_shell_hit(const dk_win_t *w, int col, int row, int *x)
{
    const ls_dui_shellbar_t *b = ls_dui_shellbar();
    if (!w || !w->own_bar || w->id != s_live_id || !b->drawn || b->x[0] < 0 || row != b->y) return -1;
    for (int i = 0; i < LS_DUI_SHELL_TITLES; i++)
        if (col >= b->x[i] && col < b->x[i + 1]) { if (x) *x = b->x[i]; return i; }
    return -1;
}
int dk_compat_shell_at(const dk_win_t *w, int i, int *x, int *y)
{
    const ls_dui_shellbar_t *b = ls_dui_shellbar();
    if (!w || !w->own_bar || w->id != s_live_id || !b->drawn || b->x[0] < 0 || i < 0 || i >= LS_DUI_SHELL_TITLES) return -1;
    *x = b->x[i];
    *y = b->y;
    return i;
}

EXT_RAM_BSS_ATTR static ls_ink_keep_t s_live_keep;            /* what the live frame asked to keep black */
static uint8_t s_map[256];
static struct { uint8_t face, strong, dis; const void *theme; bool ok, same; } s_map_key;

static int lum(const ls_tui_theme_t *t, uint8_t idx)
{
    const uint16_t c = t->palette[idx & 15];
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (r * 255 / 31 * 299 + g * 255 / 63 * 587 + b * 255 / 31 * 114) / 1000;
}

static int apart(const ls_tui_theme_t *t, uint8_t a, uint8_t b)
{
    const int d = lum(t, a) - lum(t, b);
    return d < 0 ? -d : d;
}

#define MAP_NEED 64
static uint8_t readable(const ls_tui_theme_t *t, uint8_t want, uint8_t bg, uint8_t fallback)
{
    if (apart(t, want, bg) >= MAP_NEED) return want;
    const uint8_t other = (uint8_t)(want ^ TUI_BRIGHT);          /* the same hue, the other weight */
    if (apart(t, other, bg) >= MAP_NEED) return other;
    const uint8_t hue = want & 7;
    const uint8_t kin = hue == TUI_YELLOW ? TUI_RED : hue == TUI_CYAN ? TUI_BLUE : hue == TUI_GREEN ? TUI_CYAN
                      : hue == TUI_BLUE ? TUI_CYAN : hue == TUI_RED ? TUI_YELLOW : hue == TUI_MAGENTA ? TUI_RED : hue;
    if (apart(t, kin, bg) >= MAP_NEED) return kin;
    if (apart(t, (uint8_t)(kin | TUI_BRIGHT), bg) >= MAP_NEED) return (uint8_t)(kin | TUI_BRIGHT);
    return fallback;
}

static void map_build(void)
{
    const ls_tui_theme_t *t = ls_tui_active_theme();
    const uint8_t face = dk_attr(R_FACE), strong = TUI_ATTR_FG(dk_attr(R_STRONG)), dis = TUI_ATTR_FG(dk_attr(R_DISABLED));
    if (s_map_key.ok && s_map_key.face == face && s_map_key.strong == strong && s_map_key.dis == dis && s_map_key.theme == t) return;
    s_map_key.face = face; s_map_key.strong = strong; s_map_key.dis = dis; s_map_key.theme = t;
    s_map_key.ok = true;
    const uint8_t fbg = TUI_ATTR_BG(face), ffg = TUI_ATTR_FG(face);
    s_map_key.same = fbg == TUI_BLACK || !t;
    if (s_map_key.same) return;
    const uint8_t band = apart(t, TUI_BLACK | TUI_BRIGHT, fbg) >= MAP_NEED ? (TUI_BLACK | TUI_BRIGHT) : TUI_BLACK;
    for (int a = 0; a < 256; a++) {
        const uint8_t fg = TUI_ATTR_FG(a), bg = TUI_ATTR_BG(a);
        uint8_t nf = fg, nb = bg;
        if (bg == TUI_BLACK) {
            nb = fbg;
            if (fg == TUI_BLACK) nf = fbg;                                   /* drawn to be unseen: still unseen */
            else if (fg == TUI_WHITE) nf = ffg;
            else if (fg == (TUI_WHITE | TUI_BRIGHT)) nf = strong;
            else if (fg == (TUI_BLACK | TUI_BRIGHT)) nf = apart(t, dis, fbg) >= 40 ? dis : ffg;
            else nf = readable(t, fg, fbg, ffg);
        } else if (apart(t, bg, fbg) < 40) {
            /* a band as light as the face: the dark band of this scheme */
            nb = band;
            nf = readable(t, fg == TUI_BLACK || fg == (TUI_BLACK | TUI_BRIGHT) ? (TUI_WHITE | TUI_BRIGHT) : fg, nb, TUI_WHITE | TUI_BRIGHT);
        }
        s_map[a] = TUI_ATTR(nf, nb);
    }
}

/* The app's colour roles (ls_ink, ls_tui_ui.h), from the scheme. A role is
   handed to the app in the app's own colour space: one on the face is text
   on TUI_BLACK, which the map above turns into the face; a band of another
   colour is itself, and the map is told to leave it alone. So a selection
   bar an app draws with LS_INK_SELECT is the scheme's Selected Items colour
   on any face, and literals the app still has go through the map as before. */
static uint8_t s_ink_final[LS_INK_COUNT], s_ink_app[LS_INK_COUNT];
static bool s_ink_ok;

static void ink_build(void)
{
    static const uint8_t ROLE[LS_INK_COUNT] = {
        [LS_INK_SELECT] = R_SELECT, [LS_INK_SELECT_DIM] = R_SELECT_DIM, [LS_INK_KEY] = R_KEY,
        [LS_INK_KEY_ON] = R_TITLE_ON, [LS_INK_MENU] = R_MENU, [LS_INK_TITLE] = R_TITLE_ON,
        [LS_INK_TEXT] = R_FACE, [LS_INK_STRONG] = R_STRONG, [LS_INK_DIM] = R_DISABLED,
        [LS_INK_ACCENT] = R_INFO, [LS_INK_GOOD] = R_GOOD, [LS_INK_WARN] = R_WARN, [LS_INK_BAD] = R_BAD,
        [LS_INK_RULE] = R_SHADOW, [LS_INK_ACCEL] = R_ACCEL, [LS_INK_CAPTION] = R_TITLE_OFF,
    };
    uint8_t fin[LS_INK_COUNT];
    for (int i = 0; i < LS_INK_COUNT; i++) fin[i] = dk_attr(ROLE[i]);
    if (s_ink_ok && !memcmp(fin, s_ink_final, sizeof(fin)) && s_map_key.ok && s_map_key.theme == ls_tui_active_theme()
        && s_map_key.face == dk_attr(R_FACE)) return;
    memcpy(s_ink_final, fin, sizeof(fin));
    s_ink_ok = true;
    s_map_key.ok = false;                    /* the map again, then the roles over it */
    map_build();
    const ls_tui_theme_t *t = ls_tui_active_theme();
    const uint8_t face = dk_attr(R_FACE), fbg = TUI_ATTR_BG(face), ffg = TUI_ATTR_FG(face);
    const uint8_t strong = TUI_ATTR_FG(dk_attr(R_STRONG));
    /* Warn and bad are one hue in the light schemes: warn takes the first
       other hue that reads on the face. */
    if (t && TUI_ATTR_FG(fin[LS_INK_WARN]) == TUI_ATTR_FG(fin[LS_INK_BAD])) {
        static const uint8_t TRY[] = {TUI_YELLOW, TUI_MAGENTA, TUI_YELLOW | TUI_BRIGHT, TUI_MAGENTA | TUI_BRIGHT, TUI_BLUE};
        for (unsigned i = 0; i < sizeof(TRY); i++)
            if (TRY[i] != TUI_ATTR_FG(fin[LS_INK_BAD]) && apart(t, TRY[i], fbg) >= MAP_NEED) {
                fin[LS_INK_WARN] = TUI_ATTR(TRY[i], fbg);
                break;
            }
    }
    for (int i = 0; i < LS_INK_COUNT; i++) {
        uint8_t fg = TUI_ATTR_FG(fin[i]), bg = TUI_ATTR_BG(fin[i]);
        if (s_map_key.same) {                /* a black face: the app's space is the grid's */
            s_ink_app[i] = fin[i];
            continue;
        }
        if (bg == fbg) {                     /* on the face: text on the app's ground */
            const bool plain = fg == ffg || fg == strong;
            const uint8_t a = TUI_ATTR(fg == ffg ? TUI_WHITE : fg == strong ? (TUI_WHITE | TUI_BRIGHT) : fg, TUI_BLACK);
            /* A hue the scheme chose is taken as it is; white, grey and black
               keep the meaning the map gives them. */
            if (!plain && (fg & 7) != TUI_BLACK && (fg & 7) != TUI_WHITE) s_map[a] = fin[i];
            s_ink_app[i] = a;
            if (!plain && ((fg & 7) == TUI_BLACK || (fg & 7) == TUI_WHITE))
                s_ink_app[i] = TUI_ATTR(i == LS_INK_DIM || i == LS_INK_RULE ? (TUI_BLACK | TUI_BRIGHT) : TUI_WHITE, TUI_BLACK);
            continue;
        }
        if (bg == TUI_BLACK) bg = TUI_BLACK | TUI_BRIGHT;       /* black is the app's ground: the nearest band */
        if (fg == bg) fg = TUI_WHITE | TUI_BRIGHT;
        s_ink_app[i] = TUI_ATTR(fg, bg);
        s_map[s_ink_app[i]] = s_ink_app[i];
    }
}

/* Rects the app asked to keep on its own black ground (ls_ink_keep): a
   waterfall reads on black, not on silver. */
static bool kept(const ls_ink_keep_t *k, int x, int y)
{
    for (int i = 0; i < k->n; i++)
        if (x >= k->r[i].x && y >= k->r[i].y && x < k->r[i].x + k->r[i].w && y < k->r[i].y + k->r[i].h) return true;
    return false;
}

static void blit(tui_surface *sf, const tui_cell *src, int w, int h, tui_rect at, tui_rect clip, bool still,
                 const ls_ink_keep_t *keep)
{
    clip = tui_rect_intersect(clip, tui_surface_rect(sf));
    ink_build();
    const bool same = s_map_key.same;
    if (same || (keep && !keep->n)) keep = NULL;
    /* The light schemes' palette has dark plain hues, for text on silver: on
       black glass they take their bright twins. */
    const bool glass_fix = keep && dk_theme_own(ls_tui_get_theme());
    for (int y = 0; y < h && y < at.h; y++) {
        const int gy = at.y + y;
        if (gy < clip.y || gy >= clip.y + clip.h) continue;
        bool row_kept = false;
        for (int i = 0; keep && i < keep->n; i++) if (y >= keep->r[i].y && y < keep->r[i].y + keep->r[i].h) row_kept = true;
        for (int x = 0; x < w && x < at.w; x++) {
            const int gx = at.x + x;
            if (gx < clip.x || gx >= clip.x + clip.w) continue;
            tui_cell c = src[(size_t)y * w + x];
            /* A still frame has no picture behind its image cells: the
               picture is the live app's. Keep the ground. */
            const bool image = c.ch < 0x100 && (uint8_t)c.ch == (uint8_t)LS_TUI_IMAGE_CELL;
            if (still && image) c.ch = ' ';
            const bool glass = row_kept && kept(keep, x, y);
            if (!same && (!image || still) && !glass) c.attr = s_map[c.attr];
            else if (glass && glass_fix) {
                const uint8_t fg = TUI_ATTR_FG(c.attr), bg = TUI_ATTR_BG(c.attr);
                if (fg >= TUI_RED && fg <= TUI_CYAN && dk_contrast(fg, bg) < 30) c.attr = TUI_ATTR(fg | TUI_BRIGHT, bg);
            }
            sf->back[(size_t)gy * sf->w + gx] = c;
        }
    }
}

/* Card apps share one host screen, so the app can change while the screen
   does not: select on either, or the host keeps the last card app it ran
   (PINBALL opened showing PROBE). */
bool dk_compat_select(dk_win_t *w)
{
    if (!w) return false;
    const bool app_changed = s_selected != w->app;
    const int now = ls_tui_screen_current();
    /* The live app moved itself to another app's screen (FILES' PLAY shows
       MUSIC with the track, OPEN IN MAP shows MAP): that app opens in its own
       window and carries on. Forcing this window's screen back left the new
       one at once, so FILES could open nothing. */
    if (!app_changed && w->id == s_selected_id && now > 0 && now != w->screen) {   /* 0 is HOME: BACK stays put */
        for (int i = 0; i < ls_app_count(); i++) {
            const ls_app_t *a = ls_app_at(i);
            if (!a || !a->screen || a == w->app || ls_tui_screen_index_of(a->screen) != now) continue;
            const int id = ls_desk_open_app(a->id);
            if (id >= 0 && id != w->id) return true;
        }
    }
    if (w->app && w->app->select && app_changed) w->app->select(w->app->id);
    s_selected = w->app;
    s_selected_id = w->id;
    /* Card apps share one screen: a new card re-enters it. Anything else
       already on show stays as it is (a hand-off above must not restart it). */
    if (now != w->screen || (app_changed && w->app && w->app->select)) ls_tui_screen_show(w->screen);
    return false;
}

/* A press can raise an app before its first front frame. Prepare its own
   surface and select it before sending a held finger, just as drawing does. */
bool dk_compat_prepare(dk_win_t *w, tui_rect client)
{
    if (!w || client.w < 1 || client.h < 1) return false;
    dk_win_t *old = dk_win(s_live_id);
    if (old && old != w) dk_compat_snapshot(old);
    return live_ensure(client.w, client.h) && !dk_compat_select(w);
}

void dk_compat_draw_live(tui_surface *sf, dk_win_t *w, tui_rect client, tui_rect clip)
{
    if (!w || client.w < 1 || client.h < 1) return;
    if (!live_ensure(client.w, client.h)) {
        dk_fill(sf, tui_rect_intersect(client, clip), dk_attr(R_FACE));
        dk_text(sf, clip, client.x + 1, client.y + 1, "Not enough memory for this window.", dk_attr(R_BAD));
        return;
    }
    if (dk_compat_select(w)) {                /* handed to another app's window: keep this one's last frame */
        dk_compat_draw_snapshot(sf, w, client, clip);
        return;
    }
    ls_tui_screen_set_full(true);
    ls_tui_set_origin(&s_live, client.x, client.y);
    ink_build();
    ls_ink_set(s_ink_app);                    /* the scheme's roles for this frame, and an empty keep list */
    {
        int pc = -1, pr = -1;
        dk_pointer(&pc, &pr);
        if (dk_hover_in(w, client)) ls_dui_pointer_set(pc - client.x, pr - client.y);
        else ls_dui_pointer_set(-1, -1);
    }
    ls_dui_shellbar_begin(dk_compat_shell_open);
    ls_tui_router_draw(&s_live);
    s_live_keep = *ls_ink_kept();
    ls_ink_set(NULL);
    ls_dui_pointer_set(-1, -1);
    /* The app drew a menu bar of its own, with the desk's menus in it: the
       desk's bar above it goes. On a page of the app that has no bar (P25's
       CALLS, a handheld view) the desk's bar is back, in the row the app's
       bar had, so every page has a menu and nothing below it moves. The
       desk draws the window once more when this changes (draw_window). */
    w->own_bar = ls_dui_shellbar()->drawn;
    ls_tui_set_origin(NULL, 0, 0);
    s_live_id = w->id;
    pending_tap(w);
    blit(sf, s_live.back, s_live_w, s_live_h, client, clip, false, &s_live_keep);
}

/* F11 full screen and focused mode: the app draws straight to the grid, on
   its own black ground. Where the scheme's face is black too its roles hold
   as they are; on a light face the app keeps its own colours. */
void dk_compat_full_inks(bool on)
{
    if (!on) { ls_ink_set(NULL); return; }
    ink_build();
    ls_ink_set(s_map_key.same ? s_ink_app : NULL);
}

void dk_compat_snapshot(dk_win_t *w)
{
    if (!w || s_live_id != w->id || !s_live_back) return;
    if (!w->snap || w->sw != s_live_w || w->sh != s_live_h) {
        dk_compat_forget(w);
        w->snap = dk_psram((size_t)s_live_w * s_live_h * sizeof(tui_cell) + sizeof(ls_ink_keep_t));
        if (!w->snap) return;
        s_snaps++;
        w->sw = s_live_w;
        w->sh = s_live_h;
    }
    memcpy(w->snap, s_live.back, (size_t)s_live_w * s_live_h * sizeof(tui_cell));
    memcpy(w->snap + (size_t)s_live_w * s_live_h, &s_live_keep, sizeof(s_live_keep));   /* behind the cells */
}

void dk_compat_forget(dk_win_t *w)
{
    if (!w || !w->snap) return;
    dk_psram_free(w->snap);
    w->snap = NULL;
    w->sw = w->sh = 0;
    s_snaps--;
    if (s_live_id == w->id) s_live_id = -1;
}

void dk_compat_draw_snapshot(tui_surface *sf, dk_win_t *w, tui_rect client, tui_rect clip)
{
    if (!w) return;
    if (w->snap && w->sw == client.w && w->sh == client.h) {
        ls_ink_keep_t keep;
        memcpy(&keep, w->snap + (size_t)w->sw * w->sh, sizeof(keep));
        blit(sf, w->snap, w->sw, w->sh, client, clip, true, &keep);
        return;
    }
    /* No frame of this size: the window says so instead of guessing. */
    dk_fill(sf, tui_rect_intersect(client, clip), dk_attr(R_FACE));
    const char *msg = "paused - click to redraw";
    const int n = dk_text_len(msg);
    dk_text(sf, tui_rect_intersect(client, clip), client.x + (client.w - n) / 2, client.y + client.h / 2,
            msg, dk_attr(R_FACE));
}

/* A held finger in the live window: sliders, map pan and game sticks read it.
   col/row are window cells, px/py grid pixels relative to the window. */
void dk_compat_finger(int col, int row, int px, int py, bool down)
{
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col >= s_live_w) col = s_live_w - 1;
    if (row >= s_live_h) row = s_live_h - 1;
    ls_tui_touch_client(col, row, px, py, down);
}

/* A click that raised a window: its app has not drawn yet, so its buttons are
   not where the click is. Deliver it right after its first draw in front. */
static struct { int id, col, row; } s_pend;

void dk_compat_tap_later(dk_win_t *w, int col, int row)
{
    if (!w) return;
    s_pend.id = w->id;
    s_pend.col = col;
    s_pend.row = row;
}

static void pending_tap(dk_win_t *w)
{
    if (!s_pend.id || s_pend.id != w->id) return;
    s_pend.id = 0;
    if (s_pend.col >= 0 && s_pend.row >= 0 && s_pend.col < s_live_w && s_pend.row < s_live_h)
        ls_tui_router_touch_direct(s_pend.col, s_pend.row);
}

bool dk_compat_tap(int col, int row)
{
    if (col < 0 || row < 0 || col >= s_live_w || row >= s_live_h) return false;
    return ls_tui_router_touch_direct(col, row);
}

bool dk_compat_key(ls_tk_t k, char ch)
{
    return ls_tui_router_key_direct(k, ch);
}
