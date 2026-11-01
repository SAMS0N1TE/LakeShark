/* Compatibility windows: every app, unchanged, in a window.

   The router draws, as it always has, into an off-screen surface the size of
   the front window's client area (full mode: no status row, tab strip or hint
   row; the taskbar and the window's status bar carry those). That surface is
   copied into the grid. When the window loses the front its last frame is
   kept as a snapshot, so a background window shows what it last showed and
   nothing in it moves. Everything here lives in PSRAM. */

#include "ls_desk_priv.h"

#include <string.h>

#include "../ls_tui_screen.h"
#include "../ls_tui_touch.h"

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

static void blit(tui_surface *sf, const tui_cell *src, int w, int h, tui_rect at, tui_rect clip, bool still)
{
    clip = tui_rect_intersect(clip, tui_surface_rect(sf));
    for (int y = 0; y < h && y < at.h; y++) {
        const int gy = at.y + y;
        if (gy < clip.y || gy >= clip.y + clip.h) continue;
        for (int x = 0; x < w && x < at.w; x++) {
            const int gx = at.x + x;
            if (gx < clip.x || gx >= clip.x + clip.w) continue;
            tui_cell c = src[(size_t)y * w + x];
            /* A still frame has no picture behind its image cells: the
               picture is the live app's. Keep the ground. */
            if (still && c.ch < 0x100 && (uint8_t)c.ch == (uint8_t)LS_TUI_IMAGE_CELL) c.ch = ' ';
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
    ls_tui_router_draw(&s_live);
    ls_tui_set_origin(NULL, 0, 0);
    s_live_id = w->id;
    pending_tap(w);
    blit(sf, s_live.back, s_live_w, s_live_h, client, clip, false);
}

void dk_compat_snapshot(dk_win_t *w)
{
    if (!w || s_live_id != w->id || !s_live_back) return;
    if (!w->snap || w->sw != s_live_w || w->sh != s_live_h) {
        dk_compat_forget(w);
        w->snap = dk_psram((size_t)s_live_w * s_live_h * sizeof(tui_cell));
        if (!w->snap) return;
        s_snaps++;
        w->sw = s_live_w;
        w->sh = s_live_h;
    }
    memcpy(w->snap, s_live.back, (size_t)s_live_w * s_live_h * sizeof(tui_cell));
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
        blit(sf, w->snap, w->sw, w->sh, client, clip, true);
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
