/* The Control Panel's applets: tables of rows over settings the board
   already has. Nothing here invents a setting. A row reads the same getter
   the SETTINGS app, the tray or the console reads, and changes it through
   the same setter, so the two never disagree. What has no setter on the
   board (a time zone, the hotspot switch) is not offered; what would cut
   off the viewer you are using (the web screen) is shown and left to the
   app that owns it. */

#include "ls_desk_priv.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "core/settings.h"
#include "core/display_ctl.h"
#include "core/ls_time.h"
#include "core/ls_version.h"
#include "core/ls_safe_mode.h"
#include "audio/audio_out.h"
#include "audio/speech.h"
#include "audio/tone.h"
#include "board/ls_haptic.h"
#include "ls_keypad.h"
#include "ls_lora_port.h"
#include "ls_gauge.h"
#include "ls_gps.h"
#include "ls_sdcard.h"
#include "../ls_theme.h"
#include "../ls_tui_screen.h"
#include "../ls_notify.h"
#include "../ls_value.h"
#include "../ls_action.h"
#include "../ls_quick.h"
#include "../ls_i18n.h"
#include "../ls_power.h"
#include "../ls_rterm.h"
#include "../ls_radio_select.h"
#include "../ls_wireless.h"

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const char *onoff(bool on) { return on ? "on" : "off"; }

/* A named value as text, or "--" when the board does not publish it. */
static void value_text(const char *path, char *b, size_t n)
{
    ls_val_t v;
    const char *unit = NULL;
    if (!ls_value_read(path, &v, &unit)) { snprintf(b, n, "--"); return; }
    if (v.kind == LS_VAL_TEXT) snprintf(b, n, "%s", v.s ? v.s : "--");
    else if (v.kind == LS_VAL_INT) snprintf(b, n, "%ld%s%s", v.i, unit ? " " : "", unit ? unit : "");
    else if (v.kind == LS_VAL_BOOL) snprintf(b, n, "%s", v.i ? "yes" : "no");
    else if (v.kind == LS_VAL_FLOAT) snprintf(b, n, "%.2f", (double)v.f);
    else snprintf(b, n, "--");
}

/* ============================================================== Display == */

static void scheme_show(char *b, size_t n) { snprintf(b, n, "%s", ls_desk_theme_name(ls_desk_theme())); }
static void scheme_step(int d) { ls_desk_set_theme((ls_desk_theme() + (d < 0 ? LS_DESK_THEME_COUNT - 1 : 1)) % LS_DESK_THEME_COUNT); }
static void wall_show(char *b, size_t n)
{
    const uint32_t w = dk_wall_current();
    if (w & DK_WP_FILE) {
        const char *names[8];
        uint32_t ids[8];
        const int nf = dk_wall_files(names, ids, 8);
        for (int i = 0; i < nf; i++) if (ids[i] == w) { snprintf(b, n, "%s", names[i]); return; }
        snprintf(b, n, "a picture");
    } else snprintf(b, n, "%s", dk_wall_builtin_name((int)w));
}
static void wall_step(int d)
{
    /* the built-in grounds; pictures are picked in Display Properties */
    const int count = dk_wall_builtin_count();
    const uint32_t w = dk_wall_current();
    int i = w & DK_WP_FILE ? 0 : (int)w;
    i = (i + (d < 0 ? count - 1 : 1)) % count;
    dk_prefs_t p = *dk_prefs();
    p.wall = (uint32_t)i;
    ls_desk_apply_prefs(&p);
}
static int anim_on(void) { return dk_prefs()->anim; }
static void anim_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(dk_prefs()->anim)); }
static void anim_step(int d) { (void)d; dk_prefs_t p = *dk_prefs(); p.anim = !p.anim; ls_desk_apply_prefs(&p); }
static int live_on(void) { return dk_prefs()->live_drag; }
static void live_show(char *b, size_t n) { snprintf(b, n, "%s", dk_prefs()->live_drag ? "contents" : "outline"); }
static void live_step(int d) { (void)d; dk_prefs_t p = *dk_prefs(); p.live_drag = !p.live_drag; ls_desk_apply_prefs(&p); }
static int day_on(void) { return ls_tui_daylight(); }
static void day_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(ls_tui_daylight())); }
static void day_step(int d) { (void)d; const bool on = !ls_tui_daylight(); ls_tui_set_daylight(on); settings_set_daylight(on); }
static void bri_show(char *b, size_t n) { snprintf(b, n, "%d %%", display_ctl_get_user()); }
static int bri_pct(void) { return display_ctl_get_user(); }
static void bri_set(int v)
{
    v = (v + 2) / 5 * 5;
    v = v < 5 ? 5 : v > 100 ? 100 : v;             /* never stepped dark */
    if (v != display_ctl_get_user()) display_ctl_set_user(v);
}
static void bri_step(int d) { bri_set(display_ctl_get_user() + d * 10); }
static int dim_on(void) { return display_ctl_autodim_enabled(); }
static void dim_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(display_ctl_autodim_enabled())); }
static void dim_step(int d) { (void)d; display_ctl_set_autodim(!display_ctl_autodim_enabled()); }
static const int DIM_STEPS[] = {15, 30, 60, 120, 240};
static void dimt_show(char *b, size_t n) { snprintf(b, n, "%d s", display_ctl_autodim_timeout()); }
static int dimt_pct(void) { return display_ctl_autodim_timeout() * 100 / 240; }
static void dimt_step(int d)
{
    const int cur = display_ctl_autodim_timeout();
    if (d > 0) { for (int i = 0; i < COUNT(DIM_STEPS); i++) if (DIM_STEPS[i] > cur) { display_ctl_set_autodim_timeout(DIM_STEPS[i]); return; } }
    else for (int i = COUNT(DIM_STEPS) - 1; i >= 0; i--) if (DIM_STEPS[i] < cur) { display_ctl_set_autodim_timeout(DIM_STEPS[i]); return; }
}
static void grid_show(char *b, size_t n)
{
    int cols = 0, rows = 0, cw = 0, ch = 0;
    ls_tui_geometry(&cols, &rows, &cw, &ch);
    snprintf(b, n, "%d x %d cells of %d x %d dots", cols, rows, cw, ch);
}
static void go_show_appearance(char *b, size_t n) { snprintf(b, n, "colours, pictures, effects"); }
/* The desktop's gadgets, one switch each. */
static void gadget_flip(int bit) { dk_shell_set_gadgets(dk_shell_gadgets() ^ bit); }
#define GADGET(name, bit) static int name##_on(void) { return (dk_shell_gadgets() & (bit)) != 0; } \
    static void name##_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(name##_on())); } \
    static void name##_step(int d) { (void)d; gadget_flip(bit); }
