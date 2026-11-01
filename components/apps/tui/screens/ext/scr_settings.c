/* Settings: a first page of what changes often, and menus for the rest. */

#include "../../ls_tui_screen.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_theme.h"
#include "../../ls_tui_ui.h"
#include "../../ls_box.h"
#include "../../ls_icons.h"
#include "core/settings.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "ls_keypad.h"
#include "ls_lora_port.h"
/* Brightness and auto-dim go through display_ctl, which applies as
   well as stores. */
#include "core/display_ctl.h"
#include "../../ls_notify.h"
/* Whether speech exists, so the spoken greeting and voice are offered only then. */
#include "audio/speech.h"
#include "audio/tone.h"
#include "audio/audio_out.h"
#include "audio/audio_events.h"
#include "../../ls_voice_opts.h"
#include "../../ls_app.h"
#include "../../ls_value.h"
#include "../../ls_i18n.h"
#include "../../ls_action.h"
#include "../../ls_quick.h"

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
static void dim_show(char *b, size_t n) { snprintf(b, n, "%s", display_ctl_autodim_enabled() ? ls_tr("on") : ls_tr("off")); }
static void dim_next(void) { display_ctl_set_autodim(!display_ctl_autodim_enabled()); }

static void dimt_show(char *b, size_t n) { snprintf(b, n, ls_tr("%d s"), display_ctl_autodim_timeout()); }
static void dimt_step(int dir)
{
    static const int STEPS[] = { 15, 30, 60, 120, 240 };
    const int cur = display_ctl_autodim_timeout();
    if (dir > 0) {
        for (unsigned i = 0; i < sizeof(STEPS) / sizeof(STEPS[0]); i++)
            if (STEPS[i] > cur) { display_ctl_set_autodim_timeout(STEPS[i]); return; }
    } else {
        for (int i = (int)(sizeof(STEPS) / sizeof(STEPS[0])) - 1; i >= 0; i--)
            if (STEPS[i] < cur) { display_ctl_set_autodim_timeout(STEPS[i]); return; }
    }
}
static void dimt_next(void) { dimt_step(+1); }

/* ---- volume ------------------------------------------------------------ */
/* The live volume, not the stored one: this used to write only the saved
   setting, so the box changed and the speaker (spoken callouts included) did
   not until the next boot. audio_volume_set() moves the codec and saves. */
static void vol_show(char *b, size_t n) { snprintf(b, n, "%d %%", audio_volume_get()); }
static void vol_step(int dir)
{
    const int v = audio_volume_get() + dir * 5;
    audio_volume_set(v < 0 ? 0 : v > 100 ? 100 : v);
}
static void vol_next(void) { vol_step(+1); }

static void mute_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tr(audio_is_muted() ? "MUTED" : "sound on")); }
static void mute_next(void) { audio_toggle_mute(); }

static void theme_show(char *b, size_t n)
{
    const ls_tui_theme_t *t = ls_tui_get_theme();
    const char *name = t && t->name ? t->name : "?";
    if (ls_tui_daylight()) snprintf(b, n, ls_tr("%s, after Daylight"), name);
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
static void day_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tui_daylight() ? ls_tr("on") : ls_tr("off")); }
static void day_next(void)
{
    const bool on = !ls_tui_daylight();
    ls_tui_set_daylight(on);
    settings_set_daylight(on);
}

/* ---- misc booleans ----------------------------------------------------- */
static void usb_show(char *b, size_t n) { snprintf(b, n, "%s", app_usb_autoreboot() ? ls_tr("on") : ls_tr("off")); }
static void usb_next(void) { app_set_usb_autoreboot(!app_usb_autoreboot()); }
static void keylight_show(char *b,size_t n) { snprintf(b,n,"%s",ls_tr(settings_get_keyboard_light()?"on":"off")); }
static void keylight_next(void)
{
    const bool on=!settings_get_keyboard_light();
    if(ls_keypad_backlight(on)==ESP_OK) settings_set_keyboard_light(on);
}

/* The words have to be the firmware's words. */

static void boot_show(char *b, size_t n)
{
    static const char *M[] = { "off", "chirp", "spoken" };
    int m = settings_get_boot_sound();
    if (m == 2 && !speech_available()) m = 1;
    snprintf(b, n, "%s", (m >= 0 && m < 3) ? ls_tr(M[m]) : "?");
}
static void boot_next(void)
{
    const int modes = speech_available() ? 3 : 2;
    int m = settings_get_boot_sound();
    if (m == 2 && !speech_available()) m = 1;
    settings_set_boot_sound((m + 1) % modes);
}

