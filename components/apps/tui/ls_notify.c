/* See ls_notify.h. The banner, the count, and the polling behind them. */
#include "ls_notify.h"

#include <stdio.h>
#include <string.h>

#include "ls_tui_ui.h"
#ifdef ESP_PLATFORM
#include "ls_haptic.h"
#endif

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
/* The screen the unread MESSAGES came from. A system notice (accent) replaces
   s_now for its few seconds but is not a message: it neither counts nor moves this. */
static int         s_unread_screen = -1;
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

static void post(const ls_notice_t *n, bool alert)
{
    if (!n) return;
    s_now = *n;
    s_ttl = SHOW_FRAMES;
    if (!n->accent) {
        if (s_unread < 999) s_unread++;
        s_unread_screen = n->screen;
    }
    if (alert && !n->quiet) {
        if (n->haptic_only) {
#ifdef ESP_PLATFORM
            if (s_vibe) ls_haptic_play(LS_HAPTIC_ALERT);
#else
            ls_notify_alert_hw(false, s_vibe);
#endif
        } else ls_notify_alert_hw(s_ring, s_vibe);
    }
    /* The banner covers rows the screen underneath already drew, and the
       cell renderer only blits what changed - so without this the first
       frame of a banner over a static screen would be the only one that
       reached the panel, and dismissing it would leave the banner painted.
       The same reason gives for the animation. */
    ls_tui_invalidate();
}

void ls_notify_post(const ls_notice_t *n) { post(n, true); }
void ls_notify_post_quiet(const ls_notice_t *n) { post(n, false); }

