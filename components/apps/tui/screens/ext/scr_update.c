/* UPDATE: updates over WiFi, an app of its own in SYSTEM. The page is
   ls_update_view; Settings > DEVICE > Update opens this app too. */

#include "../../ls_app.h"
#include "../../ls_tui_screen.h"
#include "../../ls_update_view.h"

static void enter(void) { ls_update_view_enter(); }

static void draw(tui_surface *sf, tui_rect a) { ls_update_view_draw(sf, a); }

/* Only ENTER is the page's; ESC (HOME) and the F-keys go on to the router. */
static bool key(ls_tk_t k, char ch) { return ls_update_view_key(k, ch); }

static bool touch(int col, int row)
{
    if (ls_update_view_touch(col, row)) {                 /* BACK */
        const ls_app_t *home = ls_app_by_id("home");
        ls_tui_screen_show(home ? ls_tui_screen_index_of(home->screen) : 0);
    }
    return true;
}

const ls_tui_screen_t ls_scr_update = {
    .name  = "UPDATE",
    .hint  = "TAP INSTALL or ENTER  ESC home",
    .enter = enter,
    .leave = NULL,
    .draw  = draw,
    .key   = key,
    .touch = touch,
};
