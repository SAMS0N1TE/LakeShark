/* Settings: a first page of what changes often, and menus for the rest. */

#include "../../ls_tui_screen.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_theme.h"
#include "../../ls_tui_ui.h"
#include "core/settings.h"
#include "ls_keypad.h"
/* Brightness and auto-dim go through display_ctl, which applies as
   well as stores. */
#include "core/display_ctl.h"
#include "../../ls_notify.h"
/* Whether speech exists, so the spoken greeting and voice are offered only then. */
#include "audio/speech.h"
#include "audio/audio_out.h"
#include "audio/audio_events.h"
#include "../../ls_voice_opts.h"

static int s_sel;

typedef struct {
    const char *label;
    void (*show)(char *buf, size_t len);
    void (*next)(void);
    /* A level: drawn with < and >, which a tap or LEFT/RIGHT step through
       without wrapping. NULL for a setting that cycles. */
    void (*step)(int dir);
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
/* A level too: < and > step by ten and stop at 5 and 100, so the panel is
   never stepped dark. */
static void bri_step(int dir)
{
    const int v = display_ctl_get_user() + dir * 10;
    display_ctl_set_user(v < 5 ? 5 : v > 100 ? 100 : v);
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
/* The live volume, not the stored one: this used to write only the saved
   setting, so the box changed and the speaker (spoken callouts included) did
   not until the next boot. audio_volume_set() moves the codec and saves. */
static void vol_show(char *b, size_t n) { snprintf(b, n, "%d %%", audio_volume_get()); }
static void vol_step(int dir)
{
    const int v = audio_volume_get() + dir * 5;
    audio_volume_set(v < 0 ? 0 : v > 100 ? 100 : v);
    /* Turning the volume is asking to hear it. */
    if (audio_is_muted()) audio_toggle_mute();
}
static void vol_next(void) { vol_step(+1); }

static void mute_show(char *b, size_t n) { snprintf(b, n, "%s", audio_is_muted() ? "MUTED" : "sound on"); }
static void mute_next(void) { audio_toggle_mute(); }

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
static void keylight_show(char *b,size_t n) { snprintf(b,n,"%s",settings_get_keyboard_light()?"on":"off"); }
static void keylight_next(void)
{
    const bool on=!settings_get_keyboard_light();
    if(ls_keypad_backlight(on)==ESP_OK) settings_set_keyboard_light(on);
}

/* The words have to be the firmware's words. */

static void boot_show(char *b, size_t n)
{
    static const char *M[] = { "off", "chime", "spoken" };
    int m = settings_get_boot_sound();
    if (m == 2 && !speech_available()) m = 1;
    snprintf(b, n, "%s", (m >= 0 && m < 3) ? M[m] : "?");
}
static void boot_next(void)
{
    const int modes = speech_available() ? 3 : 2;
    int m = settings_get_boot_sound();
    if (m == 2 && !speech_available()) m = 1;
    settings_set_boot_sound((m + 1) % modes);
}

static void keydim_show(char *b, size_t n) { snprintf(b, n, "%s", display_ctl_keyboard_dim() ? "with screen" : "off"); }
static void keydim_next(void) { display_ctl_set_keyboard_dim(!display_ctl_keyboard_dim()); }

/* ---- voice -------------------------------------------------------------- */

/* The voice, its level, a test, and what each app says aloud. */

static void voice_show(char *b, size_t n)
{
    if (speech_available())
        snprintf(b, n, "%s %d%%", speech_voice_name(speech_voice_get()), speech_volume_get());
    else
        snprintf(b, n, "off");
}
static void voice_next(void)
{
    ls_opt_open(&ls_voice_ctx_all);
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

static void lock_show(char *b, size_t n) { snprintf(b, n, "lock now"); }
static void lock_next(void) { ls_tui_set_locked(true); }

/* Holds the screen the way it is now: the board stops turning it to match
   how it is held. Attaching the keyboard still turns it. */
static void rotlock_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_auto_rotate() ? "off" : "on"); }
static void rotlock_next(void) { settings_set_auto_rotate(!settings_get_auto_rotate()); }

/* ---- pages ---------------------------------------------------------------
   The first page holds what gets changed often; the rest sit one level down
   in DISPLAY, SOUND and DEVICE, each opened from a box and left by its BACK
   box, ESC or BACKSPACE. */

typedef struct {
    const char   *title;
    const item_t *items;
    int           n;
} set_page_t;

static const set_page_t *page_get(void);
static void page_open(int p);

enum { SETP_ROOT, SETP_DISPLAY, SETP_SOUND, SETP_DEVICE, SETP_COUNT };
static int s_page = SETP_ROOT;

static void back_show(char *b, size_t n) { snprintf(b, n, "< SETTINGS"); }
static void back_next(void) { page_open(SETP_ROOT); }

static void display_show(char *b, size_t n)
{
    char theme[40];
    theme_show(theme, sizeof(theme));
    if (display_ctl_autodim_enabled())
        snprintf(b, n, "%s, dim %d s  >", theme, display_ctl_autodim_timeout());
    else
        snprintf(b, n, "%s  >", theme);
}
static void display_next(void) { page_open(SETP_DISPLAY); }

static void sound_show(char *b, size_t n)
{
    char boot[16], voice[24];
    boot_show(boot, sizeof(boot));
    if (speech_available()) snprintf(voice, sizeof(voice), "%s", speech_voice_name(speech_voice_get()));
    else                    snprintf(voice, sizeof(voice), "no voice");
    snprintf(b, n, "%s, %s, alerts %s  >", boot, voice,
             settings_get_alert_ring() ? "on" : "off");
}
static void sound_next(void) { page_open(SETP_SOUND); }

static void device_show(char *b, size_t n)
{
    snprintf(b, n, "keys %s, USB %s  >", settings_get_keyboard_light() ? "on" : "off",
             settings_get_usb_autoreboot() ? "on" : "off");
}
static void device_next(void) { page_open(SETP_DEVICE); }

static const item_t ROOT_ITEMS[] = {
    { "Volume",         vol_show,     vol_next,     vol_step },
    { "Brightness",     bri_show,     bri_next,     bri_step },
    { "Mute",           mute_show,    mute_next,    NULL },
    { "Screen lock",    lock_show,    lock_next,    NULL },
    { "Rotate lock",    rotlock_show, rotlock_next, NULL },
    { "Display",        display_show, display_next, NULL },
    { "Sound",          sound_show,   sound_next,   NULL },
    { "Device",         device_show,  device_next,  NULL },
};
static const item_t DISPLAY_ITEMS[] = {
    { "BACK",           back_show,  back_next, NULL },
    { "Theme",          theme_show, theme_next, NULL },
    { "Daylight",       day_show,   day_next, NULL },
    { "Font",           font_show,  font_next, NULL },
    { "Auto dim",       dim_show,   dim_next, NULL },
    { "Dim after",      dimt_show,  dimt_next, NULL },
};
static const item_t SOUND_ITEMS[] = {
    { "BACK",           back_show,  back_next, NULL },
    { "Boot sound",     boot_show,  boot_next, NULL },
    { "Voice",          voice_show, voice_next, NULL },
    { "Alert sound",    ring_show,  ring_next, NULL },
    { "Vibrate",        vibe_show,  vibe_next, NULL },
};
static const item_t DEVICE_ITEMS[] = {
    { "BACK",           back_show,     back_next, NULL },
    { "Keyboard light", keylight_show, keylight_next, NULL },
    { "Keyboard dim",   keydim_show,   keydim_next, NULL },
    { "USB autoreboot", usb_show,      usb_next, NULL },
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const set_page_t SET_PAGES[SETP_COUNT] = {
    [SETP_ROOT]    = { "SETTINGS",         ROOT_ITEMS,    COUNT(ROOT_ITEMS) },
    [SETP_DISPLAY] = { "SETTINGS > DISPLAY", DISPLAY_ITEMS, COUNT(DISPLAY_ITEMS) },
    [SETP_SOUND]   = { "SETTINGS > SOUND", SOUND_ITEMS,   COUNT(SOUND_ITEMS) },
    [SETP_DEVICE]  = { "SETTINGS > DEVICE", DEVICE_ITEMS,  COUNT(DEVICE_ITEMS) },
};
#define MAX_ITEMS 8

static const set_page_t *page_get(void) { return &SET_PAGES[s_page]; }

/* Into a page with its first box selected; back to the first page with the
   box that opened the page selected. */
static void page_open(int p)
{
    const int from = s_page;
    s_page = p;
    s_sel = 0;
    if (p == SETP_ROOT && from != SETP_ROOT)
        for (int i = 0; i < COUNT(ROOT_ITEMS); i++)
            if ((from == SETP_DISPLAY && ROOT_ITEMS[i].next == display_next) ||
                (from == SETP_SOUND   && ROOT_ITEMS[i].next == sound_next) ||
                (from == SETP_DEVICE  && ROOT_ITEMS[i].next == device_next))
                s_sel = i;
}

/* Each setting is a box you press, not a row you select. */

static tui_rect s_hit[MAX_ITEMS];
static tui_rect s_dec[MAX_ITEMS], s_inc[MAX_ITEMS];
static int      s_hit_n;

static void draw_one(tui_surface *sf, tui_rect a, const item_t *it, int i, bool sel)
{
    const uint8_t hue  = sel ? (TUI_CYAN | TUI_BRIGHT) : TUI_CYAN;
    const uint8_t edge = TUI_ATTR(hue, TUI_BLACK);
    const uint8_t name = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);

    ls_panel_box(sf, a, NULL, hue);

    tui_put_char(sf, a, a.x + 1, a.y, ' ', edge);
    tui_put_str(sf, a, a.x + 2, a.y, it->label, name);
    tui_put_char(sf, a, a.x + 2 + (int)strlen(it->label), a.y, ' ', edge);

    tui_rect field = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (field.h >= 1 && field.w >= 4) {
        ls_fill_dither(sf, field, sel ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
        /* Room for a sixteen-letter theme name ", after Daylight". */
        char buf[64];
        it->show(buf, sizeof(buf));
        ls_dither_label(sf, field, (field.h - 1) / 2, buf, val);
    }

    s_dec[i].w = s_inc[i].w = 0;
    if (it->step && field.h >= 1 && field.w >= 20) {
        /* < and > as solid keys at either end of the box; a tap anywhere in
           that half of the box steps the same way. */
        const uint8_t key = TUI_ATTR(TUI_BLACK, TUI_CYAN | TUI_BRIGHT);
        const int kw = field.w / 4 > 5 ? field.w / 4 : 5;
        const int mid = field.y + (field.h - 1) / 2;
        s_dec[i] = tui_rect_make(field.x, field.y, kw, field.h);
        s_inc[i] = tui_rect_make(field.x + field.w - kw, field.y, kw, field.h);
        tui_fill(sf, s_dec[i], ' ', key);
        tui_fill(sf, s_inc[i], ' ', key);
        tui_put_char(sf, a, field.x + kw / 2, mid, '<', key);
        tui_put_char(sf, a, field.x + field.w - kw + kw / 2, mid, '>', key);
    }

    if (s_hit_n < MAX_ITEMS)
        s_hit[s_hit_n++] = a;
}

static void draw(tui_surface *sf, tui_rect area)
{
    const set_page_t *pg = page_get();

    s_hit_n = 0;
    ls_panel_box(sf, area, pg->title, TUI_CYAN);
    if (area.h < 6 || area.w < 18) return;

    tui_rect body = tui_rect_make(area.x + 1, area.y + 1,
                                  area.w - 2, area.h - 2);
    if (body.h < 4) return;

    /* Landscape is wide and short: two columns keep the boxes tall enough to
       hit. Portrait has the height for one column. */
    if (ls_tui_is_wide() && body.w >= 44 && pg->n > 4) {
        const int per = (pg->n + 1) / 2;
        const int gap = (per * 4 - 1 > body.h) ? 0 : 1;
        const int height = body.h - (per - 1) * gap;

        tui_rect col[2];
        const int cw = body.w / 2;
        for (int c = 0; c < 2; c++)
            col[c] = tui_rect_make(body.x + c * cw, body.y,
                                   c == 1 ? body.w - cw : cw, body.h);
        for (int i = 0; i < pg->n; i++) {
            const tui_rect c = col[i / per];
            const int row = i % per;
            const int y = c.y + row * height / per + row * gap;
            const int bh = (row + 1) * height / per - row * height / per;
            if (y + bh > c.y + c.h) break;
            draw_one(sf, tui_rect_make(c.x, y, c.w - 1, bh), &pg->items[i], i, i == s_sel);
        }
    } else {
        const int gap = body.h >= pg->n * 4 - 1 ? 1 : 0;
        const int height = body.h - (pg->n - 1) * gap;

        for (int i = 0; i < pg->n; i++) {
            const int y = body.y + i * height / pg->n + i * gap;
            const int bh = (i + 1) * height / pg->n - i * height / pg->n;
            if (y + bh > body.y + body.h) break;
            draw_one(sf, tui_rect_make(body.x, y, body.w, bh), &pg->items[i], i, i == s_sel);
        }
    }
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    const set_page_t *pg = page_get();
    if (s_sel >= pg->n) s_sel = 0;
    switch (k) {
    case LS_TK_UP:    s_sel = (s_sel + pg->n - 1) % pg->n; return true;
    case LS_TK_DOWN:  s_sel = (s_sel + 1) % pg->n;         return true;
    case LS_TK_ENTER: pg->items[s_sel].next();             return true;
    case LS_TK_LEFT:
    case LS_TK_RIGHT:
        if (!pg->items[s_sel].step) return false;
        pg->items[s_sel].step(k == LS_TK_LEFT ? -1 : 1);
        return true;
    case LS_TK_ESC:
    case LS_TK_BACKSPACE:
        if (s_page == SETP_ROOT) return false;
        page_open(SETP_ROOT);
        return true;
    default: return false;
    }
}

/* One press advances the setting. Every item here is a cycle - brightness
   steps, the theme list, on and off - so there is nothing a second press
   would confirm, and asking for one would only make the screen slower to
   use with no safety bought. */
static bool touch(int col, int row)
{
    const set_page_t *pg = page_get();
    for (int i = 0; i < s_hit_n && i < pg->n; i++) {
        const tui_rect r = s_hit[i];
        if (col >= r.x && col < r.x + r.w &&
            row >= r.y && row < r.y + r.h) {
            s_sel = i;
            if (pg->items[i].step) {
                /* A level steps down from the left half of its box and up
                   from the right half, so a finger does not have to find
                   the arrow keys. */
                pg->items[i].step(col < r.x + r.w / 2 ? -1 : 1);
                return true;
            }
            pg->items[i].next();
            return true;
        }
    }
    /* A miss is a miss. */

    return true;
}

/* Settings opens on its first page every time. */
static void enter(void)
{
    s_page = SETP_ROOT;
    s_sel = 0;
}

const ls_tui_screen_t ls_scr_settings = {
    .name = "SET",
    .hint = "TAP to change  UP DOWN ENTER  ESC back",
    .enter = enter,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
