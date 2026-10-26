/* The native app frame before the app draws itself: a card with the app's own
   tile picture, then loading, the question, or the reason it did not open. */
#include "ls_lap_ui.h"
#include "ls_tui_ui.h"
#include "ls_icons.h"
#include <stdio.h>
#include <string.h>

static bool s_asking;   /* the last draw put up OPEN / BACK */

static const uint8_t HUE[8] = { TUI_WHITE, TUI_RED, TUI_GREEN, TUI_YELLOW,
                                TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE };

/* The app's card: its picture on the left, name, line, version and size. */
static int card(tui_surface *sf, tui_rect a, const ls_lap_info_t *app, uint8_t frame)
{
    const int h = LS_ICON_ROWS + 4;
    tui_rect r = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, h);
    tui_box(sf, r, NULL, frame);
    if (!app) return r.y + r.h;
    const uint8_t hue = HUE[app->manifest.color & 7] | TUI_BRIGHT;
    bool art = false;
    for (int y = 0; y < LAP_ICON_H; y++) art |= app->manifest.icon_art[y][0] != 0;
    const int ix = r.x + 2, iy = r.y + 2;
    if (app->has_icon_px)
        ls_icon_draw_px(sf, r, ix, iy, app->icon_px, TUI_ATTR(hue, TUI_BLACK));
    else if (art)
        for (int y = 0; y < LAP_ICON_H; y++)
            tui_put_str(sf, r, ix, iy + y, app->manifest.icon_art[y], TUI_ATTR(hue, TUI_BLACK));
    else
        ls_icon_draw(sf, r, ix, iy, app->manifest.icon > 0 && app->manifest.icon < LS_ICON__COUNT ? app->manifest.icon : LS_ICON_STAR,
                     TUI_ATTR(hue, TUI_BLACK));
    const int tx = ix + LS_ICON_COLS + 3;
    tui_put_str(sf, r, tx, r.y + 2, app->manifest.name, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, r, tx, r.y + 3, app->manifest.sub, TUI_ATTR(TUI_CYAN, TUI_BLACK));
    char line[64];
    snprintf(line, sizeof(line), "v%s", app->manifest.version);
    tui_put_str(sf, r, tx, r.y + 5, line, TUI_ATTR(TUI_YELLOW, TUI_BLACK));
    if (app->manifest.author[0]) {
        snprintf(line, sizeof(line), "by %s", app->manifest.author);
        tui_put_str(sf, r, tx, r.y + 6, line, TUI_ATTR(TUI_MAGENTA, TUI_BLACK));
    }
    snprintf(line, sizeof(line), "%lu KB on the card", (unsigned long)((app->size + 1023) / 1024));
    tui_put_str(sf, r, tx, r.y + 7, line, LS_ATTR_DIM);
    return r.y + r.h;
}

static void lines(tui_surface *sf, tui_rect a, int y, const char *const *text, int n, uint8_t at)
{
    for (int i = 0; i < n && y + i < a.y + a.h; i++)
        ls_safe_line(sf, a, y + i, text[i], at);
}