void ls_notify_poll(int visible)
{
    s_frame++;
    if (s_ttl > 0) {
        s_ttl--;
        if (!s_ttl) {
            ls_tui_invalidate();
            /* A notice with nowhere to go is read when its banner ends, because there is no other way to read it. */

            if (!s_now.accent && s_now.screen < 0 && s_unread) s_unread = 0;
        }
    }

    /* AND A COUNT IS CLEARED BY LOOKING AT WHAT IT IS COUNTING. */

    if (s_unread && s_unread_screen >= 0 && s_unread_screen == visible) {
        s_unread = 0;
        ls_tui_invalidate();
    }

    for (int i = 0; i < s_probes; i++) {
        ls_notice_t n;
        memset(&n, 0, sizeof(n));
        n.screen = -1;
        if (!s_probe[i](&n)) continue;

        if (!n.notify_visible && n.screen >= 0 && n.screen == visible) continue;
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

#ifndef LS_NOTIFY_EXTERNAL_ALERT_HW
__attribute__((weak))
void ls_notify_alert_hw(bool ring, bool vibe) { (void)ring; (void)vibe; }
#endif

/* ---------------------------------------------------------------- draw -- */

/* The cell at step `i` of the way round the box's edge, clockwise from the
   top-left corner, and the character the box has there. */
static void edge_at(tui_rect r, int i, int *x, int *y, char *ch)
{
    const int w = r.w, h = r.h;
    const int top = w - 1, right = h - 1, bottom = w - 1;
    if (i < top)                      { *x = r.x + i;               *y = r.y; }
    else if (i < top + right)         { *x = r.x + w - 1;           *y = r.y + (i - top); }
    else if (i < top + right + bottom){ *x = r.x + w - 1 - (i - top - right); *y = r.y + h - 1; }
    else                              { *x = r.x;                   *y = r.y + h - 1 - (i - top - right - bottom); }
    const bool corner = (*x == r.x || *x == r.x + w - 1) && (*y == r.y || *y == r.y + h - 1);
    *ch = corner ? '+' : (*y == r.y || *y == r.y + h - 1) ? '-' : '|';
}

void ls_notify_draw(tui_surface *sf, tui_rect area)
{
    s_rect = tui_rect_make(0, -1, 0, 0);
    s_close = tui_rect_make(0, -1, 0, 0);
    if (s_ttl <= 0 || area.w < 20 || area.h < 5) return;

    /* Four rows at the TOP of the screen's area, under the chrome: that is
       where every device anyone has held puts one, and the bottom of this
       interface is where the controls are. Small, and made to stand out by
       moving: a light runs round its red edge for as long as it is up. */
    const int full = 4;

    const int age = SHOW_FRAMES - s_ttl;
    int h = full;
    if (age < SLIDE_FRAMES)   h = 1 + age;
    if (s_ttl < SLIDE_FRAMES) h = s_ttl;
    if (h > full) h = full;
    if (h < 1) h = 1;

    tui_rect r = tui_rect_make(area.x, area.y, area.w, h);
    s_rect = r;

    /* Messages are red; a system notice (UPDATE) brings its own colour. */
    const uint8_t hue = s_now.accent ? s_now.accent : TUI_RED;
    tui_fill(sf, r, ' ', A(TUI_WHITE, TUI_BLACK));
    ls_panel_box(sf, r, NULL, hue | TUI_BRIGHT);

    if (h == full) {
        /* "name: message" is split so who sent it sits in the title bar. */
        const char *msg = s_now.body;
        char head[LS_NOTIFY_TITLE + LS_NOTIFY_BODY + 8];
        const char *colon = strstr(s_now.body, ": ");
        if (!s_now.accent && colon && colon - s_now.body < 40) {
            snprintf(head, sizeof(head), "%s  FROM %.*s", s_now.title,
                     (int)(colon - s_now.body), s_now.body);
            msg = colon + 2;
        } else {
            snprintf(head, sizeof(head), "%s", s_now.title);
        }

        /* The bar pulses red and white for its first two seconds. */
        const bool pulse = age < FLASH_FRAMES * 2 && ((age / 5) & 1);
        const uint8_t bar = pulse ? A(hue, TUI_WHITE | TUI_BRIGHT)
                                  : A(TUI_WHITE | TUI_BRIGHT, hue);
        tui_fill(sf, tui_rect_make(r.x + 1, r.y + 1, r.w - 2, 1), ' ', bar);
        tui_put_str(sf, r, r.x + 2, r.y + 1, head, bar);
        tui_put_str(sf, r, r.x + r.w - 5, r.y + 1, "[X]", bar);

        /* A message longer than the line scrolls through it, pausing at
           each end so both are readable. */
        const int width = r.w - 4;
        const int len = (int)strlen(msg);
        int off = 0;
        if (len > width) {
            const int travel = len - width, pause = 20;
            const int t = (age / 3) % (travel + 2 * pause);
            off = t < pause ? 0 : t < pause + travel ? t - pause : travel;
        }
        char line[LS_NOTIFY_BODY];
        snprintf(line, sizeof(line), "%.*s", width, msg + off);
        tui_put_str(sf, r, r.x + 2, r.y + 2, line, A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));

        /* The light: sixteen heavy cells travelling round the edge, white
           at the head, yellow behind it (the notice's own colour for a system
           notice). */
        const uint8_t tail = s_now.accent ? (uint8_t)(hue | TUI_BRIGHT) : (uint8_t)(TUI_YELLOW | TUI_BRIGHT);
        const int perim = 2 * (r.w - 1) + 2 * (r.h - 1);
        const int head_at = (age * 3) % perim;
        for (int k = 0; k < 16; k++) {
            int x, y;
            char ch;
            edge_at(r, (head_at - k + perim) % perim, &x, &y, &ch);
            ch = ch == '-' ? '=' : '#';
            tui_put_char(sf, r, x, y, ch,
                         A(k < 5 ? (TUI_WHITE | TUI_BRIGHT) : tail, TUI_BLACK));
        }
    }

    /* A dismiss target of its own, so tapping the banner can mean "take me
       there" without that being the only thing a tap can mean. */
    s_close = tui_rect_make(r.x + r.w - 7, r.y, 7, r.h);
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
        if (!s_now.accent) s_unread = 0;
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