static void keydim_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tr(display_ctl_keyboard_dim() ? "with screen" : "off")); }
static void keydim_next(void) { display_ctl_set_keyboard_dim(!display_ctl_keyboard_dim()); }

/* ---- voice -------------------------------------------------------------- */

/* The voice, its level, a test, and what each app says aloud. */

static void voice_show(char *b, size_t n)
{
    if (speech_available())
        snprintf(b, n, "%s %d%%", speech_voice_name(speech_voice_get()), speech_volume_get());
    else
        snprintf(b, n, "%s", ls_tr("off"));
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

static void ring_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_alert_ring() ? ls_tr("on") : ls_tr("off")); }
static void ring_next(void)
{
    const bool en = !settings_get_alert_ring();
    settings_set_alert_ring(en);
    ls_notify_set_alerts(settings_get_alert_ring(), ls_notify_vibe());
}
/* Which sound a notification makes. Tapping cycles it and plays the new one,
   so the choice is heard rather than read. */
static void nsound_show(char *b, size_t n)
{
    int count = 0, cur = 0;
    const char *const *names = settings_notification_sound_names(&count, &cur);
    if (!names || cur < 0 || cur >= count) { snprintf(b, n, "?"); return; }
    size_t i = 0;
    for (; names[cur][i] && i + 1 < n; i++)
        b[i] = (char)(names[cur][i] >= 'A' && names[cur][i] <= 'Z' ? names[cur][i] + 32 : names[cur][i]);
    b[i] = 0;
}
static void nsound_next(void)
{
    settings_cycle_notification_sound();
    (void)snd_alert_start();
}
static void vibe_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_alert_vibe() ? ls_tr("on") : ls_tr("off")); }
static void vibe_next(void)
{
    const bool en = !settings_get_alert_vibe();
    settings_set_alert_vibe(en);
    ls_notify_set_alerts(ls_notify_ring(), settings_get_alert_vibe());
}

/* ---- update ------------------------------------------------------------- */

/* Updates over WiFi live in main and have an app of their own, UPDATE in
   SYSTEM. This row shows where they stand and opens that app. */
static void update_show(char *b, size_t n)
{
    ls_val_t v;
    if (ls_value_read("ota.state", &v, NULL) && v.kind == LS_VAL_TEXT && v.s)
        snprintf(b, n, "%s", ls_tr(v.s));
    else
        snprintf(b, n, "%s", ls_tr("not in this build"));
}
/* ENTER opens UPDATE, which checks as it opens. */
static void update_next(void)
{
    for (int k = 0; k < ls_app_count(); k++) {
        const ls_app_t *app = ls_app_at(k);
        if (app && app->id && !strcmp(app->id, "update")) { ls_app_open(k); return; }
    }
}

/* Daily or Off. Off means the board never looks for an update by itself,
   not even a quiet one; the UPDATE app's CHECK still works. */
static void updchk_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tr(settings_get_update_check() ? "Daily" : "Off")); }
static void updchk_next(void) { settings_set_update_check(!settings_get_update_check()); }

/* Where the LoRa radio is wired: the board's socket or a module on the
   header. With nothing else saved the header is the board's CON1 module;
   'lora port' sets other pins. A change waits for a reboot, and the header
   takes the keyboard's pins when its wiring needs them. */
static void loraport_show(char *b, size_t n)
{
    ls_lora_port_t next;
    ls_lora_port_saved(&next);
    const ls_lora_port_t *now = ls_lora_port();
    const char *what = ls_tr(!next.header ? "onboard"
                     : ls_lora_port_is_con1(&next) ? "header CON1"
                     : ls_lora_port_on_socket(&next) ? "header, socket pins" : "header, own pins");
    /* the whole saved port against what this boot runs: a changed table or
       TCXO waits for a reboot just as a changed pin does */
    ls_lora_port_t shown = next;
    if (!shown.header) ls_lora_port_board(&shown);
    if (!ls_lora_port_same(&shown, now)) snprintf(b, n, ls_tr("%s, reboot to apply"), what);
    else snprintf(b, n, "%s%s", what, ls_lora_port_blocks_keyboard() ? ls_tr(", keyboard off") : "");
}
static void loraport_next(void)
{
    /* the row shows the result: a refused save leaves it as it was */
    char why[96];
    (void)ls_lora_port_toggle(why, sizeof(why));
}

