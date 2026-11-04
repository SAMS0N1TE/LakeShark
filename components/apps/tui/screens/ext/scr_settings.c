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
#include "../../ls_lap_service.h"
#include "../../ls_power.h"
#include "ls_gauge.h"
#include "../../ls_braille.h"
#include "../../ls_tui_touch.h"
#include "../../ls_radio_panel.h"

static int s_sel;

typedef struct {
    const char *label;
    void (*show)(char *buf, size_t len);
    void (*next)(void);
    /* A level: drawn with < and >, which a tap or LEFT/RIGHT step through
       without wrapping. NULL for a setting that cycles. */
    void (*step)(int dir);
    /* How the value is drawn. A switch says whether it is on (a green pill,
       or a red one when `warn`: on is the state to notice); a level says
       where it sits, 0..100, for its meter. */
    int  (*on)(void);
    int  (*pct)(void);
    /* A level set outright, 0..100: what a finger on its slider does. */
    void (*set)(int pct);
    /* A row with nothing to show right now (a service slot not in use). */
    bool (*hidden)(void);
    bool warn;
    /* The row opens somewhere else: drawn with an arrow. */
    bool go;
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

/* ---- how each value reads ---------------------------------------------- */

static int vol_pct(void)      { return audio_volume_get(); }
static int bri_pct(void)      { return display_ctl_get_user(); }
/* Only on a change: both save, and a held finger asks every frame. */
static void vol_set(int v)
{
    v = (v + 2) / 5 * 5;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v != audio_volume_get()) audio_volume_set(v);
}
static void bri_set(int v)
{
    v = (v + 2) / 5 * 5;
    v = v < 5 ? 5 : v > 100 ? 100 : v;
    if (v != display_ctl_get_user()) display_ctl_set_user(v);
}
static int mute_on(void)      { return audio_is_muted(); }
static int rotlock_on(void)   { return !settings_get_auto_rotate(); }
static int day_on(void)       { return ls_tui_daylight(); }
static int dim_on(void)       { return display_ctl_autodim_enabled(); }
static int dimt_pct(void)     { return display_ctl_autodim_timeout() * 100 / 240; }
static int ring_on(void)      { return settings_get_alert_ring(); }
static int vibe_on(void)      { return settings_get_alert_vibe(); }
static int keylight_on(void)  { return settings_get_keyboard_light(); }
static int keydim_on(void)    { return display_ctl_keyboard_dim(); }
static int usb_on(void)       { return app_usb_autoreboot(); }
static int updchk_on(void)    { return settings_get_update_check(); }

/* ---- power ---------------------------------------------------------------
   The mode and what WATCH keeps: choices stepped with < and > as well as a
   tap, so the one wanted is reached without going round. */
static void pmode_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tr(ls_power_mode_name(ls_power_mode()))); }
static void pmode_step(int dir)
{
    const int m = ((int)ls_power_mode() + (dir < 0 ? LS_PWR_MODES - 1 : 1)) % LS_PWR_MODES;
    ls_power_set_mode((ls_pwr_mode_t)m);
}
static void pmode_next(void) { pmode_step(+1); }
static void pkeep_show(char *b, size_t n) { snprintf(b, n, "%s", ls_tr(ls_power_keep_name(ls_power_keep()))); }
static void pkeep_step(int dir)
{
    const int k = ((int)ls_power_keep() + (dir < 0 ? LS_KEEPS - 1 : 1)) % LS_KEEPS;
    ls_power_set_keep((ls_pwr_keep_t)k);
}
static void pkeep_next(void) { pkeep_step(+1); }

/* Native apps' background services: one row each, there only while one runs. */
static ls_lap_service_info_t service_rows[4];
static int s_service_n;
static int services_refresh(void)
{
    const int n = ls_lap_services(service_rows, 4);
    for (int i = n; i < 4; i++) memset(&service_rows[i], 0, sizeof(service_rows[i]));
    s_service_n = n;
    return n;
}
static void service_show(unsigned i, char *b, size_t n)
{
    if (i >= (unsigned)s_service_n) snprintf(b, n, "%s", ls_tr("none"));
    else snprintf(b, n, "STOP %.15s (%lu us)", service_rows[i].name, (unsigned long)service_rows[i].stats.last_us);
}
static void service_stop(unsigned i)
{
    /* Stop exactly the row last presented, even if another resident ends. */
    if (i < 4 && service_rows[i].id[0]) ls_lap_service_request_stop(service_rows[i].id);
}
#define SERVICE_ROW(i) static void svc_show_##i(char *b, size_t n) { service_show(i, b, n); } \
    static void svc_stop_##i(void) { service_stop(i); } \
    static bool svc_hide_##i(void) { return i >= s_service_n; }
SERVICE_ROW(0) SERVICE_ROW(1) SERVICE_ROW(2) SERVICE_ROW(3)
#undef SERVICE_ROW

/* ---- pages ---------------------------------------------------------------
   Two levels and no more: the first page has the controls reached for most
   (volume, brightness, mute, lock, rotate) over six cards, each in its own
   colour, and a card opens its page. A page lists its settings under a live
   panel of what they act on; BACK, ESC or BACKSPACE return. */

enum { SETP_ROOT, SETP_DISPLAY, SETP_SOUND, SETP_POWER, SETP_RADIO, SETP_NETWORK, SETP_SYSTEM, SETP_COUNT };

typedef struct {
    const char   *title;
    const char   *what;      /* the card's line under the title */
    uint8_t       hue;
    int           icon;
    const item_t *items;
    int           n;
    void (*panel)(tui_surface *sf, tui_rect r, uint8_t hue);
    int           panel_rows; /* the panel's height in portrait, borders in */
} set_page_t;

static int s_page = SETP_ROOT;