/* The warning sign, in three colours: a frame, the bar, the dot. */
static void sign(tui_surface *sf, tui_rect a, int x, int y, uint8_t c)
{
    tui_put_str(sf, a, x, y,     "   /\\   ", TUI_ATTR(c, TUI_BLACK));
    tui_put_str(sf, a, x, y + 1, "  /  \\  ", TUI_ATTR(c, TUI_BLACK));
    tui_put_str(sf, a, x, y + 2, " / || \\ ", TUI_ATTR(c, TUI_BLACK));
    tui_put_str(sf, a, x, y + 3, "/  ()  \\", TUI_ATTR(c, TUI_BLACK));
    tui_put_str(sf, a, x, y + 4, "'------'", TUI_ATTR(c, TUI_BLACK));
    tui_put_str(sf, a, x + 3, y + 2, "||", TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, a, x + 3, y + 3, "()", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
}

/* A plain-words hint for the reasons a load fails most often. */
static const char *hint(const char *why)
{
    if (!why || !*why) return NULL;
    if (strstr(why, "needs ") || strstr(why, "symbol")) return "Built for newer firmware. Update LakeShark, then try again.";
    if (strstr(why, "api") || strstr(why, "API")) return "Made for a different app version. Get the current one from the store.";
    if (strstr(why, "signature")) return "The file was changed after it was signed. Install it again.";
    if (strstr(why, "PSRAM") || strstr(why, "memory")) return "Not enough free memory. Close other apps or restart.";
    if (strstr(why, "removed") || strstr(why, "restart")) return "The card changed. Restart the board to refresh the list.";
    return "Copy the app to /apps again, or install it from the store.";
}

void ls_lap_ui_draw(tui_surface *sf, tui_rect area, const ls_lap_info_t *app,
                    const ls_lap_status_t *st, const char *error, bool crashed)
{
    s_asking = false;
    if (area.w < 30 || area.h < 18) {
        ls_safe_line(sf, area, area.y, st && st->state == LS_LAP_LOADING ? "Opening..." : (error ? error : ""), TUI_DEFAULT_ATTR);
        return;
    }
    const bool ask = st && st->state == LS_LAP_FAILED && st->needs_confirmation;
    const bool loading = st && (st->state == LS_LAP_LOADING || st->state == LS_LAP_IDLE);
    const uint8_t frame = TUI_ATTR(ask ? TUI_YELLOW : loading ? TUI_CYAN : TUI_RED, TUI_BLACK);
    int y = card(sf, area, app, frame) + 1;
    tui_rect body = tui_rect_make(area.x + 2, y, area.w - 4, area.y + area.h - y);

    if (loading) {
        /* Real progress only: the app is being read, then checked. */
        ls_safe_line(sf, body, y, "OPENING", TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK));
        static const char *const what[] = {
            "Reading the app from the card, checking its",
            "signature and linking it to the firmware.",
        };
        lines(sf, body, y + 2, what, 2, LS_ATTR_DIM);
        return;
    }

    if (st && st->state == LS_LAP_RUNNING) {
        /* Running without a draw callback: it works in the background. */
        ls_safe_line(sf, body, y, "RUNNING", TUI_ATTR(TUI_GREEN | TUI_BRIGHT, TUI_BLACK));
        ls_safe_line(sf, body, y + 2, "This app draws nothing; it keeps working while", LS_ATTR_DIM);
        ls_safe_line(sf, body, y + 3, "it is open. ESC returns to HOME.", LS_ATTR_DIM);
        return;
    }

    if (ask) {
        sign(sf, body, body.x, y, crashed ? TUI_RED | TUI_BRIGHT : TUI_YELLOW | TUI_BRIGHT);
        const int tx = body.x + 10;
        tui_rect text = tui_rect_make(tx, y, body.x + body.w - tx, 6);
        if (app && (app->manifest.flags & LAP_CAP_NETWORK)) {
            tui_put_str(sf, text, tx, y, "ALLOW NETWORK ACCESS?", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
            if (crashed) tui_put_str(sf, text, tx, y + 1, "It stopped the board last time", TUI_ATTR(TUI_RED, TUI_BLACK));
            static const char *const why[] = {
                "This app can connect to devices and servers",
                "using the network already joined in LINK.",
                "Allow only apps you trust. Permission lasts",
                "until this app closes; BACK denies access.",
            };
            lines(sf, body, y + 6, why, 4, TUI_ATTR(TUI_WHITE, TUI_BLACK));
        } else if (crashed) {
            tui_put_str(sf, text, tx, y, "IT STOPPED THE BOARD", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
            tui_put_str(sf, text, tx, y + 1, "LAST TIME IT RAN", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
            static const char *const why[] = {
                "The board restarted while this app",
                "was drawing or reading a key.",
                "Open it again if you think that was",
                "a one-off; if it happens again,",
                "tell its author.",
            };
            lines(sf, body, y + 6, why, 5, TUI_ATTR(TUI_WHITE, TUI_BLACK));
        } else {
            tui_put_str(sf, text, tx, y, "NOT FROM THE STORE", TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));
            tui_put_str(sf, text, tx, y + 1, "no LakeShark signature", TUI_ATTR(TUI_YELLOW, TUI_BLACK));
            static const char *const why[] = {
                "An app runs with the whole board: the card,",
                "the screen, every value the radios publish.",
                "Open apps from people you trust. Store apps",
                "are signed and open without this question.",
            };
            lines(sf, body, y + 6, why, 4, TUI_ATTR(TUI_WHITE, TUI_BLACK));
        }
        const int bh = 4, by = area.y + area.h - bh - 1;
        ls_btn_t b[2] = {
            { app && (app->manifest.flags & LAP_CAP_NETWORK) ? "ALLOW & OPEN" : crashed ? "OPEN AGAIN" : "OPEN ANYWAY", "ENTER", '\r', false, false, false },
            { "BACK", "ESC", 0, false, false, false },
        };
        ls_btn_bar_raised_slot(sf, tui_rect_make(area.x + 1, by, area.w - 2, bh), b, 2, -1, LS_BTN_SLOT_SCREEN);
        s_asking = true;
        return;
    }

    /* Failed: what happened, in the board's words and in plain ones. */
    tui_put_str(sf, body, body.x, y,     " __  __ ", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, body, body.x, y + 1, " \\ \\/ / ", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
    tui_put_str(sf, body, body.x, y + 2, "  >  <  ", TUI_ATTR(TUI_RED, TUI_BLACK));
    tui_put_str(sf, body, body.x, y + 3, " /_/\\_\\ ", TUI_ATTR(TUI_RED, TUI_BLACK));
    tui_put_str(sf, body, body.x + 10, y, "COULD NOT OPEN", TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK));
    tui_rect why = tui_rect_make(body.x + 10, y + 1, body.w - 10, 2);
    tui_put_str(sf, why, why.x, y + 1, error && *error ? error : "unknown error", TUI_ATTR(TUI_YELLOW, TUI_BLACK));
    const char *h = hint(error);
    if (h) {
        char a[64], b2[64];
        const int w = body.w < 63 ? body.w : 63;
        int cut = (int)strlen(h) <= w ? (int)strlen(h) : w;
        while (cut > 0 && cut < (int)strlen(h) && h[cut] != ' ') cut--;
        snprintf(a, sizeof(a), "%.*s", cut, h);
        snprintf(b2, sizeof(b2), "%s", h[cut] ? h + cut + 1 : "");
        const char *const two[] = { a, b2 };
        lines(sf, body, y + 5, two, 2, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    }
    const int bh = 4, by = area.y + area.h - bh - 1;
    ls_btn_t b[1] = { { "BACK TO HOME", "ESC", 0, false, false, false } };
    ls_btn_bar_raised_slot(sf, tui_rect_make(area.x + 1, by, area.w - 2, bh), b, 1, -1, LS_BTN_SLOT_SCREEN);
}

ls_lap_ui_act_t ls_lap_ui_touch(int col, int row)
{
    const int hit = ls_btn_hit_slot(col, row, LS_BTN_SLOT_SCREEN);
    if (hit < 0) return LS_LAP_UI_NONE;
    if (!s_asking) return LS_LAP_UI_BACK;
    return hit == 0 ? LS_LAP_UI_OPEN : LS_LAP_UI_BACK;
}

ls_lap_ui_act_t ls_lap_ui_key(ls_tk_t k, char ch, const ls_lap_status_t *st)
{
    (void)ch;
    const bool ask = st && st->state == LS_LAP_FAILED && st->needs_confirmation;
    if (ask && k == LS_TK_ENTER) return LS_LAP_UI_OPEN;
    return LS_LAP_UI_NONE;   /* ESC falls through to the router: HOME */
}