GADGET(gclock, DKG_CLOCK) GADGET(gradio, DKG_RADIO) GADGET(gmesh, DKG_MESH) GADGET(gboard, DKG_SYSTEM)
#undef GADGET
static int labels_on(void) { return dk_prefs()->labels; }
static void labels_show(char *b, size_t n) { snprintf(b, n, "%s", dk_prefs()->labels ? "on their own backing" : "on the wallpaper"); }
static void labels_step(int d) { (void)d; dk_prefs_t p = *dk_prefs(); p.labels = !p.labels; ls_desk_apply_prefs(&p); }
static void display_sum(char *b, size_t n) { snprintf(b, n, "%s, %s", ls_desk_theme_name(ls_desk_theme()), ls_tui_daylight() ? "daylight" : "night ground"); }

static const dk_cpl_row_t DISPLAY_ROWS[] = {
    {.label = "Scheme", .kind = CR_CHOICE, .show = scheme_show, .step = scheme_step},
    {.label = "Desktop ground", .kind = CR_CHOICE, .show = wall_show, .step = wall_step},
    {.label = "Daylight", .kind = CR_SWITCH, .show = day_show, .step = day_step, .on = day_on},
    {.label = "Animations", .kind = CR_SWITCH, .show = anim_show, .step = anim_step, .on = anim_on},
    {.label = "Dragging shows", .kind = CR_SWITCH, .show = live_show, .step = live_step, .on = live_on},
    {.label = "Icon names", .kind = CR_SWITCH, .show = labels_show, .step = labels_step, .on = labels_on},
    {.label = "Gadget: clock, GPS", .kind = CR_SWITCH, .show = gclock_show, .step = gclock_step, .on = gclock_on},
    {.label = "Gadget: receiver", .kind = CR_SWITCH, .show = gradio_show, .step = gradio_step, .on = gradio_on},
    {.label = "Gadget: mesh", .kind = CR_SWITCH, .show = gmesh_show, .step = gmesh_step, .on = gmesh_on},
    {.label = "Gadget: board", .kind = CR_SWITCH, .show = gboard_show, .step = gboard_step, .on = gboard_on},
    {.label = "Panel brightness", .kind = CR_LEVEL, .show = bri_show, .step = bri_step, .pct = bri_pct, .set = bri_set},
    {.label = "Panel auto dim", .kind = CR_SWITCH, .show = dim_show, .step = dim_step, .on = dim_on},
    {.label = "Dim after", .kind = CR_LEVEL, .show = dimt_show, .step = dimt_step, .pct = dimt_pct},
    {.label = "This window", .kind = CR_INFO, .show = grid_show},
    {.label = "Appearance", .kind = CR_GO, .show = go_show_appearance, .go = "@display"},
};

/* ================================================================ Sound == */

static void vol_show(char *b, size_t n) { snprintf(b, n, "%d %%", audio_volume_get()); }
static int vol_pct(void) { return audio_volume_get(); }
static void vol_set(int v)
{
    v = (v + 2) / 5 * 5;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v != audio_volume_get()) audio_volume_set(v);
}
static void vol_step(int d) { vol_set(audio_volume_get() + d * 5); }
static int mute_on(void) { return audio_is_muted(); }
static void mute_show(char *b, size_t n) { snprintf(b, n, "%s", audio_is_muted() ? "MUTED" : "sound on"); }
static void mute_step(int d) { (void)d; audio_mute_set(!audio_is_muted()); }      /* explicit, never a blind toggle */
static void boot_show(char *b, size_t n)
{
    static const char *const M[] = {"off", "chirp", "spoken"};
    int m = settings_get_boot_sound();
    if (m == 2 && !speech_available()) m = 1;
    snprintf(b, n, "%s", m >= 0 && m < 3 ? M[m] : "?");
}
static void boot_step(int d)
{
    const int modes = speech_available() ? 3 : 2;
    int m = settings_get_boot_sound();
    if (m == 2 && !speech_available()) m = 1;
    settings_set_boot_sound((m + (d < 0 ? modes - 1 : 1)) % modes);
}
static int ring_on(void) { return settings_get_alert_ring(); }
static void ring_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(settings_get_alert_ring())); }
static void ring_step(int d)
{
    (void)d;
    settings_set_alert_ring(!settings_get_alert_ring());
    ls_notify_set_alerts(settings_get_alert_ring(), ls_notify_vibe());
}
static void nsound_show(char *b, size_t n)
{
    int count = 0, cur = 0;
    const char *const *names = settings_notification_sound_names(&count, &cur);
    snprintf(b, n, "%s", names && cur >= 0 && cur < count ? names[cur] : "?");
}
static void nsound_step(int d) { (void)d; settings_cycle_notification_sound(); (void)snd_alert_start(); }   /* heard, not read */
static bool vibe_now(void) { return ls_haptic_enabled() && settings_get_alert_vibe(); }
static int vibe_on(void) { return vibe_now(); }
static void vibe_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(vibe_now())); }
static void vibe_step(int d)
{
    (void)d;
    const bool en = !vibe_now();
    settings_set_alert_vibe(en);
    if (settings_get_alert_vibe() != en) return;       /* a refused write changes nothing */
    if (!en) ls_haptic_stop();
    ls_haptic_set_enabled(en);
    settings_set_haptics(en);
    ls_notify_set_alerts(ls_notify_ring(), en);
    if (en) (void)ls_haptic_play(LS_HAPTIC_CONFIRM);
}
static void voice_show(char *b, size_t n)
{
    if (speech_available()) snprintf(b, n, "%s %d%%", speech_voice_name(speech_voice_get()), speech_volume_get());
    else snprintf(b, n, "no voice in this build");
}
static void music_go_show(char *b, size_t n) { snprintf(b, n, "play from the card"); }
static void sound_sum(char *b, size_t n)
{
    if (audio_is_muted()) snprintf(b, n, "muted (volume %d %%)", audio_volume_get());
    else snprintf(b, n, "volume %d %%", audio_volume_get());
}