static const item_t QUICK_ITEMS[] = {
    { .label = "Volume",      .show = vol_show,     .next = vol_next,     .step = vol_step, .pct = vol_pct, .set = vol_set },
    { .label = "Brightness",  .show = bri_show,     .next = bri_next,     .step = bri_step, .pct = bri_pct, .set = bri_set },
    { .label = "Mute",        .show = mute_show,    .next = mute_next,    .on = mute_on, .warn = true },
    { .label = "Screen lock", .show = lock_show,    .next = lock_next },
    { .label = "Rotate lock", .show = rotlock_show, .next = rotlock_next, .on = rotlock_on },
};
static const item_t DISPLAY_ITEMS[] = {
    { .label = "Theme",     .show = theme_show, .next = theme_next },
    { .label = "Daylight",  .show = day_show,   .next = day_next, .on = day_on },
    { .label = "Font",      .show = font_show,  .next = font_next },
    { .label = "Auto dim",  .show = dim_show,   .next = dim_next, .on = dim_on },
    { .label = "Dim after", .show = dimt_show,  .next = dimt_next, .step = dimt_step, .pct = dimt_pct },
};
static const item_t SOUND_ITEMS[] = {
    { .label = "Boot sound",   .show = boot_show,   .next = boot_next },
    { .label = "Voice",        .show = voice_show,  .next = voice_next, .go = true },
    { .label = "Alert sound",  .show = ring_show,   .next = ring_next, .on = ring_on },
    { .label = "Notify sound", .show = nsound_show, .next = nsound_next },
    { .label = "Vibrate",      .show = vibe_show,   .next = vibe_next, .on = vibe_on },
};
static const item_t POWER_ITEMS[] = {
    { .label = "Power mode",     .show = pmode_show,    .next = pmode_next, .step = pmode_step },
    { .label = "Watch keeps",    .show = pkeep_show,    .next = pkeep_next, .step = pkeep_step },
    { .label = "Keyboard light", .show = keylight_show, .next = keylight_next, .on = keylight_on },
    { .label = "Keyboard dim",   .show = keydim_show,   .next = keydim_next, .on = keydim_on },
    { .label = "Service 1", .show = svc_show_0, .next = svc_stop_0, .hidden = svc_hide_0, .warn = true },
    { .label = "Service 2", .show = svc_show_1, .next = svc_stop_1, .hidden = svc_hide_1, .warn = true },
    { .label = "Service 3", .show = svc_show_2, .next = svc_stop_2, .hidden = svc_hide_2, .warn = true },
    { .label = "Service 4", .show = svc_show_3, .next = svc_stop_3, .hidden = svc_hide_3, .warn = true },
};
static const item_t RADIO_ITEMS[] = {
    { .label = "LoRa radio",  .show = loraport_show, .next = loraport_next },
    { .label = "LoRa module", .show = loramod_show,  .next = loramod_next },
};
static const item_t NETWORK_ITEMS[] = {
    { .label = "Hotspot",           .show = ap_show,     .next = ap_next },
    { .label = "Update",            .show = update_show, .next = update_next, .go = true },
    { .label = "Check for updates", .show = updchk_show, .next = updchk_next, .on = updchk_on },
};
static const item_t SYSTEM_ITEMS[] = {
    { .label = "Language",       .show = lang_show, .next = lang_next },
    { .label = "USB autoreboot", .show = usb_show,  .next = usb_next, .on = usb_on },
};
#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void panel_display(tui_surface *sf, tui_rect r, uint8_t hue);
static void panel_sound(tui_surface *sf, tui_rect r, uint8_t hue);
static void panel_power(tui_surface *sf, tui_rect r, uint8_t hue);
static void panel_radio(tui_surface *sf, tui_rect r, uint8_t hue);
static void panel_network(tui_surface *sf, tui_rect r, uint8_t hue);
static void panel_system(tui_surface *sf, tui_rect r, uint8_t hue);

static const set_page_t SET_PAGES[SETP_COUNT] = {
    [SETP_ROOT]    = { "SETTINGS", NULL, TUI_CYAN, LS_ICON_GEAR, QUICK_ITEMS, COUNT(QUICK_ITEMS), NULL },
    [SETP_DISPLAY] = { "DISPLAY", "theme, font, dimming", TUI_CYAN, LS_ICON_TERMINAL,
                       DISPLAY_ITEMS, COUNT(DISPLAY_ITEMS), panel_display, 8 },
    [SETP_SOUND]   = { "SOUND", "boot, voice, alerts", TUI_MAGENTA, LS_ICON_MUSIC,
                       SOUND_ITEMS, COUNT(SOUND_ITEMS), panel_sound, 8 },
    [SETP_POWER]   = { "POWER", "battery, power mode", TUI_GREEN, LS_ICON_POWER,
                       POWER_ITEMS, COUNT(POWER_ITEMS), panel_power, 18 },
    [SETP_RADIO]   = { "RADIO", "LoRa port and module", TUI_YELLOW, LS_ICON_RADIO,
                       RADIO_ITEMS, COUNT(RADIO_ITEMS), panel_radio, 8 },
    [SETP_NETWORK] = { "NETWORK", "hotspot, updates", TUI_BLUE, LS_ICON_WIRELESS,
                       NETWORK_ITEMS, COUNT(NETWORK_ITEMS), panel_network, 8 },
    [SETP_SYSTEM]  = { "SYSTEM", "language, USB, board", TUI_WHITE, LS_ICON_CHIP,
                       SYSTEM_ITEMS, COUNT(SYSTEM_ITEMS), panel_system, 8 },
};
#define N_CARDS (SETP_COUNT - 1)
#define MAX_ITEMS 16

static const set_page_t *page_get(void) { return &SET_PAGES[s_page]; }

/* What the selection walks: on the first page the quick controls, then the
   cards; on a page its rows that are showing. */
static int s_vis[MAX_ITEMS], s_vis_n;
static void visible_refresh(void)
{
    const set_page_t *pg = page_get();
    s_vis_n = 0;
    /* the service rows come from the last snapshot the draw took, so a key
       acts on the row that was on the glass, not on one shifted since */
    for (int i = 0; i < pg->n && s_vis_n < MAX_ITEMS; i++)
        if (!pg->items[i].hidden || !pg->items[i].hidden()) s_vis[s_vis_n++] = i;
}
static int entries(void) { visible_refresh(); return s_page == SETP_ROOT ? s_vis_n + N_CARDS : s_vis_n; }

static void page_open(int p)
{
    const int from = s_page;
    s_page = p;
    s_sel = 0;
    visible_refresh();
    /* Back on the first page with the card that was open selected. */
    if (p == SETP_ROOT && from != SETP_ROOT) s_sel = COUNT(QUICK_ITEMS) + from - 1;
}

/* ---- drawing --------------------------------------------------------------- */

#define HUE_HI(h) ((uint8_t)((h) | TUI_BRIGHT))

typedef struct { tui_rect r; int item; int page; } hit_t;
static hit_t    s_hit[MAX_ITEMS + N_CARDS + 2];
static int      s_hit_n;
static tui_rect s_back;
static tui_rect s_dec[MAX_ITEMS], s_inc[MAX_ITEMS];

static void hit_add(tui_rect r, int item, int page)
{
    if (s_hit_n < (int)(sizeof(s_hit) / sizeof(s_hit[0]))) s_hit[s_hit_n++] = (hit_t){ r, item, page };
}

static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s = (char)(*s + 32); }

/* One row's value, in `w` cells from `x` on row `y`: a pill for a switch, a
   meter between < and > for a level, a choice between < and >, or the words.
   Fills the step keys' rects (full `h` tall from `ky`) for the touch. */