/* Which T-LR2021 the header module is: the V0.3 and V1.0 switch table or the
   older V0.2 one. Custom pins from 'lora port' are shown and never changed
   from here. */
static void loramod_show(char *b, size_t n)
{
    ls_lora_port_t next;
    ls_lora_port_saved(&next);
    const char *what = ls_lora_port_on_socket(&next) || ls_lora_port_is_v03(&next) ? "V0.3 / V1.0"
                     : ls_lora_port_is_v02(&next) ? "V0.2" : ls_tr("custom (lora port)");
    /* only a header port is running a table; onboard keeps it for later */
    ls_lora_port_t shown = next;
    if (!shown.header) ls_lora_port_board(&shown);
    if (!ls_lora_port_same(&shown, ls_lora_port())) snprintf(b, n, ls_tr("%s, reboot to apply"), what);
    else snprintf(b, n, "%s", what);
}
static void loramod_next(void)
{
    char why[96];
    (void)ls_lora_port_next_module(why, sizeof(why));
}

/* The file hotspot: its name and this board's own password (made once and
   kept; see ls_wifi_ap_pass.h). One tap arms, a second makes a new password -
   a phone already joined has to join again, so it is never one stray tap. */
static bool s_ap_armed;
static void ap_show(char *b, size_t n)
{
    ls_val_t ssid, pass;
    if (!ls_value_read("wifi.ap_pass", &pass, NULL) || pass.kind != LS_VAL_TEXT || !pass.s) {
        snprintf(b, n, "%s", ls_tr("not in this build"));
        return;
    }
    if (s_ap_armed) { snprintf(b, n, "%s", ls_tr("tap again for a new password")); return; }
    const bool named = ls_value_read("wifi.ap_ssid", &ssid, NULL) && ssid.kind == LS_VAL_TEXT && ssid.s;
    snprintf(b, n, "%s  %s", named ? ssid.s : "", pass.s);
}
static void ap_next(void)
{
    if (!s_ap_armed) { s_ap_armed = true; return; }
    s_ap_armed = false;
    ls_val_t out;
    (void)ls_action_call("wifi.ap_renew", NULL, &out, ls_quick_grant_builtin());
}

static void lock_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tr("lock now")); }
static void lock_next(void) { ls_tui_set_locked(true); }

/* Holds the screen the way it is now: the board stops turning it to match
   how it is held. Attaching the keyboard still turns it. */
static void rotlock_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_auto_rotate() ? ls_tr("off") : ls_tr("on")); }
static void rotlock_next(void) { settings_set_auto_rotate(!settings_get_auto_rotate()); }

/* English or Ukrainian. The change is live: the next frame is drawn in it,
   and ls_i18n hands it to the setting (installed at boot). */
static void lang_show(char *b, size_t n) { snprintf(b, n, "%s", ls_i18n_lang_name(ls_i18n_lang())); }
static void lang_next(void) { ls_i18n_set_lang(ls_i18n_next(ls_i18n_lang())); }

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

static void back_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tr("< SETTINGS")); }
static void back_next(void) { page_open(SETP_ROOT); }

static void display_show(char *b, size_t n)
{
    char theme[40];
    theme_show(theme, sizeof(theme));
    if (display_ctl_autodim_enabled())
        snprintf(b, n, ls_tr("%s, dim %d s  >"), theme, display_ctl_autodim_timeout());
    else
        snprintf(b, n, "%s  >", theme);
}
static void display_next(void) { page_open(SETP_DISPLAY); }

static void sound_show(char *b, size_t n)
{
    char boot[16], voice[24];
    boot_show(boot, sizeof(boot));
    if (speech_available()) snprintf(voice, sizeof(voice), "%s", speech_voice_name(speech_voice_get()));
    else                    snprintf(voice, sizeof(voice), "%s", ls_tr("no voice"));
    snprintf(b, n, ls_tr("%s, %s, alerts %s  >"), boot, voice,
             settings_get_alert_ring() ? ls_tr("on") : ls_tr("off"));
}
static void sound_next(void) { page_open(SETP_SOUND); }

