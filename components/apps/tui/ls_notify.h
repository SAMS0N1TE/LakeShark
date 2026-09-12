/* Something happened while you were looking at something else. */

#ifndef LS_NOTIFY_H
#define LS_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "ls_tui.h"
#include "ls_tui_screen.h"
#include "tuilib/tui_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LS_NOTIFY_TITLE 20
#define LS_NOTIFY_BODY  64

typedef struct {
    char    title[LS_NOTIFY_TITLE];  /* who: "MESH", "KB1QWE"          */
    char    body[LS_NOTIFY_BODY];    /* what: the message, or a summary */
    uint8_t hue;                     /* the source app's colour         */
    /* Which screen to open when the banner is tapped, or -1 for none.
       A notice you cannot act on is a notice you have to remember. */
    int     screen;
} ls_notice_t;

/* Report a notice, or return false when nothing has happened. Called once a
   frame from the draw path, so it must be cheap and must not block. */
typedef bool (*ls_notify_probe_t)(ls_notice_t *out);

/* Up to four sources. Installing the same probe twice is ignored, so a
   caller that runs its setup more than once cannot double up the notices. */
void ls_notify_add_probe(ls_notify_probe_t probe);

void ls_notify_poll(int visible);

/* Post one directly. For anything that is not a polled source. */
void ls_notify_post(const ls_notice_t *n);

/* True while a banner is on screen. */
bool ls_notify_showing(void);

/* How many notices have arrived and not been looked at. Drawn in the status
   row, and it is the half that persists: the banner is a few seconds and the
   count is until you go and read them. */
int  ls_notify_unread(void);

/* Whether the unread badge should be drawn inverted this frame. */

bool ls_notify_badge_inverted(void);
void ls_notify_clear(void);

/* The router draws the banner last, over the screen's own area, and gives it
   input first. Both return true when the banner consumed the input. A tap on
   the banner opens the screen it came from; a tap on its X, or ESC, or
   letting it time out, dismisses it. */
void ls_notify_draw(tui_surface *sf, tui_rect area);
bool ls_notify_touch(int col, int row);
bool ls_notify_key(ls_tk_t key);

/* Alerts, and what they are honestly worth today. */

bool ls_notify_ring(void);
bool ls_notify_vibe(void);
void ls_notify_set_alerts(bool ring, bool vibe);

void ls_notify_alert_hw(bool ring, bool vibe);

#ifdef __cplusplus
}
#endif

#endif /* LS_NOTIFY_H */
