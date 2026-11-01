/* The desk's motion. Every animation here starts because the owner did
   something or the board's state changed (a window opened, a menu was
   pulled, a viewer connected), runs for a fraction of a second against the
   clock at whatever rate the board draws, and stops. Nothing loops, and
   nothing moves while nothing happens. Effects > animations off turns all
   of it into the end state at once. Input is never held for any of it. */

#include "ls_desk_priv.h"

#include <string.h>

#include "esp_timer.h"

enum { AN_NONE, AN_ZOOM, AN_ICON, AN_KEY, AN_START, AN_SPLASH, AN_DIM };

typedef struct {
    uint8_t kind;
    int64_t t0;
    int ms;
    tui_rect a, b;
    int arg, arg2;
} anim_t;

#define N_ANIM 8
static anim_t s_an[N_ANIM];

static bool enabled(void) { return dk_prefs()->anim; }
static int64_t now_us(void) { return esp_timer_get_time(); }

/* 0..1000 of the way through, or -1 when it is over. */
static int progress(const anim_t *a)
{
    const int64_t t = now_us() - a->t0;
    if (t < 0) return 0;
    if (t >= (int64_t)a->ms * 1000) return -1;
    return (int)(t / a->ms);
}

static anim_t *start(int kind, int ms)
{
    if (!enabled()) return NULL;
    anim_t *slot = NULL;
    for (int i = 0; i < N_ANIM; i++)
        if (s_an[i].kind == AN_NONE || progress(&s_an[i]) < 0) { slot = &s_an[i]; break; }
    if (!slot) slot = &s_an[0];
    memset(slot, 0, sizeof(*slot));
    slot->kind = (uint8_t)kind;
    slot->t0 = now_us();
    slot->ms = ms;
    return slot;
}

static anim_t *find(int kind)
{
    for (int i = 0; i < N_ANIM; i++)
        if (s_an[i].kind == kind && progress(&s_an[i]) >= 0) return &s_an[i];
    return NULL;
}

bool dk_anim_active(void)
{
    for (int i = 0; i < N_ANIM; i++)
        if (s_an[i].kind != AN_NONE && progress(&s_an[i]) >= 0) return true;
    return false;
}

void dk_anim_zoom(tui_rect from, tui_rect to)
{
    anim_t *a = start(AN_ZOOM, 260);
    if (a) { a->a = from; a->b = to; }
}
void dk_anim_icon(int i) { anim_t *a = start(AN_ICON, 160); if (a) a->arg = i; }
void dk_anim_key(int win, int key) { anim_t *a = start(AN_KEY, 140); if (a) { a->arg = win; a->arg2 = key; } }
void dk_anim_start(void) { start(AN_START, 220); }
void dk_anim_splash(void) { start(AN_SPLASH, 1800); }
void dk_anim_dim(void) { start(AN_DIM, 360); }

/* How far an effect has got, 0..1000, 1000 when it is not running. */
int dk_anim_icon_progress(int i)
{
    const anim_t *a = find(AN_ICON);
    return a && a->arg == i ? progress(a) : 1000;
}
int dk_anim_key_progress(int win, int key)
{
    const anim_t *a = find(AN_KEY);
    return a && a->arg == win && a->arg2 == key ? progress(a) : 1000;
}
int dk_anim_start_progress(void) { const anim_t *a = find(AN_START); return a ? progress(a) : 1000; }
int dk_anim_dim_progress(void) { const anim_t *a = find(AN_DIM); return a ? progress(a) : 1000; }
bool dk_anim_splash_on(void) { return find(AN_SPLASH) != NULL; }
void dk_anim_splash_end(void) { anim_t *a = find(AN_SPLASH); if (a) a->kind = AN_NONE; }

static tui_rect lerp(tui_rect a, tui_rect b, int k)
{
    return tui_rect_make(a.x + (b.x - a.x) * k / 1000, a.y + (b.y - a.y) * k / 1000,
                         a.w + (b.w - a.w) * k / 1000, a.h + (b.h - a.h) * k / 1000);
}