static void value_draw(tui_surface *sf, tui_rect clip, int x, int y, int w, int ky, int kh,
                       const item_t *it, int i, uint8_t hue, bool sel)
{
    char buf[64];
    it->show(buf, sizeof(buf));
    const uint8_t words = TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
    s_dec[i].w = s_inc[i].w = 0;
    if (w < 6) return;
    if (it->step && w >= 16) {
        const uint8_t key = TUI_ATTR(TUI_BLACK, sel ? HUE_HI(hue) : hue);
        const int kw = w >= 30 ? 5 : 3;
        s_dec[i] = tui_rect_make(x, ky, kw, kh);
        s_inc[i] = tui_rect_make(x + w - kw, ky, kw, kh);
        tui_fill(sf, s_dec[i], ' ', key);
        tui_fill(sf, s_inc[i], ' ', key);
        tui_put_char(sf, clip, x + kw / 2, y, '<', key);
        tui_put_char(sf, clip, x + w - kw + kw / 2, y, '>', key);
        const int ix = x + kw + 1, iw = w - 2 * kw - 2;
        if (it->pct) {
            /* the meter, then the figure */
            const int tw = (int)strlen(buf);
            const int mw = iw - tw - 1;
            if (mw >= 6) ls_meter_ascii(sf, clip, ix, y, mw, it->pct() / 100.0f, HUE_HI(hue));
            tui_put_str(sf, clip, ix + (mw >= 6 ? mw + 1 : 0), y, buf, words);
        } else {
            const int tw = (int)strlen(buf);
            tui_put_str(sf, clip, ix + (iw > tw ? (iw - tw) / 2 : 0), y, buf, TUI_ATTR(HUE_HI(hue), TUI_BLACK));
        }
        return;
    }
    if (it->on) {
        const bool on = it->on() != 0;
        lower(buf);
        /* on is green; for a warning switch (mute) on is red and off is
           the good state, green */
        const uint8_t pill = on ? TUI_ATTR(TUI_BLACK, it->warn ? (TUI_RED | TUI_BRIGHT) : (TUI_GREEN | TUI_BRIGHT))
                           : it->warn ? TUI_ATTR(TUI_BLACK, TUI_GREEN)
                                      : TUI_ATTR(TUI_WHITE, TUI_BLACK | TUI_BRIGHT);
        char p[40];
        snprintf(p, sizeof(p), "  %s  ", buf);
        const int pw = (int)strlen(p) < w ? (int)strlen(p) : w;
        const int px = x + w - pw;
        tui_fill(sf, tui_rect_make(px, y, pw, 1), ' ', pill);
        tui_put_str(sf, tui_rect_make(px, y, pw, 1), px, y, p, pill);
        return;
    }
    const int tw = (int)strlen(buf) + (it->go ? 2 : 0);
    const int tx = x + (w > tw ? w - tw : 0);
    tui_put_str(sf, tui_rect_make(x, y, w, 1), tx, y, buf, it->warn ? TUI_ATTR(TUI_RED | TUI_BRIGHT, TUI_BLACK) : words);
    if (it->go) tui_put_str(sf, clip, x + w - 1, y, ">", TUI_ATTR(HUE_HI(hue), TUI_BLACK));
}

/* A setting as a box to press: the label on its top rule, the value inside. */
static void row_draw(tui_surface *sf, tui_rect a, const item_t *it, int i, uint8_t hue, bool sel)
{
    ls_panel_box(sf, a, NULL, sel ? HUE_HI(hue) : hue);
    const char *label = ls_tr(it->label);
    const uint8_t name = TUI_ATTR(sel ? TUI_BLACK : (TUI_WHITE | TUI_BRIGHT), sel ? HUE_HI(hue) : TUI_BLACK);
    char lb[40];
    snprintf(lb, sizeof(lb), " %s ", label);
    tui_put_str(sf, a, a.x + 1, a.y, lb, name);
    const tui_rect f = tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
    if (f.h < 1) return;
    if (sel) ls_fill_dither(sf, f, LS_DITHER_LIGHT, hue);
    value_draw(sf, a, f.x + 1, f.y + (f.h - 1) / 2, f.w - 2, f.y, f.h, it, i, hue, sel);
}

/* ---- the level sliders ------------------------------------------------------
   Volume and brightness drawn as the radio screens draw volume and gain: a
   card of upright bars, the bar pressed and everything left of it lit, and a
   - and + key at the right. A finger held on the bars follows the drag. */
static ls_radio_card_t s_lvc[2];
static tui_rect        s_lv[2];
static int             s_lvdrag = -1;

static void lv_draw(tui_surface *sf, tui_rect a, const item_t *it, int i, uint8_t hue)
{
    char v[12];
    snprintf(v, sizeof(v), "%d%%", it->pct());
    ls_radio_card_t *c = &s_lvc[i];
    memset(c, 0, sizeof(*c));
    c->label = it->label;
    c->value = v;
    c->frac = it->pct() / 100.0f;
    c->hue = hue;
    c->live = true;
    c->settable = true;
    c->thumb_keys = true;
    ls_radio_vfo_card_draw(sf, a, c);
    s_lv[i] = a;
}

static int lv_level(int i, int col)
{
    return (int)(ls_radio_vfo_card_bar_frac(s_lvc[i].bar, col) * 100.0f + 0.5f);
}

/* A finger that went down on a slider's bars sets it from the column it is
   on now, each frame, until it lifts. */
static void lv_follow(const item_t *items)
{
    int sc, sr, c, r;
    if (!ls_tui_touch_held(&sc, &sr, &c, &r)) { s_lvdrag = -1; return; }
    if (s_lvdrag < 0)
        for (int i = 0; i < 2; i++)
            if (s_lv[i].w && s_lvc[i].bar.w && tui_rect_contains(s_lvc[i].bar, sc, sr)) s_lvdrag = i;
    if (s_lvdrag < 0) return;
    s_sel = s_lvdrag;
    items[s_lvdrag].set(lv_level(s_lvdrag, c));
}

/* A tap on a slider card: - and + step, the bars set. True when it was one. */
static bool lv_touch(int i, const item_t *it, int col, int row)
{
    if (!s_lv[i].w || !tui_rect_contains(s_lv[i], col, row)) return false;
    const ls_radio_card_t *c = &s_lvc[i];
    switch (ls_radio_vfo_card_hit(c->down, c->up, c->value_rect, c->bar, col, row)) {
    case LS_RADIO_PART_DOWN: it->step(-1); break;
    case LS_RADIO_PART_UP:   it->step(1);  break;
    case LS_RADIO_PART_BAR:  it->set(lv_level(i, col)); break;
    default: break;
    }
    return true;
}

