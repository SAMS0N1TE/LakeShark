/* The Control Panel's applets: property sheets over settings the board
   already has. Nothing here invents a setting. A control reads the same
   getter the SETTINGS app, the tray or the console reads, and writes it
   through the same setter, so the two never disagree; what it wrote is
   read back after Apply, so a write the board refused shows as refused.
   What has no setter on the board (a time zone, the hotspot switch) is a
   line of fact, not a field; what would cut off the viewer you are using
   (the web screen) is shown and left to the app that owns it. Display is
   Display Properties (ls_desk_props.c). */

#include "ls_desk_sheet.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "core/settings.h"
#include "core/display_ctl.h"
#include "core/ls_time.h"
#include "core/ls_tz.h"
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
#define GROUP(t) {.kind = SH_GROUP, .label = t}
#define FACT(l, f) {.kind = SH_TEXT, .label = l, .show = f}
#define FACT_IF(l, f, h) {.kind = SH_TEXT, .label = l, .show = f, .hidden = h}
#define NOTE(t) {.kind = SH_NOTE, .label = t}
#define CHECK(l, g, s) {.kind = SH_CHECK, .label = l, .get = g, .set = s}
#define DROP(l, g, s, c, t) {.kind = SH_DROP, .label = l, .get = g, .set = s, .count = c, .text = t}
#define OPEN(l, c, where) {.kind = SH_BUTTON, .label = l, .cap = c, .go = where}
#define PAGE(t, c) {.title = t, .ctl = c, .n = COUNT(c)}

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

static void display_sum(char *b, size_t n) { snprintf(b, n, "%s, %s", ls_desk_theme_name(ls_desk_theme()), ls_tui_daylight() ? "daylight" : "night ground"); }

/* ================================================================ Sound == */