/* A zoom: the rectangle and two fainter ones behind it, a trail from where
   the window was to where it is going. */
static void draw_zoom(tui_surface *sf, const anim_t *a, int k)
{
    static const int LAG[3] = {0, 160, 320};
    static const int ROLE[3] = {R_STRONG, R_HILITE, R_SHADOW};
    for (int i = 2; i >= 0; i--) {
        const int kk = k - LAG[i];
        if (kk < 0) continue;
        tui_rect r = lerp(a->a, a->b, kk);
        if (r.w < 2) r.w = 2;
        if (r.h < 2) r.h = 2;
        dk_frame_march(sf, tui_surface_rect(sf), r, dk_attr(ROLE[i]), (unsigned)kk / 120);
    }
}

/* The welcome a viewer sees as it connects: the fin cuts in, the name
   types itself, then it gets out of the way. Any key or click ends it. */
static void draw_splash(tui_surface *sf, int k)
{
    const tui_rect all = tui_surface_rect(sf);
    const uint8_t g = dk_attr(R_DESK);
    tui_fill(sf, all, ' ', g);
    const int cx = sf->w / 2, cy = sf->h / 2 - 3;
    const uint8_t fin = (uint8_t)((g & 0xF0) | (dk_light_desk() ? TUI_BLUE : (TUI_CYAN | TUI_BRIGHT)));
    const uint8_t back = (uint8_t)((g & 0xF0) | (dk_light_desk() ? TUI_BLACK : (TUI_WHITE | TUI_BRIGHT)));
    const int rows = 7, shown = k < 400 ? rows * k / 400 : rows;
    for (int r = rows - shown; r < rows; r++) {
        ls_box_put(sf, all, cx - 2 * r - 1, cy - rows + r, LS_BOX_DIAG_R, fin);
        ls_box_put(sf, all, cx - 2 * r - 2, cy - rows + r, LS_BOX_DIAG_R, fin);
        ls_box_put(sf, all, cx + 1, cy - rows + r, LS_BOX_DIAG_L, back);
    }
    const uint8_t sea = (uint8_t)((g & 0xF0) | (dk_light_desk() ? TUI_BLUE : (TUI_BLUE | TUI_BRIGHT)));
    const int span = k < 500 ? 20 * k / 500 : 20;
    for (int x = cx - span; x <= cx + span; x++) tui_put_char(sf, all, x, cy, ((x - cx) & 3) < 2 ? '~' : '-', sea);
    static const char NAME[] = "L A K E S H A R K   D E S K T O P";
    const int n = (int)sizeof(NAME) - 1, typed = k < 450 ? 0 : k < 1000 ? n * (k - 450) / 550 : n;
    for (int i = 0; i < typed; i++)
        if (NAME[i] != ' ')
            tui_put_char(sf, all, cx - n / 2 + i, cy + 2, NAME[i],
                         (uint8_t)((g & 0xF0) | (i < 17 ? (dk_light_desk() ? TUI_BLUE : (TUI_CYAN | TUI_BRIGHT))
                                                        : (dk_light_desk() ? TUI_BLACK : (TUI_WHITE | TUI_BRIGHT)))));
    if (k > 1000) {
        const char *w = "Welcome. The board is ready.";
        dk_text(sf, all, cx - (int)strlen(w) / 2, cy + 4, w, (uint8_t)((g & 0xF0) | (dk_light_desk() ? TUI_BLACK : TUI_WHITE)));
    }
}

void dk_anim_draw_trails(tui_surface *sf)
{
    for (int i = 0; i < N_ANIM; i++) {
        anim_t *a = &s_an[i];
        if (a->kind == AN_NONE) continue;
        const int k = progress(a);
        if (k < 0) { a->kind = AN_NONE; continue; }
        if (a->kind == AN_ZOOM) draw_zoom(sf, a, k);
    }
}

void dk_anim_draw(tui_surface *sf)
{
    const anim_t *s = find(AN_SPLASH);
    if (s) draw_splash(sf, progress(s));
}