static const dk_cpl_row_t SOUND_ROWS[] = {
    {.label = "Volume", .kind = CR_LEVEL, .show = vol_show, .step = vol_step, .pct = vol_pct, .set = vol_set},
    {.label = "Mute", .kind = CR_SWITCH, .warn = true, .show = mute_show, .step = mute_step, .on = mute_on},
    {.label = "Boot sound", .kind = CR_CHOICE, .show = boot_show, .step = boot_step},
    {.label = "Alert sound", .kind = CR_SWITCH, .show = ring_show, .step = ring_step, .on = ring_on},
    {.label = "Notify sound", .kind = CR_CHOICE, .show = nsound_show, .step = nsound_step},
    {.label = "Vibrate", .kind = CR_SWITCH, .show = vibe_show, .step = vibe_step, .on = vibe_on},
    {.label = "Voice", .kind = CR_GO, .show = voice_show, .go = "set"},
    {.label = "Music player", .kind = CR_GO, .show = music_go_show, .go = "music"},
};

/* ===================================================== Mouse & Keyboard == */

static void dbl_show(char *b, size_t n) { snprintf(b, n, "%d ms", dk_shell_dbl_ms()); }
static int dbl_pct(void) { return (dk_shell_dbl_ms() - 200) * 100 / 700; }
static void dbl_set(int pct) { dk_shell_set_dbl_ms((200 + pct * 7 + 25) / 50 * 50); }
static void dbl_step(int d) { dk_shell_set_dbl_ms(dk_shell_dbl_ms() + d * 50); }

#if defined(ESP_PLATFORM) && defined(CONFIG_LS_BLE_HEAD) && CONFIG_LS_BLE_HEAD
#include "../../../../main/ble_hid.h"
#pragma weak hid_list_bonds
EXT_RAM_BSS_ATTR static hid_device_t s_kb[HID_BONDS];
static int kb_count(void)
{
    if (!hid_list_bonds) return 0;
    uint32_t ver = 0;
    return (int)hid_list_bonds(s_kb, HID_BONDS, &ver);
}
static void kb_show(int i, char *b, size_t n)
{
    if (i >= kb_count()) { snprintf(b, n, "--"); return; }
    snprintf(b, n, "%.24s  %s", s_kb[i].name[0] ? s_kb[i].name : "(unnamed)",
             s_kb[i].state == HID_READY ? "connected" : s_kb[i].reconnect ? "paired, away" : "paired");
}
#else
static int kb_count(void) { return 0; }
static void kb_show(int i, char *b, size_t n) { (void)i; snprintf(b, n, "--"); }
#endif
#define KB_ROW(i) static void kb_show_##i(char *b, size_t n) { kb_show(i, b, n); } \
                  static bool kb_hide_##i(void) { return i >= kb_count(); }
KB_ROW(0) KB_ROW(1) KB_ROW(2) KB_ROW(3)
#undef KB_ROW
static void bt_show(char *b, size_t n)
{
    int up = 0;
    const int bonded = dk_sys_bt(&up);
    if (bonded < 0) snprintf(b, n, "no Bluetooth in this build");
    else snprintf(b, n, "%d paired, %d connected", bonded, up);
}
static bool no_bt(void) { return dk_sys_bt(NULL) < 0; }
/* Pairing is the console's: `ble hid scan`, then `ble hid pair <id>` with
   the passkey it prints. Run the scan in TERMINAL, where its list shows. */
static void pair_show(char *b, size_t n) { snprintf(b, n, "scan in TERMINAL"); }
static void pair_step(int d)
{
    (void)d;
    if (!dk_sys_console("ble hid scan 20"))
        dk_msg("Pair a keyboard", "TERMINAL is busy or not in this build.", "On the console: ble hid scan, then ble hid pair <id>");
}
static int bleboot_on(void) { return settings_get_ble_at_boot(); }
static void bleboot_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(settings_get_ble_at_boot())); }
static void bleboot_step(int d) { (void)d; settings_set_ble_at_boot(!settings_get_ble_at_boot()); }
static int keylight_on(void) { return settings_get_keyboard_light(); }
static void keylight_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(settings_get_keyboard_light())); }
static void keylight_step(int d)
{
    (void)d;
    const bool on = !settings_get_keyboard_light();
    if (ls_keypad_backlight(on) == ESP_OK) settings_set_keyboard_light(on);    /* no keypad: the row stays as it was */
}
static int keydim_on(void) { return display_ctl_keyboard_dim(); }
static void keydim_show(char *b, size_t n) { snprintf(b, n, "%s", display_ctl_keyboard_dim() ? "with the screen" : "off"); }
static void keydim_step(int d) { (void)d; display_ctl_set_keyboard_dim(!display_ctl_keyboard_dim()); }
static void input_sum(char *b, size_t n)
{
    int up = 0;
    const int bonded = dk_sys_bt(&up);
    if (bonded < 0) snprintf(b, n, "double-click %d ms", dk_shell_dbl_ms());
    else snprintf(b, n, "%d keyboard%s, %d connected", bonded, bonded == 1 ? "" : "s", up);
}