/* A setting as one line, label left and value right, for the short boxes. */
static void line_draw(tui_surface *sf, tui_rect a, const item_t *it, int i, uint8_t hue, bool sel)
{
    if (sel) tui_fill(sf, a, ' ', TUI_ATTR(TUI_WHITE, TUI_BLUE));
    tui_put_str(sf, a, a.x + 1, a.y, ls_tr(it->label),
                TUI_ATTR(TUI_WHITE | TUI_BRIGHT, sel ? TUI_BLUE : TUI_BLACK));
    const int lw = 19;
    value_draw(sf, a, a.x + lw, a.y, a.w - lw - 1, a.y, a.h, it, i, hue, sel);
}

/* ---- the live panels: what the settings on the page act on, read now --- */

static void kv(tui_surface *sf, tui_rect r, int x, int y, const char *k, const char *v, uint8_t hue)
{
    tui_put_str(sf, r, x, y, k, TUI_ATTR(hue, TUI_BLACK));
    tui_put_str(sf, r, x + 11, y, v, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
}

static void panel_display(tui_surface *sf, tui_rect r, uint8_t hue)
{
    const ls_tui_theme_t *t = ls_tui_get_theme();
    tui_put_str(sf, r, r.x, r.y, t && t->name ? t->name : "?", TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    if (t && t->desc && r.h >= 5) tui_put_str(sf, r, r.x, r.y + 1, t->desc, LS_ATTR_DIM);
    /* The theme's sixteen colours: the dim ones over the bright. */
    const int sw = r.w / 8 > 4 ? 4 : r.w / 8;
    const int sy = r.y + (r.h >= 5 ? 2 : 1);
    for (int i = 0; i < 16 && sw >= 1; i++)
        for (int k = 0; k < sw; k++)
            tui_put_char(sf, r, r.x + (i % 8) * sw + k, sy + i / 8,
                         k == sw - 1 ? ' ' : LS_TUI_SHADE_FULL, TUI_ATTR(i, TUI_BLACK));
    if (sy + 2 < r.y + r.h) {
        char v[16];
        snprintf(v, sizeof(v), "%d %%", display_ctl_get_user());
        tui_put_str(sf, r, r.x, sy + 2, ls_tr("PANEL"), TUI_ATTR(hue, TUI_BLACK));
        ls_meter_ascii(sf, r, r.x + 6, sy + 2, r.w - 13 > 6 ? r.w - 13 : 6, display_ctl_get_user() / 100.0f, HUE_HI(hue));
        tui_put_str(sf, r, r.x + r.w - 6, sy + 2, v, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    }
}

static void panel_sound(tui_surface *sf, tui_rect r, uint8_t hue)
{
    char v[32];
    const bool muted = audio_is_muted();
    snprintf(v, sizeof(v), "%d %%", audio_volume_get());
    tui_put_str(sf, r, r.x, r.y, ls_tr("VOLUME"), TUI_ATTR(hue, TUI_BLACK));
    ls_meter_ascii(sf, r, r.x + 7, r.y, r.w - 14 > 6 ? r.w - 14 : 6, audio_volume_get() / 100.0f,
                   muted ? (TUI_BLACK | TUI_BRIGHT) : HUE_HI(hue));
    tui_put_str(sf, r, r.x + r.w - 6, r.y, v, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
    if (r.h >= 2)
        tui_put_str(sf, r, r.x, r.y + 1, ls_tr(muted ? "MUTED" : "sound on"),
                    TUI_ATTR(muted ? (TUI_RED | TUI_BRIGHT) : (TUI_GREEN | TUI_BRIGHT), TUI_BLACK));
    if (r.h >= 3) { voice_show(v, sizeof(v)); kv(sf, r, r.x, r.y + 2, ls_tr("VOICE"), v, hue); }
    if (r.h >= 4) { boot_show(v, sizeof(v)); kv(sf, r, r.x, r.y + 3, ls_tr("BOOT"), v, hue); }
}

/* The pack drawn as a cell: filled to the charge, green to red. */
static void battery_art(tui_surface *sf, tui_rect r, int x, int y, int w, int pct, bool charging)
{
    if (w < 6) return;
    const uint8_t fill = pct > 50 ? (TUI_GREEN | TUI_BRIGHT) : pct > 20 ? (TUI_YELLOW | TUI_BRIGHT) : (TUI_RED | TUI_BRIGHT);
    const uint8_t edge = TUI_ATTR(TUI_WHITE, TUI_BLACK);
    const int in = w - 3, lit = (pct * in + 50) / 100;
    ls_box_put(sf, r, x, y, LS_BOX_DR, edge);
    ls_box_put(sf, r, x, y + 2, LS_BOX_UR, edge);
    for (int c = 1; c <= in; c++) { ls_box_put(sf, r, x + c, y, LS_BOX_H, edge); ls_box_put(sf, r, x + c, y + 2, LS_BOX_H, edge); }
    ls_box_put(sf, r, x + in + 1, y, LS_BOX_DL, edge);
    ls_box_put(sf, r, x + in + 1, y + 2, LS_BOX_UL, edge);
    ls_box_put(sf, r, x, y + 1, LS_BOX_V, edge);
    ls_box_put(sf, r, x + in + 1, y + 1, LS_BOX_V, edge);
    tui_put_char(sf, r, x + in + 2, y + 1, LS_TUI_SHADE_FULL, edge);
    for (int c = 0; c < in; c++)
        tui_put_char(sf, r, x + 1 + c, y + 1, c < lit ? LS_TUI_SHADE_FULL : LS_TUI_SHADE_25,
                     TUI_ATTR(c < lit ? fill : LS_FAINT_FG, TUI_BLACK));
    if (charging) tui_put_char(sf, r, x + 1 + in / 2, y + 1, '+', TUI_ATTR(TUI_BLACK, fill));
}

static void panel_power(tui_surface *sf, tui_rect r, uint8_t hue)
{
    ls_gauge_t g;
    char v[48];
    const bool have = ls_gauge_get(&g) && g.present;
    if (have && r.h >= 3) {
        const int bw = r.w / 2 > 18 ? 18 : r.w / 2;
        battery_art(sf, r, r.x, r.y, bw, g.percent, g.charging);
        snprintf(v, sizeof(v), "%u%%", (unsigned)g.percent);
        tui_put_str(sf, r, r.x + bw + 2, r.y + 1, v, TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK));
        const int ma = g.milliamps;
        if (ma < -5 && g.remaining_mah)
            snprintf(v, sizeof(v), "%d mA  ~%.1f h", ma, (double)g.remaining_mah / (double)(-ma));
        else
            snprintf(v, sizeof(v), "%+d mA %s", ma, ls_tr(g.charging ? "charging" : ""));
        tui_put_str(sf, r, r.x + bw + 7, r.y + 1, v, TUI_ATTR(ma < 0 ? HUE_HI(hue) : (TUI_CYAN | TUI_BRIGHT), TUI_BLACK));
    } else {
        tui_put_str(sf, r, r.x, r.y, ls_tr("no battery gauge on this board"), LS_ATTR_DIM);
    }
    const int y = r.y + (have ? 3 : 1);
    if (y < r.y + r.h) {
        snprintf(v, sizeof(v), "%s", ls_tr(ls_power_mode_name(ls_power_mode())));
        tui_put_str(sf, r, r.x, y, ls_tr("MODE"), TUI_ATTR(hue, TUI_BLACK));
        tui_put_str(sf, r, r.x + 5, y, v, TUI_ATTR(TUI_BLACK, HUE_HI(hue)));
        tui_put_str(sf, r, r.x + 6 + (int)strlen(v), y, ls_tr(ls_power_mode_what(ls_power_mode())), LS_ATTR_DIM);
    }
    if (y + 1 < r.y + r.h && ls_power_mode() == LS_PWR_WATCH) {
        snprintf(v, sizeof(v), ls_tr("dark keeps %s"), ls_tr(ls_power_keep_name(ls_power_keep())));
        tui_put_str(sf, r, r.x + 5, y + 1, v, TUI_ATTR(TUI_WHITE, TUI_BLACK));
    }
    /* The last hour of current draw, a braille column a minute pair:
       what a mode change actually bought. */
    const int gy = y + 3, gh = r.y + r.h - gy - 1;
    if (gh < 2 || r.w < 20) return;
    short h[LS_POWER_HISTORY];
    const int n = ls_power_history(h, LS_POWER_HISTORY);
    if (!n) {
        tui_put_str(sf, r, r.x, gy, ls_tr("CURRENT DRAW  one sample a minute, from the gauge"), LS_ATTR_DIM);
        return;
    }
    int peak = 1;
    for (int i = 0; i < n; i++) { const int a = h[i] < 0 ? -h[i] : h[i]; if (a > peak) peak = a; }
    const int scale = (peak + 99) / 100 * 100;
    snprintf(v, sizeof(v), ls_tr("CURRENT  %d min  top %d mA"), n, scale);
    tui_put_str(sf, r, r.x, gy, v, TUI_ATTR(hue, TUI_BLACK));
    const int cols = r.w < n / 2 + 1 ? r.w : (n + 1) / 2;
    const int first = n - cols * 2 < 0 ? 0 : n - cols * 2;
    const int levels = gh * 4;
    for (int c = 0; c < cols; c++) {
        int lv[2];
        for (int half = 0; half < 2; half++) {
            const int i = first + c * 2 + half;
            const int a = i < n ? (h[i] < 0 ? -h[i] : h[i]) : 0;
            lv[half] = i < n ? (a * levels + scale / 2) / scale : 0;
        }
        for (int row = 0; row < gh; row++) {
            const int base = row * 4;
            uint8_t bits = 0;
            for (int half = 0; half < 2; half++) {
                int k = lv[half] - base;
                if (k > 4) k = 4;
                if (k > 0) bits |= ls_braille_column((unsigned)k, half);
            }
            if (bits) ls_braille_put(sf, r, r.x + c, gy + gh - row, (int16_t)(0x2800 | bits),
                                     TUI_ATTR(row >= gh * 3 / 4 ? (TUI_YELLOW | TUI_BRIGHT) : HUE_HI(hue), TUI_BLACK));
        }
    }
}

static void panel_radio(tui_surface *sf, tui_rect r, uint8_t hue)
{
    char v[64];
    loraport_show(v, sizeof(v));
    kv(sf, r, r.x, r.y, ls_tr("PORT"), v, hue);
    if (r.h >= 2) { loramod_show(v, sizeof(v)); kv(sf, r, r.x, r.y + 1, ls_tr("MODULE"), v, hue); }
    if (r.h >= 4)
        tui_put_str(sf, r, r.x, r.y + 3, ls_tr("Front ends and the radios: RADIOS > OPTIONS"), LS_ATTR_DIM);
}

static void panel_network(tui_surface *sf, tui_rect r, uint8_t hue)
{
    char v[64];
    ap_show(v, sizeof(v));
    kv(sf, r, r.x, r.y, ls_tr("HOTSPOT"), v, hue);
    if (r.h >= 2) { update_show(v, sizeof(v)); kv(sf, r, r.x, r.y + 1, ls_tr("FIRMWARE"), v, hue); }
    if (r.h >= 3) { updchk_show(v, sizeof(v)); kv(sf, r, r.x, r.y + 2, ls_tr("CHECKS"), v, hue); }
}

static void panel_system(tui_surface *sf, tui_rect r, uint8_t hue)
{
    char v[48];
    const unsigned up = (unsigned)(esp_timer_get_time() / 1000000);
    snprintf(v, sizeof(v), "%uh %02um %02us", up / 3600, up / 60 % 60, up % 60);
    kv(sf, r, r.x, r.y, ls_tr("UP"), v, hue);
    const unsigned ram = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    snprintf(v, sizeof(v), "%u KB", ram);
    if (r.h >= 2) {
        kv(sf, r, r.x, r.y + 1, ls_tr("RAM FREE"), v, hue);
        /* internal RAM is the scarce one: amber under 24 KB, red under 12 */
        if (ram < 24) tui_put_str(sf, r, r.x + 11, r.y + 1, v, TUI_ATTR(ram < 12 ? (TUI_RED | TUI_BRIGHT) : (TUI_YELLOW | TUI_BRIGHT), TUI_BLACK));
    }
    snprintf(v, sizeof(v), "%u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    if (r.h >= 3) kv(sf, r, r.x, r.y + 2, ls_tr("PSRAM FREE"), v, hue);
    if (r.h >= 4) { lang_show(v, sizeof(v)); kv(sf, r, r.x, r.y + 3, ls_tr("LANGUAGE"), v, hue); }
}

/* ---- the first page -------------------------------------------------------- */

/* The board's own state, on one line over everything. */
static void status_line(tui_surface *sf, tui_rect r, int y)
{
    ls_gauge_t g;
    char v[64];
    int x = r.x + 1;
    if (ls_gauge_get(&g) && g.present) {
        const uint8_t c = g.percent > 50 ? (TUI_GREEN | TUI_BRIGHT) : g.percent > 20 ? (TUI_YELLOW | TUI_BRIGHT) : (TUI_RED | TUI_BRIGHT);
        snprintf(v, sizeof(v), "%s %u%%", ls_tr(g.charging ? "CHARGING" : "BATTERY"), (unsigned)g.percent);
        tui_put_str(sf, r, x, y, v, TUI_ATTR(c, TUI_BLACK));
        x += (int)strlen(v) + 2;
        snprintf(v, sizeof(v), "%+d mA", g.milliamps);
        tui_put_str(sf, r, x, y, v, LS_ATTR_DIM);
        x += (int)strlen(v) + 2;
    }
    snprintf(v, sizeof(v), "%s %s", ls_tr("POWER"), ls_tr(ls_power_mode_name(ls_power_mode())));
    tui_put_str(sf, r, x, y, v, TUI_ATTR(ls_power_mode() == LS_PWR_FULL ? TUI_WHITE : (TUI_GREEN | TUI_BRIGHT), TUI_BLACK));
}

/* A card: its colour, its art and the first of its settings as they stand. */
static void card_draw(tui_surface *sf, tui_rect a, int p, bool sel)
{
    const set_page_t *pg = &SET_PAGES[p];
    const uint8_t hue = pg->hue;
    ls_panel_box(sf, a, NULL, sel ? HUE_HI(hue) : hue);
    char t[24];
    snprintf(t, sizeof(t), " %s ", ls_tr(pg->title));
    tui_put_str(sf, a, a.x + 1, a.y, t, TUI_ATTR(TUI_BLACK, HUE_HI(hue)));
    tui_put_char(sf, a, a.x + a.w - 2, a.y + a.h / 2, '>', TUI_ATTR(HUE_HI(hue), TUI_BLACK));
    const tui_rect in = tui_rect_make(a.x + 1, a.y + 1, a.w - 3, a.h - 2);
    if (sel) ls_fill_dither(sf, in, LS_DITHER_LIGHT, hue);
    int tx = in.x + 1, ty = in.y, tw = in.w - 1, th = in.h;
    if (in.h >= LS_ICON_ROWS && in.w >= LS_ICON_COLS + 20) {
        ls_icon_draw(sf, in, in.x + 1, in.y + (in.h - LS_ICON_ROWS) / 2, pg->icon, TUI_ATTR(HUE_HI(hue), TUI_BLACK));
        tx = in.x + LS_ICON_COLS + 3; tw = in.x + in.w - tx;
    } else if (in.h >= LS_ICON_ROWS + 3 && in.w >= LS_ICON_COLS + 2) {
        ls_icon_draw(sf, in, in.x + (in.w - LS_ICON_COLS) / 2, in.y, pg->icon, TUI_ATTR(HUE_HI(hue), TUI_BLACK));
        ty = in.y + LS_ICON_ROWS; th = in.h - LS_ICON_ROWS;
    }
    /* The page's first settings, a line each, as many as fit. */
    int line = 0;
    if (th >= 4 && pg->what) tui_put_str(sf, in, tx, ty + line++, ls_tr(pg->what), LS_ATTR_DIM);
    for (int i = 0; i < pg->n && line < th; i++) {
        const item_t *it = &pg->items[i];
        if (it->hidden && it->hidden()) continue;
        char v[64];
        it->show(v, sizeof(v));
        const uint8_t va = it->on ? TUI_ATTR(it->on() ? (it->warn ? TUI_RED | TUI_BRIGHT : TUI_GREEN | TUI_BRIGHT) : LS_FAINT_FG, TUI_BLACK)
                                  : TUI_ATTR(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        const tui_rect ln = tui_rect_make(tx, ty + line, tw, 1);
        if (tw >= 34) {
            /* label and value */
            tui_put_str(sf, tui_rect_make(tx, ty + line, 17, 1), tx, ty + line, ls_tr(it->label), TUI_ATTR(hue, TUI_BLACK));
            tui_put_str(sf, ln, tx + 18, ty + line, v, va);
        } else {
            /* narrow: a switch is its name lit green or grey, the rest the value */
            tui_put_str(sf, ln, tx, ty + line, it->on ? ls_tr(it->label) : v, va);
        }
        line++;
    }
}

static void draw_root(tui_surface *sf, tui_rect body)
{
    const int q = COUNT(QUICK_ITEMS);
    const bool wide = ls_tui_is_wide() && body.w >= 70;
    tui_rect left = body, right = body;
    if (wide) {
        left.w = body.w * 2 / 5 > 44 ? 44 : body.w * 2 / 5;
        right = tui_rect_make(body.x + left.w + 1, body.y, body.w - left.w - 1, body.h);
    }
    status_line(sf, left, left.y);
    /* the line is a way into POWER as well as a reading */
    hit_add(tui_rect_make(left.x, left.y, left.w, 1), -1, SETP_POWER);
    int y = left.y + 1;
    const int lh = body.h >= 40 || wide ? 4 : 3;
    /* volume and brightness as the radio screens' slider cards (volume in
       SOUND's colour, brightness in DISPLAY's), then mute, lock and rotate */
    /* three across when each has room for its name, else a row each */
    const bool across = left.w >= 45;
    const int cw = left.w / 3;
    /* the cards as tall as the radio screens' where the column has the
       room; on a short pane a row each with the meter */
    int sh = (left.y + left.h - y - (across ? lh : 9)) / 2;
    if (sh > (wide || body.h >= 60 ? 8 : 6)) sh = wide || body.h >= 60 ? 8 : 6;
    for (int i = 0; i < 2; i++) {
        const tui_rect a = tui_rect_make(left.x, y, left.w, sh >= 5 ? sh : lh);
        if (sh >= 5) lv_draw(sf, a, &QUICK_ITEMS[i], i, i == 0 ? TUI_MAGENTA : TUI_CYAN);
        else row_draw(sf, a, &QUICK_ITEMS[i], i, TUI_CYAN, s_sel == i);
        hit_add(a, i, -1);
        y += a.h;
    }
    for (int i = 2; i < q; i++) {
        const int k = i - 2;
        const tui_rect a = across ? tui_rect_make(left.x + k * cw, y, k == 2 ? left.w - 2 * cw : cw, lh)
                                  : tui_rect_make(left.x, y + k * 3, left.w, 3);
        row_draw(sf, a, &QUICK_ITEMS[i], i, TUI_CYAN, s_sel == i);
        hit_add(a, i, -1);
    }
    y += across ? lh : 9;
    /* the six cards: a column in portrait, a grid beside the quick ones in landscape */
    tui_rect area = wide ? right : tui_rect_make(body.x, y, body.w, body.y + body.h - y);
    if (area.h < 3) return;
    const int cols = wide ? (area.w >= 90 ? 3 : 2) : (area.h / N_CARDS >= LS_ICON_ROWS + 2 ? 1 : 2);
    const int rows = (N_CARDS + cols - 1) / cols;
    int ch = area.h / rows;
    if (ch > 12) ch = 12;
    const int cwid = area.w / cols;
    for (int c = 0; c < N_CARDS; c++) {
        const int col = c % cols, row = c / cols;
        const tui_rect a = tui_rect_make(area.x + col * cwid, area.y + row * ch,
                                         col == cols - 1 ? area.w - col * cwid : cwid, ch);
        card_draw(sf, a, c + 1, s_sel == q + c);
        hit_add(a, -1, c + 1);
    }
}

/* ---- a page ------------------------------------------------------------------ */

/* What each row does, for the line under the rows when there is room. */
static const struct { const char *label, *what; } HELP[] = {
    { "Volume", "Speaker and headphone level for every app. Mute keeps the level." },
    { "Brightness", "Panel backlight. Lower saves battery outdoors at night." },
    { "Mute", "Silences the speaker without losing the volume." },
    { "Screen lock", "Locks touch and keys until the unlock gesture." },
    { "Rotate lock", "Holds the screen the way it is when the board turns." },
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
    { "Power mode", "FULL as set. SAVER: half brightness, dims at 30 s, 15 fps. WATCH: also dark after 20 s." },
    { "Watch keeps", "What runs while WATCH has the screen dark. A touch brings the rest back." },
    { "Keyboard light", "Backlight of the keyboard, where there is one." },
    { "Keyboard dim", "Dims the keyboard light with the panel." },
    { "LoRa radio", "Which port the LoRa module answers on." },
    { "LoRa module", "The module type fitted to that port." },
    { "Language", "Words on screen. Numbers and units stay as they are." },
    { "Hotspot", "The board's own Wi-Fi network, for the web pages. Tap twice for a new password." },
    { "USB autoreboot", "Restart when a PC flashes the board over USB." },
    { "Update", "Check for and install new firmware over Wi-Fi." },
    { "Check for updates", "Daily looks for new firmware by itself; Off never does." },
};
static const char *help_for(const char *label)
{
    for (unsigned i = 0; i < sizeof(HELP) / sizeof(HELP[0]); i++)
        if (!strcmp(HELP[i].label, label)) return HELP[i].what;
    if (!strncmp(label, "Service", 7)) return "A native app's background service. A tap stops it.";
    return "A tap changes it.";
}

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

static void draw_page(tui_surface *sf, tui_rect body, const set_page_t *pg)
{
    const uint8_t hue = pg->hue;
    const bool wide = ls_tui_is_wide() && body.w >= 70;
    tui_rect head = body, list = body;
    if (wide) {
        head.w = body.w * 2 / 5 > 46 ? 46 : body.w * 2 / 5;
        list = tui_rect_make(body.x + head.w + 1, body.y, body.w - head.w - 1, body.h);
    }
    /* BACK, and the page's name in its colour */
    const int bh = 3;
    s_back = tui_rect_make(head.x, head.y, 12, bh);
    ls_panel_box(sf, s_back, NULL, HUE_HI(hue));
    tui_fill(sf, tui_rect_make(s_back.x + 1, s_back.y + 1, s_back.w - 2, 1), ' ', TUI_ATTR(TUI_BLACK, HUE_HI(hue)));
    tui_put_str(sf, s_back, s_back.x + 2, s_back.y + 1, ls_tr("< BACK"), TUI_ATTR(TUI_BLACK, HUE_HI(hue)));
    char t[40];
    snprintf(t, sizeof(t), "%s  %s", ls_tr("SETTINGS"), ls_tr(pg->title));
    tui_put_str(sf, head, head.x + 14, head.y + 1, t, TUI_ATTR(HUE_HI(hue), TUI_BLACK));
    for (int c = head.x + 14; c < head.x + head.w - 1; c++)
        ls_box_put(sf, head, c, head.y + 2, LS_BOX_H, TUI_ATTR(hue, TUI_BLACK));
    /* the live panel: the page's art and what its settings act on */
    /* the panel takes what the rows leave, so a short page is not a long
       empty one */
    int ph = wide ? head.h - bh : pg->panel_rows;
    /* never so tall the rows lose their line each */
    if (!wide && ph > body.h - bh - 1 - s_vis_n * (body.h >= 40 ? 3 : 1)) ph = body.h - bh - 1 - s_vis_n * (body.h >= 40 ? 3 : 1);
    if (ph > 22) ph = 22;
    if (ph < 6) ph = 6;
    const tui_rect pb = tui_rect_make(head.x, head.y + bh, head.w, ph);
    if (pb.h >= 4) {
        ls_panel_box(sf, pb, NULL, hue);
        const tui_rect in = tui_rect_make(pb.x + 1, pb.y + 1, pb.w - 2, pb.h - 2);
        int px = in.x + 1;
        if (in.h >= LS_ICON_ROWS && in.w >= LS_ICON_COLS + 24) {
            ls_icon_draw(sf, in, in.x + 1, in.y + (in.h - LS_ICON_ROWS) / 2, pg->icon, TUI_ATTR(HUE_HI(hue), TUI_BLACK));
            px = in.x + LS_ICON_COLS + 3;
        }
        if (pg->panel) pg->panel(sf, tui_rect_make(px, in.y, in.x + in.w - px, in.h), hue);
    }
    if (!wide) list = tui_rect_make(body.x, pb.y + ph + 1, body.w, body.y + body.h - pb.y - ph - 1);
    if (list.h < 2) return;
    /* the rows: boxes when there is height, lines when there is not */
    int rh = s_vis_n ? list.h / s_vis_n : 0;
    if (rh > 7) rh = 7;
    for (int k = 0; k < s_vis_n; k++) {
        const int i = s_vis[k];
        const item_t *it = &pg->items[i];
        tui_rect a;
        if (rh >= 3) {
            a = tui_rect_make(list.x, list.y + k * rh, list.w, rh);
            row_draw(sf, a, it, i, hue, s_sel == k);
        } else {
            a = tui_rect_make(list.x, list.y + k, list.w, 1);
            if (a.y >= list.y + list.h) break;
            line_draw(sf, a, it, i, hue, s_sel == k);
        }
        hit_add(a, i, -1);
    }
    /* what the selected row does, under the rows when there is room */
    const int used = rh >= 3 ? s_vis_n * rh : s_vis_n;
    const int left = list.h - used;
    if (left >= 3 && s_sel < s_vis_n) {
        const item_t *it = &pg->items[s_vis[s_sel]];
        const int y = list.y + used + 1;
        tui_put_str(sf, list, list.x + 1, y, ls_tr(it->label), TUI_ATTR(HUE_HI(hue), TUI_BLACK));
        wrap(sf, list, list.x + 1, y + 1, list.w - 2, left - 2, ls_tr(help_for(it->label)), LS_ATTR_DIM);
    }
}

/* THE DESK FORM: a property list, one row a setting or a page. */
static void draw_compact(tui_surface *sf, tui_rect body, const set_page_t *pg)
{
    int w = body.w > 76 ? 76 : body.w;
    const int x = body.x + 1;
    tui_put_str(sf, body, x + 1, body.y, ls_tr("SETTING"), TUI_ATTR(pg->hue, TUI_BLACK));
    tui_put_str(sf, body, x + 23, body.y, ls_tr("VALUE"), TUI_ATTR(pg->hue, TUI_BLACK));
    for (int c = 0; c < w - 2; c++)
        ls_box_put(sf, body, x + c, body.y + 1, LS_BOX_H, TUI_ATTR(LS_FAINT_FG, TUI_BLACK));
    int y = body.y + 2;
    if (s_page != SETP_ROOT) {
        s_back = tui_rect_make(x, y, w - 2, 1);
        tui_put_str(sf, body, x + 1, y++, ls_tr("< SETTINGS"), TUI_ATTR(HUE_HI(pg->hue), TUI_BLACK));
    }
    for (int k = 0; k < s_vis_n && y < body.y + body.h; k++, y++) {
        const tui_rect row = tui_rect_make(x, y, w - 2, 1);
        line_draw(sf, row, &pg->items[s_vis[k]], s_vis[k], pg->hue, s_sel == k);
        hit_add(row, s_vis[k], -1);
    }
    if (s_page == SETP_ROOT)
        for (int c = 0; c < N_CARDS && y < body.y + body.h; c++, y++) {
            const set_page_t *cp = &SET_PAGES[c + 1];
            const tui_rect row = tui_rect_make(x, y, w - 2, 1);
            const bool sel = s_sel == s_vis_n + c;
            if (sel) tui_fill(sf, row, ' ', TUI_ATTR(TUI_WHITE, TUI_BLUE));
            tui_put_str(sf, row, row.x + 1, y, ls_tr(cp->title), TUI_ATTR(HUE_HI(cp->hue), sel ? TUI_BLUE : TUI_BLACK));
            tui_put_str(sf, row, row.x + 19, y, ls_tr(cp->what), TUI_ATTR(TUI_WHITE, sel ? TUI_BLUE : TUI_BLACK));
            tui_put_char(sf, row, row.x + row.w - 2, y, '>', TUI_ATTR(HUE_HI(cp->hue), sel ? TUI_BLUE : TUI_BLACK));
            hit_add(row, -1, c + 1);
        }
    if (body.w >= w + 28 && s_page != SETP_ROOT && pg->panel && body.h >= 9) {
        const tui_rect side = tui_rect_make(body.x + w + 1, body.y, body.w - w - 1, 8);
        tui_box(sf, side, ls_tr(pg->title), TUI_ATTR(pg->hue, TUI_BLACK));
        pg->panel(sf, tui_rect_make(side.x + 2, side.y + 1, side.w - 4, side.h - 2), pg->hue);
    }
}

static void draw(tui_surface *sf, tui_rect area)
{
    const set_page_t *pg = page_get();
    s_hit_n = 0;
    s_back.w = 0;
    /* a held finger on a slider, against where it was drawn last frame */
    if (s_page == SETP_ROOT) lv_follow(QUICK_ITEMS);
    s_lv[0].w = s_lv[1].w = 0;
    if (s_page == SETP_POWER) services_refresh();
    visible_refresh();
    if (s_sel >= entries()) s_sel = entries() ? entries() - 1 : 0;
    ls_panel_box(sf, area, ls_tr(pg->title), pg->hue);
    if (area.h < 6 || area.w < 18) return;
    const tui_rect body = tui_rect_make(area.x + 1, area.y + 1, area.w - 2, area.h - 2);
    /* the desk form, and a pane too small for boxes: a line a setting */
    if (ls_tui_compact() || body.w < 34 || body.h < 20) { draw_compact(sf, body, pg); return; }
    if (s_page == SETP_ROOT) draw_root(sf, body);
    else draw_page(sf, body, pg);
}

/* ---- input ------------------------------------------------------------------ */

/* The item under selection index `k`, or NULL for a card. */
static const item_t *sel_item(int k, int *index)
{
    const set_page_t *pg = page_get();
    if (k >= 0 && k < s_vis_n) { if (index) *index = s_vis[k]; return &pg->items[s_vis[k]]; }
    return NULL;
}

static void press(const item_t *it)
{
    if (it->next != ap_next) s_ap_armed = false;
    it->next();
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    const int n = entries();
    if (n <= 0) return false;
    if (s_sel >= n) s_sel = 0;
    const item_t *it = sel_item(s_sel, NULL);
    switch (k) {
    case LS_TK_UP:    s_sel = (s_sel + n - 1) % n; return true;
    case LS_TK_DOWN:  s_sel = (s_sel + 1) % n;     return true;
    case LS_TK_ENTER:
        if (it) press(it);
        else if (s_page == SETP_ROOT) page_open(s_sel - s_vis_n + 1);
        return true;
    case LS_TK_LEFT:
    case LS_TK_RIGHT:
        if (!it || !it->step) return false;
        it->step(k == LS_TK_LEFT ? -1 : 1);
        return true;
    case LS_TK_ESC:
    case LS_TK_BACKSPACE:
        if (s_page == SETP_ROOT) return false;
        page_open(SETP_ROOT);
        return true;
    default: return false;
    }
}

/* One press advances the setting. Every item here is a cycle, a level or a
   switch, so there is nothing a second press would confirm (the hotspot's
   new password is the one exception, and arms itself). */
static bool touch(int col, int row)
{
    if (s_back.w && tui_rect_contains(s_back, col, row)) { page_open(SETP_ROOT); return true; }
    for (int h = 0; h < s_hit_n; h++) {
        const hit_t *t = &s_hit[h];
        if (!tui_rect_contains(t->r, col, row)) continue;
        if (t->page > 0) {
            s_sel = s_vis_n + t->page - 1;
            page_open(t->page);
            return true;
        }
        const set_page_t *pg = page_get();
        if (t->item < 0 || t->item >= pg->n) return true;
        const item_t *it = &pg->items[t->item];
        for (int k = 0; k < s_vis_n; k++) if (s_vis[k] == t->item) s_sel = k;
        /* a slider card: - and + step, the bars set the level */
        if (s_page == SETP_ROOT && t->item < 2 && it->set && lv_touch(t->item, it, col, row)) {
            s_ap_armed = false;
            return true;
        }
        if (it->step) {
            /* the keys step it; elsewhere the left half steps down and the
               right half up, so a finger does not have to find the keys */
            if (tui_rect_contains(s_dec[t->item], col, row)) it->step(-1);
            else if (tui_rect_contains(s_inc[t->item], col, row)) it->step(1);
            else if (!ls_tui_compact()) it->step(col < t->r.x + t->r.w / 2 ? -1 : 1);
            s_ap_armed = false;
            return true;
        }
        press(it);
        return true;
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
    visible_refresh();
}

const ls_tui_screen_t ls_scr_settings = {
    .name = "SET",
    .hint = "TAP to change  UP DOWN ENTER  < > step  ESC back",
    .enter = enter,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
