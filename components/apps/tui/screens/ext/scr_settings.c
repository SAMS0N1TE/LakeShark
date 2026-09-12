/* Settings: a list, driven by three keys. */

#include "../../ls_tui_screen.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_theme.h"
#include "../../ls_tui_ui.h"
#include "core/settings.h"
/* Brightness and auto-dim go through display_ctl, which applies as
   well as stores. */
#include "core/display_ctl.h"
#include "../../ls_notify.h"
/* Whether a spoken greeting can exist, so it is offered only then. */
#include "audio/sam_tts.h"

static int s_sel;

typedef struct {
    const char *label;
    void (*show)(char *buf, size_t len);
    void (*next)(void);
} item_t;

/* ---- brightness --------------------------------------------------------
   Through display_ctl. These rows wrote the stored value alone, and
   display_ctl reads the store once at boot, so a brightness change made here
   reached the panel on the next power-up and auto-dim never heard of any of
   them. */
static void bri_show(char *b, size_t n) { snprintf(b, n, "%d %%", display_ctl_get_user()); }
static void bri_next(void)
{
    int v = display_ctl_get_user() + 20;

    if (v > 100) v = 5;
    display_ctl_set_user(v);
}

/* ---- auto dim ---------------------------------------------------------- */
static void dim_show(char *b, size_t n) { snprintf(b, n, "%s", display_ctl_autodim_enabled() ? "on" : "off"); }
static void dim_next(void) { display_ctl_set_autodim(!display_ctl_autodim_enabled()); }

static void dimt_show(char *b, size_t n) { snprintf(b, n, "%d s", display_ctl_autodim_timeout()); }
static void dimt_next(void)
{
    /* Only steps the store keeps. This list had 300 and 0 on it and
       the store clamps to 5..240, so 120 stepped to a stored 240 that was not
       on the list and the next press fell back to 15 - two entries of the
       cycle could never be shown, and "never" could never be set. */
    static const int STEPS[] = { 15, 30, 60, 120, 240 };
    int cur = display_ctl_autodim_timeout();
    for (unsigned i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++) {
        if (STEPS[i] == cur) {
            display_ctl_set_autodim_timeout(STEPS[(i + 1) % (sizeof(STEPS) / sizeof(STEPS[0]))]);
            return;
        }
    }
    display_ctl_set_autodim_timeout(STEPS[0]);
}

/* ---- volume ------------------------------------------------------------ */
static void vol_show(char *b, size_t n) { snprintf(b, n, "%d %%", settings_get_volume()); }
static void vol_next(void)
{
    int v = settings_get_volume() + 10;
    if (v > 100) v = 0;
    settings_set_volume(v);
}

static void theme_show(char *b, size_t n)
{
    const ls_tui_theme_t *t = ls_tui_get_theme();
    const char *name = t && t->name ? t->name : "?";
    if (ls_tui_daylight()) snprintf(b, n, "%s, after Daylight", name);
    else                   snprintf(b, n, "%s", name);
}
static void theme_next(void)
{
    const ls_tui_theme_t *nx = ls_tui_theme_next(ls_tui_get_theme());
    if (!nx) return;
    ls_tui_set_theme(nx);
    settings_set_theme(ls_tui_theme_index(nx));
}

/* ---- daylight ----------------------------------------------------------
   "These amoleds are kinda dim in sunlight, will need a toggle for
   a daylight high contrast mode" - "So white theme, not black". Live and
   stored, like the theme, and it never forgets which theme it is covering.
   See ls_theme.h for why it is a toggle and not a theme. */
static void day_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tui_daylight() ? "on" : "off"); }
static void day_next(void)
{
    const bool on = !ls_tui_daylight();
    ls_tui_set_daylight(on);
    settings_set_daylight(on);
}

/* ---- misc booleans ----------------------------------------------------- */
static void usb_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_usb_autoreboot() ? "on" : "off"); }
static void usb_next(void) { settings_set_usb_autoreboot(!settings_get_usb_autoreboot()); }

/* The words have to be the firmware's words. */

static void boot_show(char *b, size_t n)
{
    static const char *M[] = { "off", "chime", "spoken" };
    int m = settings_get_boot_sound();
    if (m == 2 && !sam_tts_available()) m = 1;
    snprintf(b, n, "%s", (m >= 0 && m < 3) ? M[m] : "?");
}
static void boot_next(void)
{
    const int modes = sam_tts_available() ? 3 : 2;
    int m = settings_get_boot_sound();
    if (m == 2 && !sam_tts_available()) m = 1;
    settings_set_boot_sound((m + 1) % modes);
}

/* ---- font --------------------------------------------------------------- */

static void font_show(char *b, size_t n)
{
    snprintf(b, n, "%s", ls_tui_font_label(ls_tui_font_index()));
}
static void font_next(void)
{
    const int next = (ls_tui_font_index() + 1) % ls_tui_font_count();
    ls_tui_set_font_index(next);
    ls_tui_screen_request_regrid();
}

/* ---- alerts ------------------------------------------------------------- */

static void ring_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_alert_ring() ? "on" : "off"); }
static void ring_next(void)
{
    const bool en = !settings_get_alert_ring();
    settings_set_alert_ring(en);
    ls_notify_set_alerts(en, settings_get_alert_vibe());
}
static void vibe_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_alert_vibe() ? "on" : "off"); }
static void vibe_next(void)
{
    const bool en = !settings_get_alert_vibe();
    settings_set_alert_vibe(en);
    ls_notify_set_alerts(settings_get_alert_ring(), en);
}