static const dk_cpl_row_t INPUT_ROWS[] = {
    {.label = "Double-click time", .kind = CR_LEVEL, .show = dbl_show, .step = dbl_step, .pct = dbl_pct, .set = dbl_set},
    {.label = "Bluetooth keyboards", .kind = CR_INFO, .show = bt_show},
    {.label = "Keyboard 1", .kind = CR_INFO, .show = kb_show_0, .hidden = kb_hide_0},
    {.label = "Keyboard 2", .kind = CR_INFO, .show = kb_show_1, .hidden = kb_hide_1},
    {.label = "Keyboard 3", .kind = CR_INFO, .show = kb_show_2, .hidden = kb_hide_2},
    {.label = "Keyboard 4", .kind = CR_INFO, .show = kb_show_3, .hidden = kb_hide_3},
    {.label = "Pair a keyboard", .kind = CR_ACTION, .show = pair_show, .step = pair_step, .hidden = no_bt},
    {.label = "Bluetooth at boot", .kind = CR_SWITCH, .show = bleboot_show, .step = bleboot_step, .on = bleboot_on},
    {.label = "Keypad light", .kind = CR_SWITCH, .show = keylight_show, .step = keylight_step, .on = keylight_on},
    {.label = "Keypad dims", .kind = CR_SWITCH, .show = keydim_show, .step = keydim_step, .on = keydim_on},
};

/* ============================================================== Network == */