static void device_show(char *b, size_t n)
{
    snprintf(b, n, ls_tr("keys %s, USB %s  >"), settings_get_keyboard_light() ? ls_tr("on") : ls_tr("off"),
             app_usb_autoreboot() ? ls_tr("on") : ls_tr("off"));
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
    { "Dim after",      dimt_show,  dimt_next, dimt_step },
};
static const item_t SOUND_ITEMS[] = {
    { "BACK",           back_show,  back_next, NULL },
    { "Boot sound",     boot_show,  boot_next, NULL },
    { "Voice",          voice_show, voice_next, NULL },
    { "Alert sound",    ring_show,  ring_next, NULL },
    { "Notify sound",   nsound_show, nsound_next, NULL },
    { "Vibrate",        vibe_show,  vibe_next, NULL },
};
static const item_t DEVICE_ITEMS[] = {
    { "BACK",           back_show,     back_next, NULL },
    { "Keyboard light", keylight_show, keylight_next, NULL },
    { "Keyboard dim",   keydim_show,   keydim_next, NULL },
    { "LoRa radio",     loraport_show, loraport_next, NULL },
    { "LoRa module",    loramod_show,  loramod_next, NULL },
    { "Language",       lang_show,     lang_next, NULL },
    { "Hotspot",        ap_show,       ap_next, NULL },
    { "USB autoreboot", usb_show,      usb_next, NULL },
    { "Update",         update_show,   update_next, NULL },
    { "Check for updates", updchk_show, updchk_next, NULL },
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const set_page_t SET_PAGES[SETP_COUNT] = {
    [SETP_ROOT]    = { "SETTINGS",         ROOT_ITEMS,    COUNT(ROOT_ITEMS) },
    [SETP_DISPLAY] = { "SETTINGS > DISPLAY", DISPLAY_ITEMS, COUNT(DISPLAY_ITEMS) },
    [SETP_SOUND]   = { "SETTINGS > SOUND", SOUND_ITEMS,   COUNT(SOUND_ITEMS) },
    [SETP_DEVICE]  = { "SETTINGS > DEVICE", DEVICE_ITEMS,  COUNT(DEVICE_ITEMS) },
};
#define MAX_ITEMS 10

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
    const char *label = ls_tr(it->label);
    tui_put_str(sf, a, a.x + 2, a.y, label, name);
    tui_put_char(sf, a, a.x + 2 + (int)strlen(label), a.y, ' ', edge);

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

/* What each row does, for the side of the desk form. */
static const struct { const char *label, *what; } HELP[] = {
    { "Volume", "Speaker and headphone level for every app. Mute keeps the level." },
    { "Brightness", "Panel backlight. Lower saves battery outdoors at night." },
    { "Mute", "Silences the speaker without losing the volume." },
    { "Screen lock", "Locks touch and keys until the unlock gesture." },
    { "Rotate lock", "Holds the screen the way it is when the board turns." },
    { "Display", "Theme, Daylight, font and dimming." },
    { "Sound", "Boot sound, spoken callouts, alert and notice sounds." },
    { "Device", "Keyboard, LoRa radio, language, hotspot and updates." },
    { "Theme", "The colours every app is drawn in. On the desk it brings its window scheme." },
    { "Daylight", "A white theme for sunlight, over the chosen one." },
    { "Font", "Cell font: bigger reads easier, smaller fits more." },
    { "Auto dim", "Dims the panel when nothing has been touched for a while." },
    { "Dim after", "How long before Auto dim turns the panel down." },
    { "Boot sound", "What plays when the board starts." },
    { "Voice", "Spoken callouts from the radio apps." },
    { "Alert sound", "A sound for alerts that need attention." },
    { "Notify sound", "The sound for ordinary notices." },
    { "Vibrate", "Buzz on alerts, on boards with a motor." },
    { "Keyboard light", "Backlight of the keyboard, where there is one." },
    { "Keyboard dim", "Dims the keyboard light with the panel." },
    { "LoRa radio", "Which port the LoRa module answers on." },
    { "LoRa module", "The module type fitted to that port." },
    { "Language", "Words on screen. Numbers and units stay as they are." },
    { "Hotspot", "The board's own Wi-Fi network, for the web pages." },
    { "USB autoreboot", "Restart when a PC flashes the board over USB." },
    { "Update", "Check for and install new firmware over Wi-Fi." },
    { "BACK", "Back to the first page." },
};

/* Words into a box, a line at a time; the lines it used. */
static int wrap(tui_surface *sf, tui_rect box, int x, int y, int w, int rows, const char *t, uint8_t at)
{
    int used = 0;
    while (*t && used < rows) {
        int n = (int)strlen(t), cut = n;
        if (n > w) { cut = w; while (cut > 0 && t[cut] != ' ') cut--; if (!cut) cut = w; }
        char line[96];
        snprintf(line, sizeof(line), "%.*s", cut < 95 ? cut : 95, t);
        tui_put_str(sf, box, x, y + used++, line, at);
        t += cut;
        while (*t == ' ') t++;
    }
    return used;
}

/* The side of the desk form: the selected row explained, the theme's sixteen
   colours, and the board's own figures. */
static void draw_side(tui_surface *sf, tui_rect r, const set_page_t *pg)
{
    const uint8_t frame = TUI_ATTR(TUI_CYAN, TUI_BLACK);
    const uint8_t head = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t text = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const uint8_t val = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    int y = r.y;
    const int ah = r.h >= 22 ? 7 : 6;
    if (s_sel < pg->n && r.h >= ah) {
        const item_t *it = &pg->items[s_sel];
        const tui_rect b = tui_rect_make(r.x, y, r.w, ah);
        tui_box(sf, b, ls_tr("ABOUT"), frame);
        char v[64];
        it->show(v, sizeof(v));
        tui_put_str(sf, b, b.x + 2, b.y + 1, ls_tr(it->label), TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        tui_put_str(sf, b, b.x + 4 + (int)strlen(ls_tr(it->label)), b.y + 1, v, val);
        const char *what = "ENTER or a tap changes it.";
        for (unsigned i = 0; i < sizeof(HELP) / sizeof(HELP[0]); i++)
            if (!strcmp(HELP[i].label, it->label)) { what = HELP[i].what; break; }
        wrap(sf, b, b.x + 2, b.y + 2, b.w - 4, ah - 3, ls_tr(what), text);
        y += ah;
    }
    if (y + 5 <= r.y + r.h) {
        const ls_tui_theme_t *t = ls_tui_get_theme();
        const tui_rect b = tui_rect_make(r.x, y, r.w, 5);
        tui_box(sf, b, ls_tr("COLOURS"), frame);
        tui_put_str(sf, b, b.x + 2, b.y + 1, t && t->name ? t->name : "?", val);
        if (t && t->desc) {
            const int dx = b.x + 4 + (int)strlen(t->name), room = b.x + b.w - 2 - dx;
            if (room > 3) tui_put_str(sf, tui_rect_make(dx, b.y + 1, room, 1), dx, b.y + 1, t->desc, LS_ATTR_DIM);
        }
        /* Two rows of eight swatches: the dim colours over the bright. */
        const int sw = (b.w - 4) / 8 > 4 ? 4 : (b.w - 4) / 8;
        for (int i = 0; i < 16 && sw >= 1; i++)
            for (int k = 0; k < sw; k++)
                tui_put_char(sf, b, b.x + 2 + (i % 8) * sw + k, b.y + 2 + i / 8,
                             k == sw - 1 ? ' ' : LS_TUI_SHADE_FULL, TUI_ATTR(i, TUI_BLACK));
        y += 5;
    }
    if (y + 5 <= r.y + r.h) {
        const tui_rect b = tui_rect_make(r.x, y, r.w, r.y + r.h - y < 6 ? r.y + r.h - y : 6);
        tui_box(sf, b, ls_tr("THIS BOARD"), frame);
        char line[64];
        const unsigned up = (unsigned)(esp_timer_get_time() / 1000000);
        snprintf(line, sizeof(line), "%uh %02um %02us", up / 3600, up / 60 % 60, up % 60);
        tui_put_str(sf, b, b.x + 2, b.y + 1, ls_tr("UP"), head);
        tui_put_str(sf, b, b.x + 12, b.y + 1, line, val);
        snprintf(line, sizeof(line), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        tui_put_str(sf, b, b.x + 2, b.y + 2, ls_tr("RAM FREE"), head);
        tui_put_str(sf, b, b.x + 12, b.y + 2, line, val);
        snprintf(line, sizeof(line), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        if (b.h >= 5) {
            tui_put_str(sf, b, b.x + 2, b.y + 3, "PSRAM FREE", head);
            tui_put_str(sf, b, b.x + 13, b.y + 3, line, val);
        }
    }
}

/* THE DESK FORM: a property list. One row a setting - its name, its value,
   [<] and [>] on a level - with the selection bar in blue. A click on the
   row presses it, as a tap on the box does; a level steps on its keys. */
static void draw_compact(tui_surface *sf, tui_rect body, const set_page_t *pg)
{
    const uint8_t name = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    const uint8_t val  = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
    const uint8_t menu = TUI_ATTR(TUI_CYAN | TUI_BRIGHT, TUI_BLACK);
    const uint8_t bar  = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLUE);
    const uint8_t barv = TUI_ATTR(TUI_YELLOW | TUI_BRIGHT, TUI_BLUE);
    int w = body.w > 76 ? 76 : body.w;
    const int x = body.x + 1;
    tui_put_str(sf, body, x + 1, body.y, ls_tr("SETTING"), TUI_ATTR(TUI_CYAN, TUI_BLACK));
    tui_put_str(sf, body, x + 23, body.y, ls_tr("VALUE"), TUI_ATTR(TUI_CYAN, TUI_BLACK));
    for (int c = 0; c < w - 2; c++)
        ls_box_put(sf, body, x + c, body.y + 1, LS_BOX_H, TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
    for (int i = 0; i < pg->n && body.y + 2 + i < body.y + body.h; i++) {
        const item_t *it = &pg->items[i];
        const bool sel = i == s_sel;
        const tui_rect row = tui_rect_make(x, body.y + 2 + i, w - 2, 1);
        if (sel) tui_fill(sf, row, ' ', bar);
        tui_put_str(sf, row, row.x + 1, row.y, ls_tr(it->label), sel ? bar : name);
        char buf[64];
        it->show(buf, sizeof(buf));
        const bool sub = it->next == back_next || it->next == display_next ||
                         it->next == sound_next || it->next == device_next;
        s_dec[i].w = s_inc[i].w = 0;
        int vx = row.x + 22;
        if (it->step) {
            const uint8_t key = TUI_ATTR(TUI_BLACK, TUI_WHITE);
            s_dec[i] = tui_rect_make(vx, row.y, 3, 1);
            tui_put_str(sf, row, vx, row.y, " < ", key);
            tui_put_str(sf, row, vx + 4, row.y, buf, sel ? barv : val);
            s_inc[i] = tui_rect_make(vx + 5 + 10, row.y, 3, 1);
            tui_put_str(sf, row, s_inc[i].x, row.y, " > ", key);
        } else {
            tui_put_str(sf, row, vx, row.y, buf, sel ? barv : sub ? menu : val);
        }
        if (s_hit_n < MAX_ITEMS) s_hit[s_hit_n++] = row;
    }
    /* The app's own art under the list, and the side panels beside it. */
    const int below = body.y + 2 + pg->n + 1;
    if (body.y + body.h - below >= 8)
        ls_icon_draw(sf, body, x + 2, below, LS_ICON_GEAR, TUI_ATTR(TUI_BLUE | TUI_BRIGHT, TUI_BLACK));
    if (body.w >= w + 28 && body.h >= 9)
        draw_side(sf, tui_rect_make(body.x + w + 1, body.y, body.w - w - 1, body.h), pg);
    else if (body.w >= w + 16 && body.h >= 9)
        ls_icon_draw(sf, body, body.x + w + 4, body.y + 2, LS_ICON_GEAR, TUI_ATTR(TUI_BLUE | TUI_BRIGHT, TUI_BLACK));
}

static void draw(tui_surface *sf, tui_rect area)
{
    const set_page_t *pg = page_get();

    s_hit_n = 0;
    ls_panel_box(sf, area, pg->title, TUI_CYAN);
    if (area.h < 6 || area.w < 18) return;
    if (ls_tui_compact()) {
        draw_compact(sf, tui_rect_make(area.x + 1, area.y + 1, area.w - 2, area.h - 2), pg);
        return;
    }

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
    const set_page_t *pg = page_get();
    if (s_sel >= pg->n) s_sel = 0;
    switch (k) {
    case LS_TK_UP:    s_sel = (s_sel + pg->n - 1) % pg->n; return true;
    case LS_TK_DOWN:  s_sel = (s_sel + 1) % pg->n;         return true;
    case LS_TK_ENTER:
        if (pg->items[s_sel].next != ap_next) s_ap_armed = false;
        pg->items[s_sel].next();
        return true;
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
            if (pg->items[i].next != ap_next) s_ap_armed = false;
            if (pg->items[i].step && ls_tui_compact()) {
                /* The desk row: its keys step it, the rest selects it. */
                if (tui_rect_contains(s_dec[i], col, row)) pg->items[i].step(-1);
                else if (tui_rect_contains(s_inc[i], col, row)) pg->items[i].step(1);
                return true;
            }
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
    s_ap_armed = false;
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
