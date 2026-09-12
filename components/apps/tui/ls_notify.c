/* See ls_notify.h. The banner, the count, and the polling behind them. */
#include "ls_notify.h"

#include <stdio.h>
#include <string.h>

#include "ls_tui_ui.h"

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

/* Six seconds at 25 frames a second. Long enough to read a short message
   without looking up from what you were doing, short enough that it is not
   in the way of the screen it is covering. */
#define SHOW_FRAMES 150

#define MAX_PROBES 4
static ls_notify_probe_t s_probe[MAX_PROBES];
static int               s_probes;

static ls_notice_t s_now;
static int         s_ttl;
static int         s_unread;
static tui_rect    s_rect = { 0, -1, 0, 0 };
static tui_rect    s_close = { 0, -1, 0, 0 };

static bool s_ring = true;
static bool s_vibe = true;

/* A frame counter, free-running, for the unread badge.

   "Should be more attention grabbing, use cool animations for this." The
   banner has a life of its own to animate against - s_ttl says how far
   through it is - but the COUNT outlives it by design, and the count is the
   part that is still there when somebody looks up. It needs a clock that
   does not stop when the banner does. */
static uint32_t s_frame;

#define SLIDE_FRAMES 4

#define FLASH_FRAMES 25

/* ------------------------------------------------------------- sources -- */

void ls_notify_add_probe(ls_notify_probe_t probe)
{
    if (!probe || s_probes >= MAX_PROBES) return;
    for (int i = 0; i < s_probes; i++)
        if (s_probe[i] == probe) return;
    s_probe[s_probes++] = probe;
}

void ls_notify_post(const ls_notice_t *n)
{
    if (!n) return;
    s_now = *n;
    s_ttl = SHOW_FRAMES;
    if (s_unread < 999) s_unread++;
    ls_notify_alert_hw(s_ring, s_vibe);
    /* The banner covers rows the screen underneath already drew, and the
       cell renderer only blits what changed - so without this the first
       frame of a banner over a static screen would be the only one that
       reached the panel, and dismissing it would leave the banner painted.
       The same reason gives for the animation. */
    ls_tui_invalidate();
}

void ls_notify_poll(int visible)
{
    s_frame++;
    if (s_ttl > 0) {
        s_ttl--;
        if (!s_ttl) {
            ls_tui_invalidate();
            /* A notice with nowhere to go is read when its banner ends, because there is no other way to read it. */

            if (s_now.screen < 0 && s_unread) s_unread = 0;
        }
    }

    /* AND A COUNT IS CLEARED BY LOOKING AT WHAT IT IS COUNTING. */

    if (s_unread && s_now.screen >= 0 && s_now.screen == visible) {
        s_unread = 0;
        ls_tui_invalidate();
    }

    for (int i = 0; i < s_probes; i++) {
        ls_notice_t n;
        memset(&n, 0, sizeof(n));
        n.screen = -1;
        if (!s_probe[i](&n)) continue;

        if (n.screen >= 0 && n.screen == visible) continue;
        ls_notify_post(&n);
    }
}

bool ls_notify_showing(void) { return s_ttl > 0; }
int  ls_notify_unread(void)  { return s_unread; }

void ls_notify_clear(void)
{
    if (s_unread || s_ttl) ls_tui_invalidate();
    s_unread = 0;
    s_ttl = 0;
}

/* -------------------------------------------------------------- alerts -- */

bool ls_notify_ring(void) { return s_ring; }
bool ls_notify_vibe(void) { return s_vibe; }

void ls_notify_set_alerts(bool ring, bool vibe) { s_ring = ring; s_vibe = vibe; }

__attribute__((weak))
void ls_notify_alert_hw(bool ring, bool vibe) { (void)ring; (void)vibe; }

/* ---------------------------------------------------------------- draw -- */

void ls_notify_draw(tui_surface *sf, tui_rect area)
{
    s_rect = tui_rect_make(0, -1, 0, 0);
    s_close = tui_rect_make(0, -1, 0, 0);
    if (s_ttl <= 0 || area.w < 20 || area.h < 5) return;

    /* Three rows at the TOP of the screen's area, under the chrome. Top
       because that is where every device anyone has held puts one, and
       because the bottom of this interface is where the controls are: a
       banner over a DISARM button would be the worst three rows on the
       panel to cover. */
    const int full = 4;

    const int age = SHOW_FRAMES - s_ttl;
    int h = full;
    if (age < SLIDE_FRAMES)   h = 1 + age;
    if (s_ttl < SLIDE_FRAMES) h = s_ttl;
    if (h > full) h = full;
    if (h < 1) h = 1;

    tui_rect r = tui_rect_make(area.x, area.y, area.w, h);
    s_rect = r;

    const uint8_t hue = s_now.hue ? s_now.hue : TUI_CYAN;

    /* And it flashes for the first second, then settles. */

    const bool flash = age < FLASH_FRAMES && ((age / 6) & 1);
    const uint8_t edge = flash ? (TUI_WHITE | TUI_BRIGHT) : (hue | TUI_BRIGHT);

    tui_fill(sf, r, ' ', A(TUI_WHITE, TUI_BLACK));
    ls_panel_box(sf, r, NULL, edge);
    if (r.h > 2)
        ls_fill_dither(sf, tui_rect_make(r.x + 1, r.y + 1, r.w - 2, r.h - 2),
                       flash ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);

    /* The text only once there is a row to put it on. During the slide the
       frame arrives first, which is what makes the movement legible. */
    if (r.h >= 3)
        tui_put_str(sf, r, r.x + 2, r.y + 1, s_now.title,
                    A(hue | TUI_BRIGHT, TUI_BLACK));

    if (r.h >= 4) {
        char body[LS_NOTIFY_BODY];
        snprintf(body, sizeof(body), "%.*s", r.w - 5, s_now.body);
        tui_put_str(sf, r, r.x + 2, r.y + 2, body,
                    A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }

    /* A dismiss target of its own, so tapping the banner can mean "take me
       there" without that being the only thing a tap can mean. */
    s_close = tui_rect_make(r.x + r.w - 5, r.y, 5, r.h);
    if (r.h >= 3)
        tui_put_str(sf, r, r.x + r.w - 4, r.y + 1, "[X]",
                    A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

/* The badge, blinking, for as long as anything is unread. */

bool ls_notify_badge_inverted(void)
{
    return s_unread > 0 && ((s_frame / 13) & 1);
}

/* --------------------------------------------------------------- input -- */

bool ls_notify_touch(int col, int row)
{
    if (s_ttl <= 0 || s_rect.h <= 0) return false;
    if (row < s_rect.y || row >= s_rect.y + s_rect.h) return false;
    if (col < s_rect.x || col >= s_rect.x + s_rect.w) return false;

    const bool close = (s_close.h > 0 && col >= s_close.x &&
                        col < s_close.x + s_close.w);
    const int go = s_now.screen;
    s_ttl = 0;
    ls_tui_invalidate();
    if (!close && go >= 0 && go < ls_tui_screen_count()) {
        s_unread = 0;
        ls_tui_screen_show(go);
    }
    return true;
}

bool ls_notify_key(ls_tk_t key)
{
    if (s_ttl <= 0) return false;

    if (key != LS_TK_ESC) return false;
    s_ttl = 0;
    ls_tui_invalidate();
    return true;
}