typedef struct { bool connected; char ssid[33], ip[20]; int rssi; bool has_rssi; } net_t;
#ifdef ESP_PLATFORM
#include "../../../../main/ls_wifi.h"
#pragma weak ls_wifi_link_status
static void net_get(net_t *o)
{
    memset(o, 0, sizeof(*o));
    if (!ls_wifi_link_status) return;
    ls_wifi_link_status_t l;
    ls_wifi_link_status(&l);                        /* RAM only: fine every frame */
    o->connected = l.connected;
    snprintf(o->ssid, sizeof(o->ssid), "%s", l.ssid);
    snprintf(o->ip, sizeof(o->ip), "%s", l.ip);
}
#else
EXT_RAM_BSS_ATTR static ls_wireless_snapshot_t s_wl;
static void net_get(net_t *o)
{
    ls_wireless_get(&s_wl);
    memset(o, 0, sizeof(*o));
    o->connected = s_wl.wifi_connected;
    snprintf(o->ssid, sizeof(o->ssid), "%s", s_wl.ssid);
    snprintf(o->ip, sizeof(o->ip), "%s", s_wl.ip);
    o->rssi = s_wl.wifi_rssi;
    o->has_rssi = s_wl.wifi_signal;
}
#endif
static void wifi_show(char *b, size_t n)
{
    net_t t;
    net_get(&t);
    if (t.connected) snprintf(b, n, "joined %s", t.ssid);
    else snprintf(b, n, "not joined");
}
static void ip_show(char *b, size_t n)
{
    net_t t;
    net_get(&t);
    snprintf(b, n, "%s", t.connected && t.ip[0] ? t.ip : "--");
}
static int wifiboot_on(void) { return settings_get_wifi_at_boot(); }
static void wifiboot_show(char *b, size_t n) { snprintf(b, n, "%s", onoff(settings_get_wifi_at_boot())); }
static void wifiboot_step(int d) { (void)d; settings_set_wifi_at_boot(!settings_get_wifi_at_boot()); }
static void ap_show(char *b, size_t n)
{
    ls_val_t on, ssid;
    if (!ls_value_read("wifi.ap_ssid", &ssid, NULL) || ssid.kind != LS_VAL_TEXT || !ssid.s) { snprintf(b, n, "not in this build"); return; }
    const bool up = ls_value_read("wifi.ap_on", &on, NULL) && on.i;
    snprintf(b, n, "%s, %s", ssid.s, up ? "on" : "off");
}
static void appass_show(char *b, size_t n) { value_text("wifi.ap_pass", b, n); }
static bool no_ap(void) { ls_val_t v; return !ls_value_read("wifi.ap_pass", &v, NULL); }
static void ipv4(uint32_t ip, char *b, size_t n)
{
    const uint8_t *p = (const uint8_t *)&ip;         /* network byte order */
    snprintf(b, n, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
}
static void viewers_show(char *b, size_t n)
{
    ls_rterm_status_t rs;
    ls_rterm_status(&rs);
    char ip[20] = "";
    if (rs.web_client) ipv4(rs.web_ip, ip, sizeof(ip));
    snprintf(b, n, "%s%s%s%s%s%s", rs.serial_on ? "USB " : "", rs.web_client ? "web " : "", ip,
             rs.client && rs.authed ? " TCP" : "", rs.ssh_client && rs.ssh_authed ? " SSH" : "",
             rs.serial_on || rs.web_client || (rs.client && rs.authed) || (rs.ssh_client && rs.ssh_authed) ? "" : "nobody watching");
}
static void web_show(char *b, size_t n)
{
    ls_rterm_status_t rs;
    ls_rterm_status(&rs);
    if (rs.web_on) snprintf(b, n, "on, port %d%s", rs.web_port, rs.web_named ? ", by name" : "");
    else snprintf(b, n, "off");
}
static void tcp_show(char *b, size_t n)
{
    ls_rterm_status_t rs;
    ls_rterm_status(&rs);
    if (rs.net_on) snprintf(b, n, "on, port %d", rs.port);
    else snprintf(b, n, "off");
}
static void ssh_show(char *b, size_t n)
{
    ls_rterm_status_t rs;
    ls_rterm_status(&rs);
    if (rs.ssh_on) snprintf(b, n, "%s, port %d, %d key%s", rs.ssh_listening ? "listening" : "waiting for the network", rs.ssh_port, rs.ssh_keys, rs.ssh_keys == 1 ? "" : "s");
    else snprintf(b, n, "off");
}
static void wifi_go_show(char *b, size_t n) { snprintf(b, n, "join, scan, forget"); }
static void link_go_show(char *b, size_t n) { snprintf(b, n, "viewers and passwords"); }
static void net_sum(char *b, size_t n)
{
    net_t t;
    net_get(&t);
    if (t.connected) snprintf(b, n, "%s  %s", t.ssid, t.ip);
    else snprintf(b, n, "Wi-Fi not joined");
}

static const dk_cpl_row_t NET_ROWS[] = {
    {.label = "Wi-Fi", .kind = CR_INFO, .show = wifi_show},
    {.label = "Address", .kind = CR_INFO, .show = ip_show},
    {.label = "Wi-Fi at boot", .kind = CR_SWITCH, .show = wifiboot_show, .step = wifiboot_step, .on = wifiboot_on},
    {.label = "Networks", .kind = CR_GO, .show = wifi_go_show, .go = "wireless"},
    {.label = "Hotspot", .kind = CR_INFO, .show = ap_show},
    {.label = "Hotspot password", .kind = CR_INFO, .show = appass_show, .hidden = no_ap},
    {.label = "Watching now", .kind = CR_INFO, .show = viewers_show},
    {.label = "Web screen", .kind = CR_INFO, .show = web_show},
    {.label = "Network terminal", .kind = CR_INFO, .show = tcp_show},
    {.label = "SSH", .kind = CR_INFO, .show = ssh_show},
    {.label = "Remote screens", .kind = CR_GO, .show = link_go_show, .go = "link"},
};

/* =============================================================== Radios == */

static void rx_show(char *b, size_t n)
{
    const char *r = ls_tui_radio_claimed();
    if (r) snprintf(b, n, "receiving: %s", r);
    else snprintf(b, n, "parked");
}
static void park_show(char *b, size_t n) { snprintf(b, n, "park the receiver"); }
static void park_step(int d) { (void)d; ls_tui_radio_want(NULL); }
static bool nothing_claimed(void) { return ls_tui_radio_claimed() == NULL; }
static void radio_show(int r, char *b, size_t n)
{
    ls_rsel_hw_t hw;
    ls_rsel_hw(&hw);
    if (r >= LS_RSEL_RADIOS) { snprintf(b, n, "--"); return; }
    if (!hw.present[r]) { const char *why = ls_rsel_absent((ls_rsel_radio_t)r); snprintf(b, n, "%s", why && why[0] ? why : "not fitted"); return; }
    snprintf(b, n, "present");
}
static const char *radio_label(int r) { const char *nm = ls_rsel_name((ls_rsel_radio_t)r); return nm && nm[0] ? nm : "radio"; }
#define RADIO_ROW(i) static void radio_show_##i(char *b, size_t n) { char v[48]; radio_show(i, v, sizeof(v)); snprintf(b, n, "%s: %s", radio_label(i), v); } \
                     static bool radio_hide_##i(void) { return i >= LS_RSEL_RADIOS || i == 0; }
RADIO_ROW(1) RADIO_ROW(2) RADIO_ROW(3) RADIO_ROW(4) RADIO_ROW(5)
#undef RADIO_ROW
static void lora_show(char *b, size_t n)
{
    ls_rsel_hw_t hw;
    ls_rsel_hw(&hw);
    snprintf(b, n, "%s", hw.lora_name && hw.lora_name[0] ? hw.lora_name : "none found");
}
static void loraport_show(char *b, size_t n)
{
    ls_lora_port_t next;
    ls_lora_port_saved(&next);
    const char *what = !next.header ? "onboard" : ls_lora_port_is_con1(&next) ? "header CON1"
                     : ls_lora_port_on_socket(&next) ? "header, socket pins" : "header, own pins";
    ls_lora_port_t shown = next;
    if (!shown.header) ls_lora_port_board(&shown);
    if (!ls_lora_port_same(&shown, ls_lora_port())) snprintf(b, n, "%s, reboot to apply", what);
    else snprintf(b, n, "%s%s", what, ls_lora_port_blocks_keyboard() ? ", keyboard off" : "");
}
static void loraport_step(int d) { (void)d; char why[96]; (void)ls_lora_port_toggle(why, sizeof(why)); }
static void loramod_show(char *b, size_t n)
{
    ls_lora_port_t next;
    ls_lora_port_saved(&next);
    const char *what = ls_lora_port_on_socket(&next) || ls_lora_port_is_v03(&next) ? "V0.3 / V1.0"
                     : ls_lora_port_is_v02(&next) ? "V0.2" : "custom (lora port)";
    ls_lora_port_t shown = next;
    if (!shown.header) ls_lora_port_board(&shown);
    if (!ls_lora_port_same(&shown, ls_lora_port())) snprintf(b, n, "%s, reboot to apply", what);
    else snprintf(b, n, "%s", what);
}
static void loramod_step(int d) { (void)d; char why[96]; (void)ls_lora_port_next_module(why, sizeof(why)); }
static void radios_go_show(char *b, size_t n) { snprintf(b, n, "switch each radio on and off"); }
static void radios_sum(char *b, size_t n) { rx_show(b, n); }

static const dk_cpl_row_t RADIO_ROWS[] = {
    {.label = "Receiver", .kind = CR_INFO, .show = rx_show},
    {.label = "Stop receiving", .kind = CR_ACTION, .show = park_show, .step = park_step, .hidden = nothing_claimed},
    {.label = "LoRa chip", .kind = CR_INFO, .show = lora_show},
    {.label = "Radio", .kind = CR_INFO, .show = radio_show_1, .hidden = radio_hide_1},
    {.label = "Radio", .kind = CR_INFO, .show = radio_show_2, .hidden = radio_hide_2},
    {.label = "Radio", .kind = CR_INFO, .show = radio_show_3, .hidden = radio_hide_3},
    {.label = "Radio", .kind = CR_INFO, .show = radio_show_4, .hidden = radio_hide_4},
    {.label = "Radio", .kind = CR_INFO, .show = radio_show_5, .hidden = radio_hide_5},
    {.label = "LoRa radio port", .kind = CR_CHOICE, .show = loraport_show, .step = loraport_step},
    {.label = "LoRa module", .kind = CR_CHOICE, .show = loramod_show, .step = loramod_step},
    {.label = "Power and state", .kind = CR_GO, .show = radios_go_show, .go = "radios"},
};

/* ========================================================= Date and GPS == */

EXT_RAM_BSS_ATTR static ls_gps_state_t s_g;
static const ls_gps_state_t *gps(void) { ls_gps_get(&s_g); return &s_g; }
static bool clock_set(struct tm *g)
{
    const time_t now = time(NULL);
    const struct tm *gp = gmtime(&now);               /* UI task only */
    if (!gp) return false;
    *g = *gp;
    return ls_time_source() > 0 || g->tm_year > 120;
}
static void date_show(char *b, size_t n)
{
    static const char *const DAY[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const MON[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm g;
    if (clock_set(&g)) snprintf(b, n, "%s %d %s %d", DAY[g.tm_wday % 7], g.tm_mday, MON[g.tm_mon % 12], 1900 + g.tm_year);
    else snprintf(b, n, "the clock is not set");
}
static void time_show(char *b, size_t n)
{
    struct tm g;
    if (clock_set(&g)) snprintf(b, n, "%02d:%02d:%02d UTC", g.tm_hour, g.tm_min, g.tm_sec);
    else snprintf(b, n, "--:--:--");
}
static void tsrc_show(char *b, size_t n)
{
    const ls_gps_state_t *g = gps();
    const int src = ls_time_source();
    if (g->alive && g->year >= 2024) snprintf(b, n, "GPS, %d satellites", g->sats_used);
    else snprintf(b, n, "%s", src == 2 ? "network time" : src == 1 ? "board clock (RTC)" : "none yet");
}
static int gps_on(void) { return ls_gps_running(); }
static void gpson_show(char *b, size_t n) { snprintf(b, n, "%s", ls_gps_running() ? "on" : "off"); }
static void gpson_step(int d)
{
    (void)d;
    /* through the board's own action, so power policy and the app agree */
    ls_args_t in = {0};
    in.n = 1;
    in.v[0].kind = LS_VAL_BOOL;
    in.v[0].i = !ls_gps_running();
    ls_val_t out;
    (void)ls_action_call("gps.on", &in, &out, ls_quick_grant_builtin());
}
static void fix_show(char *b, size_t n)
{
    const ls_gps_state_t *g = gps();
    if (!g->alive) snprintf(b, n, "no receiver heard");
    else if (g->fix) snprintf(b, n, "%s fix, HDOP %.1f", g->sats_used >= 4 ? "3D" : "2D", (double)g->hdop);
    else snprintf(b, n, "no fix yet");
}
static void sats_show(char *b, size_t n) { const ls_gps_state_t *g = gps(); snprintf(b, n, "%d used of %d in view", g->sats_used, g->sats_visible); }
static void pos_show(char *b, size_t n)
{
    const ls_gps_state_t *g = gps();
    if (!g->fix) { snprintf(b, n, "--"); return; }
    snprintf(b, n, "%.5f %c  %.5f %c  %.0f m", g->lat_deg < 0 ? -g->lat_deg : g->lat_deg, g->lat_deg < 0 ? 'S' : 'N',
             g->lon_deg < 0 ? -g->lon_deg : g->lon_deg, g->lon_deg < 0 ? 'W' : 'E', (double)g->alt_m);
}
static void gps_go_show(char *b, size_t n) { snprintf(b, n, "sky view and signal"); }
static void date_sum(char *b, size_t n)
{
    struct tm g;
    const ls_gps_state_t *s = gps();
    if (clock_set(&g)) snprintf(b, n, "%02d:%02d UTC, GPS %s", g.tm_hour, g.tm_min, s->fix ? "fix" : s->alive ? "no fix" : "off");
    else snprintf(b, n, "clock not set");
}

static const dk_cpl_row_t DATE_ROWS[] = {
    {.label = "Date", .kind = CR_INFO, .show = date_show},
    {.label = "Time", .kind = CR_INFO, .show = time_show},
    {.label = "Set by", .kind = CR_INFO, .show = tsrc_show},
    {.label = "GPS receiver", .kind = CR_SWITCH, .show = gpson_show, .step = gpson_step, .on = gps_on},
    {.label = "Fix", .kind = CR_INFO, .show = fix_show},
    {.label = "Satellites", .kind = CR_INFO, .show = sats_show},
    {.label = "Position", .kind = CR_INFO, .show = pos_show},
    {.label = "GPS", .kind = CR_GO, .show = gps_go_show, .go = "gps"},
};

/* ================================================================ Power == */

static void pmode_show(char *b, size_t n) { snprintf(b, n, "%s", ls_power_mode_name(ls_power_mode())); }
static void pmode_step(int d) { ls_power_set_mode((ls_pwr_mode_t)(((int)ls_power_mode() + (d < 0 ? LS_PWR_MODES - 1 : 1)) % LS_PWR_MODES)); }
static void pwhat_show(char *b, size_t n) { snprintf(b, n, "%s", ls_power_mode_what(ls_power_mode())); }
static void pkeep_show(char *b, size_t n) { snprintf(b, n, "%s", ls_power_keep_name(ls_power_keep())); }
static void pkeep_step(int d) { ls_power_set_keep((ls_pwr_keep_t)(((int)ls_power_keep() + (d < 0 ? LS_KEEPS - 1 : 1)) % LS_KEEPS)); }
static void batt_show(char *b, size_t n)
{
    ls_gauge_t g;
    if (!ls_gauge_get(&g) || !g.present) { snprintf(b, n, "no battery gauge on this board"); return; }
    snprintf(b, n, "%d %%  %d.%02d V  %s%d mA", g.percent, g.millivolts / 1000, g.millivolts / 10 % 100,
             g.charging ? "charging +" : "", g.milliamps);
}
static int batt_pct(void) { ls_gauge_t g; return ls_gauge_get(&g) && g.present ? g.percent : 0; }
static bool no_gauge(void) { ls_gauge_t g; return !ls_gauge_get(&g) || !g.present; }
static bool has_gauge(void) { return !no_gauge(); }
static void parked_show(char *b, size_t n) { const char *p = ls_power_parked(); snprintf(b, n, "%s", p && p[0] ? p : "nothing"); }
static void power_sum(char *b, size_t n)
{
    ls_gauge_t g;
    if (ls_gauge_get(&g) && g.present) snprintf(b, n, "%s, battery %d %%", ls_power_mode_name(ls_power_mode()), g.percent);
    else snprintf(b, n, "%s", ls_power_mode_name(ls_power_mode()));
}

static const dk_cpl_row_t POWER_ROWS[] = {
    {.label = "Power mode", .kind = CR_CHOICE, .show = pmode_show, .step = pmode_step},
    {.label = "Which means", .kind = CR_INFO, .show = pwhat_show},
    {.label = "WATCH keeps", .kind = CR_CHOICE, .show = pkeep_show, .step = pkeep_step},
    {.label = "Held off by power", .kind = CR_INFO, .show = parked_show},
    {.label = "Battery", .kind = CR_LEVEL, .show = batt_show, .pct = batt_pct, .hidden = no_gauge},
    {.label = "Battery", .kind = CR_INFO, .show = batt_show, .hidden = has_gauge},
    {.label = "Panel auto dim", .kind = CR_SWITCH, .show = dim_show, .step = dim_step, .on = dim_on},
    {.label = "Dim after", .kind = CR_LEVEL, .show = dimt_show, .step = dimt_step, .pct = dimt_pct},
};

/* ============================================================== Storage == */

/* The card's size is a slow read (the FAT's free count): taken when the
   page opens and on Read again, never from a frame. */
static struct { bool have; uint64_t total, free_b; } s_sd;
static void sd_enter(void) { s_sd.have = ls_sdcard_mounted() && ls_sdcard_size(&s_sd.total, &s_sd.free_b); }
static void sd_show(char *b, size_t n)
{
    if (!ls_sdcard_mounted()) { snprintf(b, n, "no card"); return; }
    const char *nm = ls_sdcard_name();
    snprintf(b, n, "mounted%s%s", nm && nm[0] ? ": " : "", nm && nm[0] ? nm : "");
}
static void mb(uint64_t bytes, char *b, size_t n)
{
    if (bytes >= 10ull << 30) snprintf(b, n, "%llu GB", (unsigned long long)(bytes >> 30));
    else snprintf(b, n, "%llu MB", (unsigned long long)(bytes >> 20));
}
static void sdsize_show(char *b, size_t n) { if (s_sd.have) mb(s_sd.total, b, n); else snprintf(b, n, "--"); }
static void sdfree_show(char *b, size_t n)
{
    if (!s_sd.have) { snprintf(b, n, "--"); return; }
    char f[24];
    mb(s_sd.free_b, f, sizeof(f));
    snprintf(b, n, "%s free", f);
}
static int sdused_pct(void) { return s_sd.have && s_sd.total ? (int)((s_sd.total - s_sd.free_b) * 100 / s_sd.total) : 0; }
static void sdread_show(char *b, size_t n) { snprintf(b, n, "read again"); }
static void sdread_step(int d) { (void)d; sd_enter(); }
static void walls_show(char *b, size_t n)
{
    const char *names[8];
    uint32_t ids[8];
    snprintf(b, n, "%d on the card (desk/wallpapers)", dk_wall_files(names, ids, 8));
}
static void folder_go_show(char *b, size_t n) { snprintf(b, n, "browse the card"); }
static void files_go_show(char *b, size_t n) { snprintf(b, n, "copy, delete, play"); }
static void sd_sum(char *b, size_t n) { snprintf(b, n, "%s", ls_sdcard_mounted() ? "card mounted" : "no card"); }

static const dk_cpl_row_t STORAGE_ROWS[] = {
    {.label = "SD card", .kind = CR_INFO, .show = sd_show},
    {.label = "Size", .kind = CR_INFO, .show = sdsize_show},
    {.label = "In use", .kind = CR_LEVEL, .show = sdfree_show, .pct = sdused_pct},
    {.label = "Measure", .kind = CR_ACTION, .show = sdread_show, .step = sdread_step},
    {.label = "Wallpaper pictures", .kind = CR_INFO, .show = walls_show},
    {.label = "Folder window", .kind = CR_GO, .show = folder_go_show, .go = "@folder"},
    {.label = "SD CARD app", .kind = CR_GO, .show = files_go_show, .go = "files"},
};

/* ============================================================== Updates == */

static void ver_show(char *b, size_t n) { snprintf(b, n, "%s", ls_desk_version()); }
static void ota_show(char *b, size_t n)
{
    ls_val_t v;
    if (ls_value_read("ota.state", &v, NULL) && v.kind == LS_VAL_TEXT && v.s) snprintf(b, n, "%s", v.s);
    else snprintf(b, n, "updates are not in this build");
}
static void otach_show(char *b, size_t n) { value_text("ota.channel", b, n); }
static bool no_channel(void) { ls_val_t v; return !ls_value_read("ota.channel", &v, NULL); }
static int updchk_on(void) { return settings_get_update_check(); }
static void updchk_show(char *b, size_t n) { snprintf(b, n, "%s", settings_get_update_check() ? "daily" : "never by itself"); }
static void updchk_step(int d) { (void)d; settings_set_update_check(!settings_get_update_check()); }
/* Checking and installing stay in UPDATE: the owner starts an install there. */
static void upd_go_show(char *b, size_t n) { snprintf(b, n, "check and install"); }
static void upd_sum(char *b, size_t n) { ota_show(b, n); }

static const dk_cpl_row_t UPDATE_ROWS[] = {
    {.label = "Running", .kind = CR_INFO, .show = ver_show},
    {.label = "State", .kind = CR_INFO, .show = ota_show},
    {.label = "Channel", .kind = CR_INFO, .show = otach_show, .hidden = no_channel},
    {.label = "Look for updates", .kind = CR_SWITCH, .show = updchk_show, .step = updchk_step, .on = updchk_on},
    {.label = "UPDATE", .kind = CR_GO, .show = upd_go_show, .go = "update"},
};

/* =============================================================== System == */

static void board_show(char *b, size_t n) { ls_version_info_t v; ls_version_get(&v); snprintf(b, n, "%s", v.board ? v.board : "?"); }
static void built_show(char *b, size_t n) { ls_version_info_t v; ls_version_get(&v); snprintf(b, n, "%s %s, IDF %s", v.date ? v.date : "", v.time ? v.time : "", v.idf ? v.idf : "?"); }
static void up_show(char *b, size_t n)
{
    const long s = (long)(esp_timer_get_time() / 1000000);
    snprintf(b, n, "%ldd %02ld:%02ld:%02ld", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
}
static void ram_show(char *b, size_t n)
{
    dk_perf_t p;
    dk_sys_perf(&p);
    snprintf(b, n, "%ld KB internal, %ld KB PSRAM free", p.internal_kb, p.psram_kb);
}
static void reset_show(char *b, size_t n)
{
    const ls_safe_boot_t *sb = ls_safe_boot_result();
    if (!sb) { snprintf(b, n, "--"); return; }
    snprintf(b, n, "%s%s", ls_safe_reset_name(sb->reset), sb->safe ? "  (SAFE MODE)" : "");
}
static void lang_show(char *b, size_t n) { snprintf(b, n, "%s", ls_i18n_lang_name(ls_i18n_lang())); }
static void lang_step(int d) { (void)d; ls_i18n_set_lang(ls_i18n_next(ls_i18n_lang())); }
static void tasks_go_show(char *b, size_t n) { snprintf(b, n, "windows and memory"); }
static void diag_go_show(char *b, size_t n) { snprintf(b, n, "health, logs, tests"); }
static void bootlog_go_show(char *b, size_t n) { snprintf(b, n, "this boot and the last"); }
static void about_go_show(char *b, size_t n) { snprintf(b, n, "keys and credits"); }
static void sys_sum(char *b, size_t n) { snprintf(b, n, "%s", ls_desk_version()); }

static const dk_cpl_row_t SYSTEM_ROWS[] = {
    {.label = "Firmware", .kind = CR_INFO, .show = ver_show},
    {.label = "Board", .kind = CR_INFO, .show = board_show},
    {.label = "Built", .kind = CR_INFO, .show = built_show},
    {.label = "Up for", .kind = CR_INFO, .show = up_show},
    {.label = "Memory", .kind = CR_INFO, .show = ram_show},
    {.label = "Last reset", .kind = CR_INFO, .show = reset_show},
    {.label = "Language", .kind = CR_CHOICE, .show = lang_show, .step = lang_step},
    {.label = "Task Manager", .kind = CR_GO, .show = tasks_go_show, .go = "@tasks"},
    {.label = "MY BOARD", .kind = CR_GO, .show = diag_go_show, .go = "diag"},
    {.label = "Boot log...", .kind = CR_GO, .show = bootlog_go_show, .go = "bootlog"},
    {.label = "About", .kind = CR_GO, .show = about_go_show, .go = "@about"},
};

/* ============================================================ the panel == */

/* Marks: six cells by three, lines only. */
static const dk_cpl_applet_t APPLETS[] = {
    {"Display", "scheme, ground, effects", TUI_CYAN,
     {"╭────╮", "│╱╲╱ │", "╰─┬┬─╯"},
     DISPLAY_ROWS, COUNT(DISPLAY_ROWS), display_sum, NULL, NULL},
    {"Sound", "volume, alerts, boot sound", TUI_MAGENTA,
     {" ┌╱ ╮ ", "═╡  │ ", " └╲ ╯ "},
     SOUND_ROWS, COUNT(SOUND_ROWS), sound_sum, NULL, "music"},
    {"Mouse & Keyboard", "clicks, Bluetooth keyboards", TUI_YELLOW,
     {"┌┬┬┬┬┐", "├┴┴┴┴┤", "└────┘"},
     INPUT_ROWS, COUNT(INPUT_ROWS), input_sum, NULL, NULL},
    {"Network", "Wi-Fi, hotspot, viewers", TUI_BLUE,
     {" ╭──╮ ", "╭─••─╮", "  ╲╱  "},
     NET_ROWS, COUNT(NET_ROWS), net_sum, NULL, "wireless"},
    {"Radios", "receiver, LoRa, what is fitted", TUI_RED,
     {"╲ │ ╱ ", " ╲│╱  ", "──┴── "},
     RADIO_ROWS, COUNT(RADIO_ROWS), radios_sum, NULL, "radios"},
    {"Date, Time & GPS", "the clock and its source", TUI_GREEN,
     {"╭───╮ ", "│ │─ │ ", "╰───╯ "},
     DATE_ROWS, COUNT(DATE_ROWS), date_sum, NULL, "gps"},
    {"Power", "mode, WATCH, battery", TUI_GREEN,
     {"┌────┐", "│═══ ├", "└────┘"},
     POWER_ROWS, COUNT(POWER_ROWS), power_sum, NULL, NULL},
    {"Storage", "the SD card", TUI_CYAN,
     {"┌───╮ ", "│║║║│ ", "└───┘ "},
     STORAGE_ROWS, COUNT(STORAGE_ROWS), sd_sum, sd_enter, "files"},
    {"Updates", "version, channel, checks", TUI_YELLOW,
     {"  │   ", " ╲│╱  ", "└───┘ "},
     UPDATE_ROWS, COUNT(UPDATE_ROWS), upd_sum, NULL, "update"},
    {"System", "board, memory, language", TUI_WHITE,
     {"┬┬┬┬┬ ", "│ ╱╲ │", "┴┴┴┴┴ "},
     SYSTEM_ROWS, COUNT(SYSTEM_ROWS), sys_sum, NULL, "diag"},
};

int dk_cpl_count(void) { return COUNT(APPLETS); }
const dk_cpl_applet_t *dk_cpl_at(int i) { return i >= 0 && i < COUNT(APPLETS) ? &APPLETS[i] : NULL; }