static void pct_text(int v, char *b, size_t n) { snprintf(b, n, "%d %%", v); }
static void vol_set(int v)
{
    v = (v + 2) / 5 * 5;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v != audio_volume_get()) audio_volume_set(v);
}
static int mute_get(void) { return audio_is_muted(); }
static void mute_set(int v) { audio_mute_set(v != 0); }                 /* explicit, never a blind toggle */
static int boot_count(void) { return speech_available() ? 3 : 2; }
static int boot_get(void)
{
    int m = settings_get_boot_sound();
    if (m == 2 && !speech_available()) m = 1;
    return m < 0 ? 0 : m;
}
static void boot_set(int v) { settings_set_boot_sound(v); }
static void boot_text(int v, char *b, size_t n)
{
    static const char *const M[] = {"Off", "Chirp", "Spoken"};
    snprintf(b, n, "%s", v >= 0 && v < 3 ? M[v] : "?");
}
static int ring_get(void) { return settings_get_alert_ring(); }
static void ring_set(int v)
{
    settings_set_alert_ring(v != 0);
    ls_notify_set_alerts(settings_get_alert_ring(), ls_notify_vibe());
}
static int nsound_count(void) { int count = 0, cur = 0; (void)settings_notification_sound_names(&count, &cur); return count; }
static int nsound_get(void) { int count = 0, cur = 0; (void)settings_notification_sound_names(&count, &cur); return cur < 0 ? 0 : cur; }
static void nsound_text(int v, char *b, size_t n)
{
    int count = 0, cur = 0;
    const char *const *names = settings_notification_sound_names(&count, &cur);
    snprintf(b, n, "%s", names && v >= 0 && v < count ? names[v] : "?");
}
static void nsound_set(int v)
{
    /* the board's setter steps to the next sound: step until it is the one */
    for (int i = nsound_count(); i > 0 && nsound_get() != v; i--) settings_cycle_notification_sound();
    (void)snd_alert_start();                                             /* heard, not read */
}
static bool vibe_now(void) { return ls_haptic_enabled() && settings_get_alert_vibe(); }
static int vibe_get(void) { return vibe_now(); }
static void vibe_set(int v)
{
    const bool en = v != 0;
    if (en == vibe_now()) return;
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
static void sound_sum(char *b, size_t n)
{
    if (audio_is_muted()) snprintf(b, n, "muted (volume %d %%)", audio_volume_get());
    else snprintf(b, n, "volume %d %%", audio_volume_get());
}

static const dk_sh_ctl_t SND_VOLUME[] = {
    GROUP("Speaker"),
    {.kind = SH_SLIDER, .flags = SHF_LIVE, .label = "&Volume", .get = audio_volume_get, .set = vol_set, .text = pct_text, .min = 0, .max = 100, .step = 5},
    {.kind = SH_CHECK, .flags = SHF_WARN, .label = "&Mute all sound", .get = mute_get, .set = mute_set},
    GROUP("Start-up"),
    DROP("&Boot sound", boot_get, boot_set, boot_count, boot_text),
};
static const dk_sh_ctl_t SND_ALERTS[] = {
    GROUP("When something needs you"),
    CHECK("Play a &sound", ring_get, ring_set),
    DROP("&Notify sound", nsound_get, nsound_set, nsound_count, nsound_text),
    CHECK("V&ibrate", vibe_get, vibe_set),
    NOTE("The notify sound plays once when it is applied."),
};
static const dk_sh_ctl_t SND_MORE[] = {
    GROUP("Voice"),
    FACT("Voice", voice_show),
    OPEN("Voice settings", "Open SETTINGS...", "set"),
    GROUP("Music"),
    OPEN("Music player", "Open MUSIC...", "music"),
};
static const dk_sh_page_t SND_PAGES[] = {PAGE("&Volume", SND_VOLUME), PAGE("A&lerts", SND_ALERTS), PAGE("Voice & M&usic", SND_MORE)};

/* ===================================================== Mouse & Keyboard == */

static void ms_text(int v, char *b, size_t n) { snprintf(b, n, "%d ms", v); }

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
static void pair_press(void)
{
    if (!dk_sys_console("ble hid scan 20"))
        dk_msg("Pair a keyboard", "TERMINAL is busy or not in this build.", "On the console: ble hid scan, then ble hid pair <id>");
}
static int bleboot_get(void) { return settings_get_ble_at_boot(); }
static void bleboot_set(int v) { settings_set_ble_at_boot(v != 0); }
static int keylight_get(void) { return settings_get_keyboard_light(); }
static void keylight_set(int v)
{
    if (ls_keypad_backlight(v != 0) == ESP_OK) settings_set_keyboard_light(v != 0);    /* no keypad: it stays as it was */
}
static int keydim_get(void) { return display_ctl_keyboard_dim(); }
static void keydim_set(int v) { display_ctl_set_keyboard_dim(v != 0); }
static void input_sum(char *b, size_t n)
{
    int up = 0;
    const int bonded = dk_sys_bt(&up);
    if (bonded < 0) snprintf(b, n, "double-click %d ms", dk_shell_dbl_ms());
    else snprintf(b, n, "%d keyboard%s, %d connected", bonded, bonded == 1 ? "" : "s", up);
}

static const dk_sh_ctl_t IN_MOUSE[] = {
    GROUP("Double-click"),
    {.kind = SH_SLIDER, .label = "&Speed", .get = dk_shell_dbl_ms, .set = dk_shell_set_dbl_ms, .text = ms_text, .min = 200, .max = 900, .step = 50},
    NOTE("Two clicks within this time open an icon. Shorter is"),
    NOTE("stricter; longer suits a slow link to the board."),
};
static const dk_sh_ctl_t IN_KEYS[] = {
    GROUP("Bluetooth keyboards"),
    FACT("Paired", bt_show),
    FACT_IF("Keyboard 1", kb_show_0, kb_hide_0),
    FACT_IF("Keyboard 2", kb_show_1, kb_hide_1),
    FACT_IF("Keyboard 3", kb_show_2, kb_hide_2),
    FACT_IF("Keyboard 4", kb_show_3, kb_hide_3),
    {.kind = SH_BUTTON, .label = "Pair another", .cap = "Scan in TERMINAL...", .press = pair_press, .hidden = no_bt},
    CHECK("&Bluetooth on at start-up", bleboot_get, bleboot_set),
    GROUP("Built-in keypad"),
    CHECK("Keypad &light", keylight_get, keylight_set),
    CHECK("&Dim the keypad with the screen", keydim_get, keydim_set),
};
static const dk_sh_page_t IN_PAGES[] = {PAGE("&Mouse", IN_MOUSE), PAGE("&Keyboards", IN_KEYS)};

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
static int wifiboot_get(void) { return settings_get_wifi_at_boot(); }
static void wifiboot_set(int v) { settings_set_wifi_at_boot(v != 0); }
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
static void net_sum(char *b, size_t n)
{
    net_t t;
    net_get(&t);
    if (t.connected) snprintf(b, n, "%s  %s", t.ssid, t.ip);
    else snprintf(b, n, "Wi-Fi not joined");
}

static const dk_sh_ctl_t NET_WIFI[] = {
    GROUP("Connection"),
    FACT("Wi-Fi", wifi_show),
    FACT("Address", ip_show),
    CHECK("&Join Wi-Fi at start-up", wifiboot_get, wifiboot_set),
    GROUP("Networks"),
    OPEN("Join, scan, forget", "Open WIRELESS...", "wireless"),
};
static const dk_sh_ctl_t NET_AP[] = {
    GROUP("Hotspot"),
    FACT("Hotspot", ap_show),
    FACT_IF("Password", appass_show, no_ap),
    NOTE("The hotspot is switched on and off in WIRELESS."),
    OPEN("Hotspot switch", "Open WIRELESS...", "wireless"),
};
static const dk_sh_ctl_t NET_REMOTE[] = {
    GROUP("Watching now"),
    FACT("Viewers", viewers_show),
    GROUP("Ways in"),
    FACT("Web screen", web_show),
    FACT("Network terminal", tcp_show),
    FACT("SSH", ssh_show),
    NOTE("Switching one off here could cut off the screen you are"),
    NOTE("looking at, so the switches stay in LINK."),
    OPEN("Viewers and passwords", "Open LINK...", "link"),
};
static const dk_sh_page_t NET_PAGES[] = {PAGE("&Wi-Fi", NET_WIFI), PAGE("&Hotspot", NET_AP), PAGE("&Remote screens", NET_REMOTE)};

/* =============================================================== Radios == */

static void rx_show(char *b, size_t n)
{
    const char *r = ls_tui_radio_claimed();
    if (r) snprintf(b, n, "receiving: %s", r);
    else snprintf(b, n, "parked");
}
static void park_press(void) { ls_tui_radio_want(NULL); }
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
static void loraport_press(void) { char why[96]; (void)ls_lora_port_toggle(why, sizeof(why)); }
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
static void loramod_press(void) { char why[96]; (void)ls_lora_port_next_module(why, sizeof(why)); }
static void radios_sum(char *b, size_t n) { rx_show(b, n); }

static const dk_sh_ctl_t RAD_RX[] = {
    GROUP("Receiver"),
    FACT("Now", rx_show),
    {.kind = SH_BUTTON, .label = "Stop receiving", .cap = "&Park the receiver", .press = park_press, .off = nothing_claimed},
    GROUP("Fitted"),
    {.kind = SH_NOTE, .show = radio_show_1, .hidden = radio_hide_1},
    {.kind = SH_NOTE, .show = radio_show_2, .hidden = radio_hide_2},
    {.kind = SH_NOTE, .show = radio_show_3, .hidden = radio_hide_3},
    {.kind = SH_NOTE, .show = radio_show_4, .hidden = radio_hide_4},
    {.kind = SH_NOTE, .show = radio_show_5, .hidden = radio_hide_5},
    OPEN("Power and state", "Open RADIOS...", "radios"),
};
static const dk_sh_ctl_t RAD_LORA[] = {
    GROUP("LoRa"),
    FACT("Chip", lora_show),
    FACT("Radio port", loraport_show),
    {.kind = SH_BUTTON, .label = "Change port", .cap = "&Switch port", .press = loraport_press},
    FACT("Module", loramod_show),
    {.kind = SH_BUTTON, .label = "Change module", .cap = "&Next module", .press = loramod_press},
    NOTE("These two are saved at once and take effect after a"),
    NOTE("restart; the line says so until then."),
};
static const dk_sh_page_t RAD_PAGES[] = {PAGE("&Receiver", RAD_RX), PAGE("&LoRa", RAD_LORA)};

/* ========================================================= Date and GPS == */

EXT_RAM_BSS_ATTR static ls_gps_state_t s_g;
static const ls_gps_state_t *gps(void) { ls_gps_get(&s_g); return &s_g; }
static bool clock_set(struct tm *g)
{
    const time_t now = time(NULL);
    if (!ls_tz_local(now, g)) return false;           /* the zone SET picked */
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
    char zone[12];
    if (clock_set(&g)) { ls_tz_abbr(&g, zone, sizeof(zone)); snprintf(b, n, "%02d:%02d:%02d %s", g.tm_hour, g.tm_min, g.tm_sec, zone); }
    else snprintf(b, n, "--:--:--");
}
static void tsrc_show(char *b, size_t n)
{
    const ls_gps_state_t *g = gps();
    const int src = ls_time_source();
    if (g->alive && g->year >= 2024) snprintf(b, n, "GPS, %d satellites", g->sats_used);
    else snprintf(b, n, "%s", src == 2 ? "network time" : src == 3 ? "GPS" : src == 1 ? "board clock (RTC)" : "none yet");
}
static int gpson_get(void) { return ls_gps_running(); }
static void gpson_set(int v)
{
    /* through the board's own action, so power policy and the app agree */
    ls_args_t in = {0};
    in.n = 1;
    in.v[0].kind = LS_VAL_BOOL;
    in.v[0].i = v != 0;
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
static int sats_get(void) { return gps()->sats_used; }
static void pos_show(char *b, size_t n)
{
    const ls_gps_state_t *g = gps();
    if (!g->fix) { snprintf(b, n, "--"); return; }
    snprintf(b, n, "%.5f %c  %.5f %c  %.0f m", g->lat_deg < 0 ? -g->lat_deg : g->lat_deg, g->lat_deg < 0 ? 'S' : 'N',
             g->lon_deg < 0 ? -g->lon_deg : g->lon_deg, g->lon_deg < 0 ? 'W' : 'E', (double)g->alt_m);
}
static void date_sum(char *b, size_t n)
{
    struct tm g;
    const ls_gps_state_t *s = gps();
    if (clock_set(&g)) snprintf(b, n, "%02d:%02d UTC, GPS %s", g.tm_hour, g.tm_min, s->fix ? "fix" : s->alive ? "no fix" : "off");
    else snprintf(b, n, "clock not set");
}

static const dk_sh_ctl_t DATE_CLOCK[] = {
    GROUP("Clock"),
    FACT("Date", date_show),
    FACT("Time", time_show),
    FACT("Set by", tsrc_show),
    NOTE("The clock sets itself from GPS or the network and is"),
    NOTE("kept in UTC; the board has no time zone to choose."),
};
static const dk_sh_ctl_t DATE_GPS[] = {
    GROUP("Receiver"),
    CHECK("&GPS receiver on", gpson_get, gpson_set),
    FACT("Fix", fix_show),
    {.kind = SH_GAUGE, .label = "Satellites", .get = sats_get, .show = sats_show, .min = 0, .max = 16},
    FACT("Position", pos_show),
    GROUP("More"),
    OPEN("Sky view and signal", "Open GPS...", "gps"),
};
static const dk_sh_page_t DATE_PAGES[] = {PAGE("&Date & Time", DATE_CLOCK), PAGE("&GPS", DATE_GPS)};

/* ================================================================ Power == */

static int pmode_count(void) { return LS_PWR_MODES; }
static int pmode_get(void) { return (int)ls_power_mode(); }
static void pmode_set(int v) { ls_power_set_mode((ls_pwr_mode_t)v); }
static void pmode_text(int v, char *b, size_t n) { snprintf(b, n, "%s", ls_power_mode_name(v)); }
static int pkeep_count(void) { return LS_KEEPS; }
static int pkeep_get(void) { return (int)ls_power_keep(); }
static void pkeep_set(int v) { ls_power_set_keep((ls_pwr_keep_t)v); }
static void pkeep_text(int v, char *b, size_t n) { snprintf(b, n, "%s", ls_power_keep_name(v)); }
enum { PWR_MODE_AT = 1 };                   /* where the mode's list sits on its tab */
/* What the mode in the list means, before it is applied. */
static void pwhat_show(char *b, size_t n)
{
    const dk_sheet_t *s = dk_sheet_now();
    int m = (int)ls_power_mode();
    if (s && s->pages[0].n > PWR_MODE_AT && s->pages[0].ctl[PWR_MODE_AT].get == pmode_get) m = dk_sheet_val(&s->pages[0].ctl[PWR_MODE_AT]);
    snprintf(b, n, "%s", ls_power_mode_what(m));
}
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
static int dim_get(void) { return display_ctl_autodim_enabled(); }
static void dim_set(int v) { display_ctl_set_autodim(v != 0); }
static const int DIM_STEPS[] = {15, 30, 60, 120, 240};
static int dimt_get(void)
{
    const int cur = display_ctl_autodim_timeout();
    int at = 0;
    for (int i = 0; i < COUNT(DIM_STEPS); i++) if (DIM_STEPS[i] <= cur) at = i;
    return at;
}
static void dimt_set(int v) { if (v >= 0 && v < COUNT(DIM_STEPS)) display_ctl_set_autodim_timeout(DIM_STEPS[v]); }
static void dimt_text(int v, char *b, size_t n)
{
    const int s = DIM_STEPS[v >= 0 && v < COUNT(DIM_STEPS) ? v : 0];
    if (s >= 120) snprintf(b, n, "%d minutes", s / 60);
    else snprintf(b, n, "%d seconds", s);
}
static void power_sum(char *b, size_t n)
{
    ls_gauge_t g;
    if (ls_gauge_get(&g) && g.present) snprintf(b, n, "%s, battery %d %%", ls_power_mode_name(ls_power_mode()), g.percent);
    else snprintf(b, n, "%s", ls_power_mode_name(ls_power_mode()));
}

static const dk_sh_ctl_t PWR_MODE[] = {
    GROUP("Power mode"),
    DROP("&Mode", pmode_get, pmode_set, pmode_count, pmode_text),           /* PWR_MODE_AT */
    FACT("Which means", pwhat_show),
    DROP("&WATCH keeps", pkeep_get, pkeep_set, pkeep_count, pkeep_text),
    GROUP("Now"),
    FACT("Held off by power", parked_show),
};
static const dk_sh_ctl_t PWR_BATT[] = {
    GROUP("Battery"),
    {.kind = SH_GAUGE, .label = "Charge", .get = batt_pct, .show = batt_show, .min = 0, .max = 100, .hidden = no_gauge},
    FACT_IF("Battery", batt_show, has_gauge),
    GROUP("Screen"),
    CHECK("&Dim the panel when nothing is touched", dim_get, dim_set),
    {.kind = SH_SPIN, .label = "Dim a&fter", .get = dimt_get, .set = dimt_set, .text = dimt_text, .min = 0, .max = COUNT(DIM_STEPS) - 1, .step = 1},
};
static const dk_sh_page_t PWR_PAGES[] = {PAGE("Power &mode", PWR_MODE), PAGE("&Battery & screen", PWR_BATT)};

/* ============================================================== Storage == */

/* The card's size is a slow read (the FAT's free count): taken when the
   page opens and on Measure again, never from a frame. */
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
static void walls_show(char *b, size_t n)
{
    const char *names[8];
    uint32_t ids[8];
    snprintf(b, n, "%d on the card (desk/wallpapers)", dk_wall_files(names, ids, 8));
}
static void sd_sum(char *b, size_t n) { snprintf(b, n, "%s", ls_sdcard_mounted() ? "card mounted" : "no card"); }

static const dk_sh_ctl_t SD_CARD[] = {
    GROUP("SD card"),
    FACT("Card", sd_show),
    FACT("Size", sdsize_show),
    {.kind = SH_GAUGE, .label = "In use", .get = sdused_pct, .show = sdfree_show, .min = 0, .max = 100},
    {.kind = SH_BUTTON, .label = "Measure", .cap = "&Measure again", .press = sd_enter},
    GROUP("On the card"),
    FACT("Wallpaper pictures", walls_show),
    OPEN("Folder window", "Browse the card...", "@folder"),
    OPEN("SD CARD app", "Copy, delete, play...", "files"),
};
static const dk_sh_page_t SD_PAGES[] = {{.title = "&SD card", .ctl = SD_CARD, .n = COUNT(SD_CARD), .enter = sd_enter}};

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
static int updchk_get(void) { return settings_get_update_check(); }
static void updchk_set(int v) { settings_set_update_check(v != 0); }
static void upd_sum(char *b, size_t n) { ota_show(b, n); }

/* Checking and installing stay in UPDATE: the owner starts an install there. */
static const dk_sh_ctl_t UPD_CTL[] = {
    GROUP("This board"),
    FACT("Running", ver_show),
    FACT("State", ota_show),
    FACT_IF("Channel", otach_show, no_channel),
    GROUP("Checking"),
    CHECK("&Look for updates once a day", updchk_get, updchk_set),
    OPEN("Check and install", "Open UPDATE...", "update"),
};
static const dk_sh_page_t UPD_PAGES[] = {PAGE("&Updates", UPD_CTL)};

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
static int lang_count(void) { return LS_LANG_COUNT; }
static int lang_get(void) { return (int)ls_i18n_lang(); }
static void lang_set(int v) { if (ls_i18n_lang_valid(v)) ls_i18n_set_lang((ls_lang_t)v); }
static void lang_text(int v, char *b, size_t n) { snprintf(b, n, "%s", ls_i18n_lang_name((ls_lang_t)v)); }
static void sys_sum(char *b, size_t n) { snprintf(b, n, "%s", ls_desk_version()); }

static const dk_sh_ctl_t SYS_GENERAL[] = {
    GROUP("This board"),
    FACT("Firmware", ver_show),
    FACT("Board", board_show),
    FACT("Built", built_show),
    FACT("Up for", up_show),
    FACT("Memory", ram_show),
    FACT("Last reset", reset_show),
    GROUP("Language"),
    DROP("&Language", lang_get, lang_set, lang_count, lang_text),
};
static const dk_sh_ctl_t SYS_TOOLS[] = {
    GROUP("Tools"),
    OPEN("Task Manager", "Windows and memory...", "@tasks"),
    OPEN("MY BOARD", "Health, logs, tests...", "diag"),
    OPEN("Boot log", "This boot and the last...", "bootlog"),
    OPEN("About", "Keys and credits...", "@about"),
};
static const dk_sh_page_t SYS_PAGES[] = {PAGE("&General", SYS_GENERAL), PAGE("&Tools", SYS_TOOLS)};

/* ============================================================ the panel == */

/* Marks: six cells by three, lines only. */
static const dk_cpl_applet_t APPLETS[] = {
    {"Display", "Scheme, wallpaper, panel light", TUI_CYAN,
     {"╭────╮", "│╱╲╱ │", "╰─┬┬─╯"}, display_sum, NULL},
    {"Sound", "Volume, alerts, voice", TUI_MAGENTA,
     {" ┌╱ ╮ ", "═╡  │ ", " └╲ ╯ "}, sound_sum, "music"},
    {"Mouse & Keyboard", "Double-click, keyboards, keypad", TUI_YELLOW,
     {"┌┬┬┬┬┐", "├┴┴┴┴┤", "└────┘"}, input_sum, NULL},
    {"Network", "Wi-Fi, hotspot, remote screens", TUI_BLUE,
     {" ╭──╮ ", "╭─••─╮", "  ╲╱  "}, net_sum, "wireless"},
    {"Radios", "Receiver, LoRa, what is fitted", TUI_RED,
     {"╲ │ ╱ ", " ╲│╱  ", "──┴── "}, radios_sum, "radios"},
    {"Date, Time & GPS", "Clock, its source, GPS receiver", TUI_GREEN,
     {"╭───╮ ", "│ │─ │ ", "╰───╯ "}, date_sum, "gps"},
    {"Power", "Power mode, battery, dimming", TUI_GREEN,
     {"┌────┐", "│═══ ├", "└────┘"}, power_sum, NULL},
    {"Storage", "The SD card and what is on it", TUI_CYAN,
     {"┌───╮ ", "│║║║│ ", "└───┘ "}, sd_sum, "files"},
    {"Updates", "Version, channel, daily check", TUI_YELLOW,
     {"  │   ", " ╲│╱  ", "└───┘ "}, upd_sum, "update"},
    {"System", "Board, memory, language, tools", TUI_WHITE,
     {"┬┬┬┬┬ ", "│ ╱╲ │", "┴┴┴┴┴ "}, sys_sum, "diag"},
};

#define SHEET(t, p, h) {.title = t, .pages = p, .n = COUNT(p), .hue = h}
static const dk_sheet_t SHEETS[] = {
    {0},                                                 /* Display: dk_display_sheet() */
    SHEET("Sound Properties", SND_PAGES, TUI_MAGENTA),
    SHEET("Mouse & Keyboard Properties", IN_PAGES, TUI_YELLOW),
    SHEET("Network Properties", NET_PAGES, TUI_BLUE),
    SHEET("Radios Properties", RAD_PAGES, TUI_RED),
    SHEET("Date, Time & GPS Properties", DATE_PAGES, TUI_GREEN),
    SHEET("Power Properties", PWR_PAGES, TUI_GREEN),
    SHEET("Storage Properties", SD_PAGES, TUI_CYAN),
    SHEET("Updates Properties", UPD_PAGES, TUI_YELLOW),
    SHEET("System Properties", SYS_PAGES, TUI_WHITE),
};
_Static_assert(COUNT(SHEETS) == COUNT(APPLETS), "a sheet for every applet");

int dk_cpl_count(void) { return COUNT(APPLETS); }
const dk_cpl_applet_t *dk_cpl_at(int i) { return i >= 0 && i < COUNT(APPLETS) ? &APPLETS[i] : NULL; }
const dk_sheet_t *dk_cpl_sheet(int i)
{
    if (i == DK_CPL_DISPLAY) return dk_display_sheet();
    return i > 0 && i < COUNT(SHEETS) ? &SHEETS[i] : NULL;
}