static const item_t ITEMS[] = {
    { "Brightness",     bri_show,   bri_next   },
    { "Auto dim",       dim_show,   dim_next   },
    { "Dim after",      dimt_show,  dimt_next  },
    { "Volume",         vol_show,   vol_next   },
    { "Theme",          theme_show, theme_next },
    { "Daylight",       day_show,   day_next   },
    { "Font",           font_show,  font_next  },
    { "Boot sound",     boot_show,  boot_next  },
    { "Alert sound",    ring_show,  ring_next  },
    { "Vibrate",        vibe_show,  vibe_next  },
    { "USB autoreboot", usb_show,   usb_next   },
};
#define N_ITEMS ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

/* Each setting is a box you press, not a row you select. */

static tui_rect s_hit[12];
static int      s_hit_n;

static void draw_one(tui_surface *sf, tui_rect a, int i, bool sel)
{
    const uint8_t hue  = sel ? (TUI_CYAN | TUI_BRIGHT) : TUI_CYAN;
    const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
    const uint8_t name = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);

    ls_panel_box(sf, a, NULL, hue);

    tui_put_char(sf, a, a.x + 1, a.y, ' ', edge);
    tui_put_str(sf, a, a.x + 2, a.y, ITEMS[i].label, name);
    tui_put_char(sf, a, a.x + 2 + (int)strlen(ITEMS[i].label), a.y, ' ', edge);

    tui_rect field = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (field.h >= 1 && field.w >= 4) {
        ls_fill_dither(sf, field, sel ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
        /* Room for a sixteen-letter theme name ", after Daylight". */
        char buf[40];
        ITEMS[i].show(buf, sizeof(buf));
        ls_dither_label(sf, field, (field.h - 1) / 2, buf, val);
    }

    if (s_hit_n < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
        s_hit[s_hit_n++] = a;
}

static void draw(tui_surface *sf, tui_rect area)
{
    const uint8_t dim = LS_ATTR_DIM;

    s_hit_n = 0;
    ls_panel_box(sf, area, "SETTINGS", TUI_CYAN);
    if (area.h < 6 || area.w < 18) return;

    tui_rect body = tui_rect_make(area.x + 1, area.y + 1,
                                  area.w - 2, area.h - 3);
    if (body.h < 4) return;

    /* Landscape is wide and short: two columns keep the boxes tall enough to
       hit. Portrait has the height for one column of eight. */
    if (ls_tui_is_wide() && body.w >= 44) {
        /* Two columns, or three when three make the taller box. */

        const int per2 = (N_ITEMS + 1) / 2, per3 = (N_ITEMS + 2) / 3;
        const int ncol = (body.w / 3 >= 22 &&
                          (body.h - (per3 - 1)) / per3 >
                          (body.h - (per2 - 1)) / per2) ? 3 : 2;
        const int per = ncol == 3 ? per3 : per2;
        const int gap = (per * 4 - 1 > body.h) ? 0 : 1;
        int bh = (body.h - (per - 1) * gap) / per;
        if (bh > 6) bh = 6;
        if (bh < 3) bh = 3;

        tui_rect col[3];
        const int cw = body.w / ncol;
        for (int c = 0; c < ncol; c++)
            col[c] = tui_rect_make(body.x + c * cw, body.y,
                                   c == ncol - 1 ? body.w - c * cw : cw, body.h);
        for (int i = 0; i < N_ITEMS; i++) {
            const tui_rect c = col[i / per];
            const int y = c.y + (i % per) * (bh + gap);
            if (y + bh > c.y + c.h) break;
            draw_one(sf, tui_rect_make(c.x, y, c.w - 1, bh), i, i == s_sel);
        }
    } else {
        int bh = (body.h - (N_ITEMS - 1)) / N_ITEMS;
        if (bh > 7) bh = 7;
        if (bh < 3) bh = 3;

        const int used = N_ITEMS * bh + (N_ITEMS - 1);
        const int top = body.y + (body.h > used ? (body.h - used) / 2 : 0);

        for (int i = 0; i < N_ITEMS; i++) {
            const int y = top + i * (bh + 1);
            if (y + bh > body.y + body.h) break;
            draw_one(sf, tui_rect_make(body.x, y, body.w, bh), i, i == s_sel);
        }
    }

    tui_put_str(sf, area, area.x + 2, area.y + area.h - 2,
                "TAP a setting to change it", dim);
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    switch (k) {
    case LS_TK_UP:    s_sel = (s_sel + N_ITEMS - 1) % N_ITEMS; return true;
    case LS_TK_DOWN:  s_sel = (s_sel + 1) % N_ITEMS;           return true;
    case LS_TK_ENTER: ITEMS[s_sel].next();                     return true;
    default: return false;
    }
}

/* One press advances the setting. Every item here is a cycle - brightness
   steps, the theme list, on and off - so there is nothing a second press
   would confirm, and asking for one would only make the screen slower to
   use with no safety bought. */
static bool touch(int col, int row)
{
    for (int i = 0; i < s_hit_n && i < N_ITEMS; i++) {
        const tui_rect r = s_hit[i];
        if (col >= r.x && col < r.x + r.w &&
            row >= r.y && row < r.y + r.h) {
            s_sel = i;
            ITEMS[i].next();
            return true;
        }
    }
    /* A miss is a miss. */

    return true;
}

const ls_tui_screen_t ls_scr_settings = {
    .name = "SET",
    .hint = "TAP to change  UP DOWN ENTER",
    .enter = NULL,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
