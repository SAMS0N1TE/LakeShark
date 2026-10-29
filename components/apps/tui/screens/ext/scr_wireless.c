#include "../../ls_rid_ui.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_wireless.h"
#include "../../ls_survey_ui.h"
#include "../../ls_options.h"
#include "../../ls_rterm.h"
#include "esp_attr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ls_textpack.h"

#define INK(c) TUI_ATTR((c), TUI_BLACK)
#define WHITE INK(TUI_WHITE | TUI_BRIGHT)
#define CYAN INK(TUI_CYAN | TUI_BRIGHT)

typedef struct { tui_rect rect; char key; int ap; } hit_t;
#define HIT_MAX 32
static hit_t s_hits[HIT_MAX];
static int s_hit_count, s_tab, s_selected, s_page_size = 3;
static int s_saved_selected = -1;
static int s_saved_page_size = 1;
static char s_saved_name[33];
static EXT_RAM_BSS_ATTR ls_wireless_snapshot_t s_view;
static char s_join_ssid[33], s_note[80];
static bool s_join_secure;
static char s_forget_name[33];

static tui_rect inset(tui_rect a)
{
    return tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
}

static void line(tui_surface *sf, tui_rect a, int row, const char *text, uint8_t attr)
{
    tui_put_str(sf, tui_rect_make(a.x + 1, a.y, a.w - 2, a.h), a.x + 1, a.y + row, text, attr);
}

static void button(tui_surface *sf, tui_rect r, const char *label, char key,
                   bool on, bool dim, int ap)
{
    uint8_t hue = dim ? LS_FAINT_FG : on ? TUI_GREEN | TUI_BRIGHT : TUI_CYAN;
    ls_fill_dither(sf, inset(r), on ? LS_DITHER_MEDIUM : LS_DITHER_LIGHT, hue);
    ls_panel_box(sf, r, "", hue);
    char text[48];
    if (ls_tui_keyboard_mode() && (key == '[' || key == ']'))
        snprintf(text, sizeof(text), "%s KEY / %s", key == '[' ? "[" : "]", label);
    else if (ls_tui_keyboard_mode()) snprintf(text, sizeof(text), "[%c] %s", key, label);
    else snprintf(text, sizeof(text), "%s", label);
    ls_dither_label(sf, inset(r), (r.h - 3) / 2,
                    text, dim ? LS_ATTR_DIM : WHITE);
    if (s_hit_count < (int)(sizeof(s_hits) / sizeof(s_hits[0])))
        s_hits[s_hit_count++] = (hit_t){r, dim ? 0 : key, ap};
}

static void request(ls_wireless_op_t op, const char *ssid, const char *pass)
{
    if (ls_wireless_request(op, ssid, pass)) s_note[0] = 0;
    else LS_SNPUTS(t_scr_wireless_a_connection_update_is, s_note, sizeof(s_note), "A connection update is already running");
}

static void password_open(bool retry);

static void password_done(const char *pass)
{
    size_t n = strlen(pass);
    if ((s_join_secure && n < 8) || (n && (n < 8 || n > 63))) {
        password_open(true);
        return;
    }
    request(LS_WIRELESS_JOIN, s_join_ssid, pass);
}

static void password_open(bool retry)
{
    char title[96];
    snprintf(title, sizeof(title), "%s / %s", s_join_ssid,
        retry ? "USE 8-63 CHARACTERS" : s_join_secure ? "PASSWORD" : "PASSWORD / OPEN: EMPTY");
    ls_keyboard_open_secret(title, 63, password_done);
}

static void forget_done(const char *answer)
{
    char confirm[4];
    snprintf(confirm, sizeof(confirm), "%s", answer);
    for (int i = 0; confirm[i]; ++i)
        if (confirm[i] >= 'a' && confirm[i] <= 'z') confirm[i] += 'A' - 'a';
    if (strcmp(confirm, "YES")) return;
    ls_wireless_get(&s_view);
    for (int i = 0; i < s_view.saved_count; ++i)
        if (!strcmp(s_forget_name, s_view.saved[i])) {
            request(LS_WIRELESS_FORGET, s_forget_name, NULL);
            return;
        }
    LS_SNPUTS(t_scr_wireless_that_saved_network_is, s_note, sizeof(s_note), "That saved network is no longer present");
}

static void ssid_done(const char *ssid)
{
    if (!ssid[0]) return;
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", ssid);
    s_join_secure = false;
    password_open(false);
}

static void join_ap(int index)
{
    if (s_view.busy || index < 0 || index >= s_view.ap_count) return;
    s_selected = index;
    const ls_wireless_ap_t *ap = &s_view.aps[index];
    snprintf(s_join_ssid, sizeof(s_join_ssid), "%s", ap->ssid);
    s_join_secure = ap->secure;
    if (ap->secure) password_open(false);
    else request(LS_WIRELESS_JOIN, s_join_ssid, "");
}

static void bt_action(const ls_opt_t *o)
{
    request(o->arg ? LS_WIRELESS_BT_UNPIN : LS_WIRELESS_BT_PIN,
            o->arg ? NULL : s_view.address, NULL);
}
static const char *bt_why(const ls_opt_t *o)
{
    ls_wireless_get(&s_view);
    if (s_view.busy) return "Connection update running";
    if (!s_view.bt_available) return "Bluetooth unavailable";
    if (!o->arg && !s_view.bt_ready) return "Connect a head first";
    return NULL;
}
static void bt_show(const ls_opt_t *o, char *out, size_t n)
{
    ls_wireless_get(&s_view);
    snprintf(out, n, "%s", o->arg ? s_view.pinned : s_view.peer);
}
static const char *bt_text(const ls_opt_t *o)
{
    (void)o; ls_wireless_get(&s_view);
    return s_view.name_filter;
}
static void bt_set_text(const ls_opt_t *o, const char *text)
{
    (void)o; request(LS_WIRELESS_BT_FILTER, text, NULL);
}
static double bt_num(const ls_opt_t *o)
{
    (void)o; ls_wireless_get(&s_view); return s_view.tel_hz;
}
static void bt_set_num(const ls_opt_t *o, double value)
{
    (void)o; char text[8]; snprintf(text, sizeof(text), "%d", (int)value);
    request(LS_WIRELESS_BT_TELEMETRY, text, NULL);
}
static const ls_opt_t BT_OPTIONS[] = {
    {.label="PIN CONNECTED HEAD", .kind=LS_OPT_ACTION, .act=bt_action, .show=bt_show, .why_not=bt_why},
    {.label="UNPIN HEAD", .kind=LS_OPT_ACTION, .arg=1, .act=bt_action, .show=bt_show, .why_not=bt_why},
    {.label="HEAD NAME FILTER", .kind=LS_OPT_TEXT, .arg=2, .text=bt_text, .set_text=bt_set_text, .max=23, .why_not=bt_why},
    {.label="TELEMETRY RATE", .kind=LS_OPT_LEVEL, .arg=3, .num=bt_num, .set_num=bt_set_num,
     .lo=0, .hi=20, .step=1, .unit="Hz / 0 off", .why_not=bt_why},
};
static const ls_opt_ctx_t BT_CTX = {.name="BLUETOOTH", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(BT_OPTIONS)};

/* ---- REMOTE: the board from a phone or a PC ------------------------------ */

static bool s_show_pass;

static void pass_done(const char *p)
{
    if (!p || !*p) return;
    if (ls_rterm_password_set(p)) {
        s_show_pass = true;
        LS_SNPUTS(t_scr_wireless_password_changed_browsers_sign, s_note, sizeof(s_note), "Password changed: browsers sign in again");
    } else LS_SNPUTS(t_scr_wireless_a_password_is_6, s_note, sizeof(s_note), "A password is 6 to 23 characters");
}

static void port_done(const char *t)
{
    const int port = t ? atoi(t) : 0;
    if (ls_rterm_ssh_port_set(port)) snprintf(s_note, sizeof(s_note), "SSH port %d", port);
    else LS_SNPUTS(t_scr_wireless_a_port_is_1, s_note, sizeof(s_note), "A port is 1 to 65535, and not 2323");
}

static void forget_keys_done(const char *answer)
{
    if (!answer || strcasecmp(answer, "yes")) return;
    snprintf(s_note, sizeof(s_note), ls_rterm_ssh_keys_clear() ? "Keys forgotten: the password again"
                                                               : "Could not save the setting");
}

static void remote_act(char ch)
{
    ls_rterm_status_t st;
    ls_rterm_status(&st);
    s_note[0] = 0;
    switch (ch) {
    case 't':
        if (!ls_rterm_net_set(!st.net_on)) LS_SNPUTS(t_scr_wireless_could_not_save_the, s_note, sizeof(s_note), "Could not save the setting");
        else if (!st.net_on) s_show_pass = true;
        break;
    case 'h': s_show_pass = !s_show_pass; break;
    case 'n':
        if (ls_rterm_password_set(NULL)) {
            s_show_pass = true;
            LS_SNPUTS(t_scr_wireless_new_password_browsers_sign, s_note, sizeof(s_note), "New password: browsers sign in again");
        } else LS_SNPUTS(t_scr_wireless_could_not_save_the_2, s_note, sizeof(s_note), "Could not save the password");
        break;
    case 'o':
        if (!ls_rterm_web_set(!st.web_on)) LS_SNPUTS(t_scr_wireless_could_not_save_the, s_note, sizeof(s_note), "Could not save the setting");
        else if (!st.web_on) s_show_pass = true;
        break;
    case 'x':
        if (st.web_client) { ls_rterm_web_kick(); snprintf(s_note, sizeof(s_note), "Browsers signed out"); }
        break;
    case 'p': ls_keyboard_open_lower(LS_TEXT(t_scr_wireless_terminal_password_6_23, "TERMINAL PASSWORD / 6-23 CHARACTERS"), "", 23, pass_done); break;
    case 'k':
        if (st.client || st.ssh_client) { ls_rterm_kick(); snprintf(s_note, sizeof(s_note), "Disconnected"); }
        break;
    case 's':
        if (!ls_rterm_ssh_set(!st.ssh_on)) snprintf(s_note, sizeof(s_note), "Could not start SSH");
        else if (!st.ssh_on) LS_SNPUTS(t_scr_wireless_ssh_on_check_the, s_note, sizeof(s_note), "SSH on: check the key shown on the first connect");
        break;
    case 'r': {                              /* 'o' and 'x' are the web screen's */
        char now[8];
        snprintf(now, sizeof(now), "%d", st.ssh_port);
        ls_keyboard_open_plain("SSH PORT / 1-65535", now, 5, port_done);
        break;
    }
    case 'l':
        ls_rterm_ssh_learn(!st.ssh_learn_s);
        if (!st.ssh_learn_s) LS_SNPUTS(t_scr_wireless_now_log_in_once, s_note, sizeof(s_note), "Now log in once with the password");
        break;
    case 'f':
        if (st.ssh_keys) ls_keyboard_open_plain(LS_TEXT(t_scr_wireless_forget_the_ssh_keys, "FORGET THE SSH KEYS? TYPE YES"), "", 3, forget_keys_done);
        break;
    }
}

static void remote(tui_surface *sf, tui_rect area, bool wide)
{
    ls_rterm_status_t st;
    ls_rterm_status(&st);
    char text[96];
    const uint8_t *ip = (const uint8_t *)&st.client_ip;
    const int bh = wide ? 3 : 4;

    /* The web screen first: the way in for a phone, no app and real touch. */
    tui_rect web = tui_rect_make(area.x, area.y, area.w, 6);
    ls_panel_box(sf, web, "WEB SCREEN", st.web_on ? TUI_GREEN : TUI_CYAN);
    if (!st.web_on) {
        line(sf, web, 1, "OFF", LS_ATTR_DIM);
        line(sf, web, 2, LS_TEXT(t_scr_wireless_the_screen_in_a, "The screen in a browser: real touch, no app"), WHITE);
    } else if (!s_view.wifi_connected) {
        line(sf, web, 1, LS_TEXT(t_scr_wireless_on_waiting_for_wi, "ON  waiting for Wi-Fi (WI-FI tab)"), INK(TUI_YELLOW | TUI_BRIGHT));
    } else {
        /* The name first: it is what to save to a home screen, and it
           outlives the address the router gives out today. */
        int row = 1;
        if (st.web_named) {
            line(sf, web, row++, "ON   http://" LS_RTERM_WEB_NAME ".local/", INK(TUI_GREEN | TUI_BRIGHT));
            snprintf(text, sizeof(text), "  or http://%s/", s_view.ip);
            line(sf, web, row++, text, WHITE);
        } else {
            snprintf(text, sizeof(text), "ON   http://%s/", s_view.ip);
            line(sf, web, row++, text, INK(TUI_GREEN | TUI_BRIGHT));
        }
    }
    if (st.web_on) {
        const uint8_t *wip = (const uint8_t *)&st.web_ip;
        if (st.web_client) LS_SNPRINTF(t_scr_wireless_u_u_u_u, text, sizeof(text), "%u.%u.%u.%u is watching", wip[0], wip[1], wip[2], wip[3]);
        else snprintf(text, sizeof(text), "Nobody watching");
        line(sf, web, 3, text, st.web_client ? CYAN : LS_ATTR_DIM);
    }
    line(sf, web, 4, LS_TEXT(t_scr_wireless_the_terminal_s_password, "The terminal's password, asked once"), LS_ATTR_DIM);
    const int ww = area.w / 2, wy = web.y + web.h;
    button(sf, tui_rect_make(area.x, wy, ww, bh), st.web_on ? "TURN OFF" : "TURN ON", 'O', st.web_on, false, -1);
    button(sf, tui_rect_make(area.x + ww, wy, area.w - ww, bh), "SIGN OUT", 'X', false, !st.web_client, -1);

    tui_rect box = tui_rect_make(area.x, wy + bh, area.w, 6);
    ls_panel_box(sf, box, "NETWORK TERMINAL", st.net_on ? TUI_GREEN : TUI_CYAN);
    if (!st.net_on) {
        line(sf, box, 1, "OFF", LS_ATTR_DIM);
        line(sf, box, 2, LS_TEXT(t_scr_wireless_turn_it_on_to, "Turn it on to use the board from a phone"), WHITE);
    } else if (!s_view.wifi_connected) {
        line(sf, box, 1, LS_TEXT(t_scr_wireless_on_waiting_for_wi, "ON  waiting for Wi-Fi (WI-FI tab)"), INK(TUI_YELLOW | TUI_BRIGHT));
    } else {
        snprintf(text, sizeof(text), "ON   %s   port %d", s_view.ip, st.port);
        line(sf, box, 1, text, INK(TUI_GREEN | TUI_BRIGHT));
    }
    if (st.pass[0]) {
        if (s_show_pass) snprintf(text, sizeof(text), "PASSWORD  %s", st.pass);
        else {
            int n = snprintf(text, sizeof(text), "PASSWORD  ");
            for (size_t i = 0; i < strlen(st.pass) && n < (int)sizeof(text) - 1; i++) text[n++] = '*';
            text[n] = 0;
        }
        line(sf, box, st.net_on ? 2 : 3, text, s_show_pass ? INK(TUI_YELLOW | TUI_BRIGHT) : WHITE);
    }
    if (st.net_on) {
        if (st.client)
            snprintf(text, sizeof(text), "%u.%u.%u.%u %s", ip[0], ip[1], ip[2], ip[3],
                     st.authed ? "is watching" : "is at the password");
        else snprintf(text, sizeof(text), "Nobody connected");
        line(sf, box, 3, text, st.client ? CYAN : LS_ATTR_DIM);
    }
    line(sf, box, 4, LS_TEXT(t_scr_wireless_telnet_is_not_encrypted, "Telnet is not encrypted: your own network only"), LS_ATTR_DIM);

    const int by = box.y + box.h;
    static const char keys[] = {'T', 'H', 'N', 'P', 'K'};
    const char *labels[] = {st.net_on ? "TURN OFF" : "TURN ON", s_show_pass ? "HIDE" : "SHOW", "NEW", "SET", "KICK"};
    const int w = area.w / 5;
    for (int i = 0; i < 5; i++)
        button(sf, tui_rect_make(area.x + i * w, by, i == 4 ? area.w - 4 * w : w, bh), labels[i], keys[i],
               i == 0 && st.net_on, (i == 4 && !st.client && !st.ssh_client) || (i == 1 && !st.pass[0]), -1);

    tui_rect ssh = tui_rect_make(area.x, by + bh, area.w, 6);
    ls_panel_box(sf, ssh, "SSH", st.ssh_on ? TUI_GREEN : TUI_CYAN);
    if (!st.ssh_on) {
        line(sf, ssh, 1, LS_TEXT(t_scr_wireless_off_encrypted_a_saved, "OFF   encrypted; a saved key needs no password"), LS_ATTR_DIM);
    } else if (!s_view.wifi_connected || !st.ssh_listening) {
        LS_SNPRINTF(t_scr_wireless_on_port_d_waiting, text, sizeof(text), "ON  port %d, waiting for Wi-Fi", st.ssh_port);
        line(sf, ssh, 1, text, INK(TUI_YELLOW | TUI_BRIGHT));
    } else {
        snprintf(text, sizeof(text), "ON   %s   port %d", s_view.ip, st.ssh_port);
        line(sf, ssh, 1, text, INK(TUI_GREEN | TUI_BRIGHT));
    }
    const uint8_t *sip = (const uint8_t *)&st.ssh_ip;
    if (st.ssh_client)
        snprintf(text, sizeof(text), "%u.%u.%u.%u %s", sip[0], sip[1], sip[2], sip[3],
                 st.ssh_authed ? "is in" : "is logging in");
    else snprintf(text, sizeof(text), "Nobody connected");
    if (st.ssh_on) line(sf, ssh, 2, text, st.ssh_client ? CYAN : LS_ATTR_DIM);
    if (st.ssh_learn_s)
        LS_SNPRINTF(t_scr_wireless_keys_d_of_d, text, sizeof(text), "KEYS %d of %d   LEARNING %ds: log in now", st.ssh_keys, st.ssh_keys_max,
                 st.ssh_learn_s);
    else snprintf(text, sizeof(text), "KEYS %d of %d%s", st.ssh_keys, st.ssh_keys_max,
                  st.ssh_keys ? "" : "   LEARN, then log in once");
    line(sf, ssh, 3, text, st.ssh_learn_s ? INK(TUI_YELLOW | TUI_BRIGHT) : WHITE);
    line(sf, ssh, 4, st.ssh_fp[0] ? st.ssh_fp : "Host key: made when first turned on", LS_ATTR_DIM);

    const int sy = ssh.y + ssh.h;
    static const char skeys[] = {'S', 'R', 'L', 'F'};
    const char *slabels[] = {st.ssh_on ? "TURN OFF" : "TURN ON", "PORT", st.ssh_learn_s ? "STOP" : "LEARN", "FORGET"};
    const int sw = area.w / 4;
    for (int i = 0; i < 4; i++)
        button(sf, tui_rect_make(area.x + i * sw, sy, i == 3 ? area.w - 3 * sw : sw, bh), slabels[i], skeys[i],
               (i == 0 && st.ssh_on) || (i == 2 && st.ssh_learn_s), (i == 2 && !st.ssh_on) || (i == 3 && !st.ssh_keys), -1);
    tui_put_str(sf, area, area.x + 1, sy + bh, s_note, WHITE);

    if (sy + bh + 5 > area.y + area.h) return;
    tui_rect usb = tui_rect_make(area.x, sy + bh + 1, area.w, 4);
    if (usb.y + usb.h > area.y + area.h) return;
    ls_panel_box(sf, usb, "USB SCREEN", st.serial_on ? TUI_GREEN : TUI_CYAN);
    if (st.serial_on) {
        line(sf, usb, 1, st.serial_z ? "ON   the website is watching (compressed)" : "ON   a PC is watching (lsterm)",
             INK(TUI_GREEN | TUI_BRIGHT));
        line(sf, usb, 2, LS_TEXT(t_scr_wireless_ends_by_itself_when, "Ends by itself when the cable or page goes"), LS_ATTR_DIM);
    } else {
        line(sf, usb, 1, "Idle", LS_ATTR_DIM);
        line(sf, usb, 2, LS_TEXT(t_scr_wireless_terminalbay_com_connect_in, "terminalbay.com: CONNECT in the sidebar"), WHITE);
    }

    int help_h = area.y + area.h - usb.y - usb.h;
    if (help_h > 10) help_h = 10;
    tui_rect help = tui_rect_make(area.x, usb.y + usb.h, area.w, help_h);
    if (help.h < 4) return;
    /* The web screen's steps when it is on, Termius's when only telnet is. */
    static const char *const web_steps[] = {
        "Safari: http://" LS_RTERM_WEB_NAME ".local/",
        "The password once: the phone keeps it",
        "Share, Add to Home Screen: an app",
        "Tap the screen as you would the glass",
        "Swipe to scroll; keys on the bar",
        "The keyboard key raises the keyboard",
    };
    static const char *const telnet_steps[] = {
        "New Host: the address above",
        "SSH on, port as above (Telnet: 2323)",
        "Password in the host, or LEARN its key",
        "Charset UTF-8",
        "The bar on the last row moves the view",
        "Hide the keyboard with its own key",
    };
    const bool by_web = st.web_on || !st.net_on;
    const char *const *steps = by_web ? web_steps : telnet_steps;
    ls_panel_box(sf, help, by_web ? "PHONE SETUP / SAFARI" : "PHONE SETUP / TERMIUS", TUI_MAGENTA);
    for (int i = 0; i < 6 && i + 1 < help.h - 2; i++) {
        snprintf(text, sizeof(text), "%d %s", i + 1, steps[i]);
        line(sf, help, i + 1, text, i == (by_web ? 1 : 2) ? INK(TUI_YELLOW | TUI_BRIGHT) : WHITE);
    }
    if (help.h >= 9)
        line(sf, help, 7, by_web ? "PC: the same address in any browser" : "PC: ssh <address>, or telnet <address> 2323",
             LS_ATTR_DIM);
}

static void act(char ch)
{
    if (ch == 'g' || ch == 'G') { ls_tui_screen_show(ls_tui_screen_index_of(&ls_scr_drones)); return; }
    if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (ch == 'w' || ch == 'b' || ch == 'v' || ch == 'e') {
        s_tab = ch == 'e' ? 3 : ch == 'v' ? 2 : ch == 'b';
        s_hit_count = 0; s_note[0] = 0; s_show_pass = false;
        return;
    }
    if (s_tab == 3) { remote_act(ch); return; }
    if (ch == '[' || ch == ']') {
        int p = s_selected / s_page_size + (ch == ']' ? 1 : -1);
        int pages = (s_view.ap_count + s_page_size - 1) / s_page_size;
        if (pages > 0) s_selected = ((p + pages) % pages) * s_page_size;
        return;
    }
    if (s_view.busy) return;
    if (s_tab) {
        if (!s_view.bt_available) return;
        if (ch == 'o') ls_opt_open(&BT_CTX);
        if (ch == 's') request(LS_WIRELESS_BT_RESCAN, NULL, NULL);
        if (ch == 'd') request(LS_WIRELESS_BT_STOP, NULL, NULL);
        return;
    }
    if (!s_view.wifi_available) return;
    switch (ch) {
    case 's': request(LS_WIRELESS_SCAN, NULL, NULL); break;
    case 'r': request(LS_WIRELESS_SAVED, s_saved_selected >= 0 ? s_saved_name : NULL, NULL); break;
    case 'm': ls_keyboard_open_lower("NETWORK NAME", "", 32, ssid_done); break;
    case 'j': join_ap(s_selected); break;
    case 'd': request(LS_WIRELESS_LEAVE, NULL, NULL); break;
    case 'k':
        if (s_view.saved_count) {
            s_saved_selected = (s_saved_selected + 1) % s_view.saved_count;
            snprintf(s_saved_name, sizeof(s_saved_name), "%s", s_view.saved[s_saved_selected]);
        }
        break;
    case 'p': case 'n':
        if (s_view.saved_count) {
            int pages = (s_view.saved_count + s_saved_page_size - 1) / s_saved_page_size;
            int page = (s_saved_selected >= 0 ? s_saved_selected / s_saved_page_size : 0);
            page = (page + pages + (ch == 'n' ? 1 : -1)) % pages;
            s_saved_selected = page * s_saved_page_size;
            snprintf(s_saved_name, sizeof(s_saved_name), "%s", s_view.saved[s_saved_selected]);
        }
        break;
    case 'f':
        if (s_saved_selected >= 0) {
            char title[96];
            snprintf(s_forget_name, sizeof(s_forget_name), "%s", s_saved_name);
            LS_SNPRINTF(t_scr_wireless_forget_s_type_yes, title, sizeof(title), "FORGET %s / TYPE YES", s_forget_name);
            ls_keyboard_open(title, "", 3, forget_done);
        } else LS_SNPUTS(t_scr_wireless_select_a_saved_network, s_note, sizeof(s_note), "Select a saved network to forget");
        break;
    }
}

static void trace(tui_surface *sf, tui_rect a, bool bt, bool traffic)
{
    ls_panel_box(sf, a, traffic ? "CONTROL TRAFFIC / SEC" : "SIGNAL / dBm", TUI_CYAN);
    tui_rect g = tui_rect_make(a.x + 5, a.y + 2, a.w - 7, a.h - 4);
    if (g.w < 4 || g.h < 2) return;
    line(sf, a, 1, traffic ? "RX commands +  TX telemetry *" : "Fixed scale  -100 to -20", LS_ATTR_DIM);
    int top = traffic ? 10 : -20, bottom = traffic ? 0 : -100;
    const ls_wireless_history_t *h = &s_view.history;
    if (traffic) {
        for (unsigned i = 0; i < h->count; i++) {
            const ls_wireless_sample_t *p = &h->samples[i];
            if (!(p->valid & LS_WIRELESS_RATE_VALID)) continue;
            while (top < p->rx || top < p->tx) top *= 2;
        }
    }
    char text[20];
    snprintf(text, sizeof(text), "%d", top);
    tui_put_str(sf, a, a.x + 1, g.y, text, LS_ATTR_DIM);
    snprintf(text, sizeof(text), "%d", bottom);
    tui_put_str(sf, a, a.x + 1, g.y + g.h - 1, text, LS_ATTR_DIM);
    for (int x = 0; x < g.w; x += 2)
        tui_put_char(sf, g, g.x + x, g.y + g.h / 2, '.', INK(TUI_CYAN));
    bool any = false;
    unsigned oldest = (h->next + LS_WIRELESS_SAMPLES - h->count) % LS_WIRELESS_SAMPLES;
    for (unsigned i = 0; i < h->count; i++) {
        const ls_wireless_sample_t *p = &h->samples[(oldest + i) % LS_WIRELESS_SAMPLES];
        unsigned mask = traffic ? LS_WIRELESS_RATE_VALID : bt ? LS_WIRELESS_BT_VALID : LS_WIRELESS_WIFI_VALID;
        if (!(p->valid & mask)) continue;
        any = true;
        int x = g.x + (int)(LS_WIRELESS_SAMPLES - h->count + i) * (g.w - 1) / (LS_WIRELESS_SAMPLES - 1);
        for (int series = 0; series < (traffic ? 2 : 1); series++) {
            int value = traffic ? (series ? p->tx : p->rx) : bt ? p->bt : p->wifi;
            if (value < bottom) value = bottom;
            if (value > top) value = top;
            int y = g.y + (top - value) * (g.h - 1) / (top - bottom);
            tui_put_char(sf, g, x, y, series ? '*' : '+',
                INK(series ? TUI_YELLOW | TUI_BRIGHT : TUI_GREEN | TUI_BRIGHT));
        }
    }
    if (!any) ls_dither_label(sf, g, g.h / 2,
        traffic ? "Waiting for link traffic" : "Connect to measure signal", WHITE);
    line(sf, a, a.h - 2, "60s ago", LS_ATTR_DIM);
    tui_put_str(sf, a, a.x + a.w - 5, a.y + a.h - 2, "now", LS_ATTR_DIM);
}

static void saved_networks(tui_surface *sf, tui_rect a)
{
    char title[48]; LS_SNPRINTF(t_scr_wireless_saved_d_k_select, title, sizeof(title), "SAVED / %d / K SELECT", s_view.saved_count);
    ls_panel_box(sf, a, title, TUI_CYAN);
    int rows = a.h - 3;
    if (rows <= 0) return;
    s_saved_page_size = rows;
    int start = s_saved_selected >= 0 ? s_saved_selected / rows * rows : 0;
    if (!s_view.saved_count) line(sf, a, 1, "No saved networks", LS_ATTR_DIM);
    for (int i = start; i < s_view.saved_count && i < start + rows; ++i) {
        bool connected = s_view.wifi_connected && !strcmp(s_view.saved[i], s_view.ssid);
        char text[48]; snprintf(text, sizeof(text), "%c%c %.32s",
            i == s_saved_selected ? '>' : ' ', connected ? '*' : ' ', s_view.saved[i]);
        int y = a.y + 1 + i - start;
        line(sf, a, 1 + i - start, text, connected ? INK(TUI_GREEN | TUI_BRIGHT) : WHITE);
        if (s_hit_count < HIT_MAX) s_hits[s_hit_count++] = (hit_t){
            tui_rect_make(a.x + 1, y, a.w - 2, 1), 0, -2 - i};
    }
    if (s_view.saved_count > rows) {
        int y = a.y + a.h - 2, half = (a.w - 2) / 2;
        line(sf, a, a.h - 2, "< PREV saved", CYAN);
        tui_put_str(sf, a, a.x + 1 + half, y, "NEXT saved >", CYAN);
        if (s_hit_count + 2 <= HIT_MAX) {
            s_hits[s_hit_count++] = (hit_t){tui_rect_make(a.x + 1, y, half, 1), 'p', -1};
            s_hits[s_hit_count++] = (hit_t){tui_rect_make(a.x + 1 + half, y, a.w - 2 - half, 1), 'n', -1};
        }
    }
}

static void networks(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, LS_TEXT(t_scr_wireless_nearby_tap_to_join, "NEARBY / TAP TO JOIN"), TUI_CYAN);
    int row_h = ls_tui_is_wide() && ls_tui_keyboard_mode() ? 3 : 4;
    int nav_h = ls_tui_is_wide() && ls_tui_keyboard_mode() ? 3 : 4;
    s_page_size = (a.h - 3 - nav_h) / row_h;
    if (s_page_size < 1) s_page_size = 1;
    if (s_page_size > 5) s_page_size = 5;
    int start = s_selected / s_page_size * s_page_size;
    if (!s_view.ap_count) {
        line(sf, a, 3, (s_view.busy && s_view.operation == LS_WIRELESS_SCAN) ? "Scanning..." : "Press SCAN to find networks", WHITE);
    }
    for (int i = 0; i < s_page_size && start + i < s_view.ap_count; i++) {
        const ls_wireless_ap_t *ap = &s_view.aps[start + i];
        tui_rect r = tui_rect_make(a.x + 1, a.y + 2 + i * row_h, a.w - 2, row_h);
        if (r.y + r.h > a.y + a.h - nav_h - 1) break;
        bool selected = start + i == s_selected;
        if (selected) ls_fill_dither(sf, r, LS_DITHER_LIGHT, TUI_CYAN);
        char text[64];
        snprintf(text, sizeof(text), "%c %.32s", selected ? '>' : ' ', ap->ssid);
        line(sf, r, 0, text, WHITE);
        LS_SNPRINTF(t_scr_wireless_d_dbm_ch_d, text, sizeof(text), "  %d dBm  CH %d  %s%s", ap->rssi, ap->channel,
                 ap->secure ? "PASSWORD" : "OPEN", ap->saved ? " / SAVED" : "");
        line(sf, r, 1, text, INK(TUI_YELLOW | TUI_BRIGHT));
        if (s_hit_count < HIT_MAX) s_hits[s_hit_count++] = (hit_t){r, 'j', start + i};
    }
    int width = (a.w - 2) / 2;
    int y = a.y + a.h - nav_h - 1;
    button(sf, tui_rect_make(a.x + 1, y, width, nav_h), "PREV", '[', false,
           s_view.ap_count <= s_page_size, -1);
    button(sf, tui_rect_make(a.x + 1 + width, y, a.w - 2 - width, nav_h), "NEXT", ']', false,
           s_view.ap_count <= s_page_size, -1);
}

static void activity(tui_surface *sf, tui_rect a)
{
    ls_panel_box(sf, a, LS_TEXT(t_scr_wireless_flipper_control_head, "FLIPPER CONTROL HEAD"), TUI_CYAN);
    bool moving = s_view.bt_state > 0 && !s_view.bt_ready;
    const ls_wireless_history_t *h = &s_view.history;
    if (h->count) {
        const ls_wireless_sample_t *p = &h->samples[(h->next + LS_WIRELESS_SAMPLES - 1) % LS_WIRELESS_SAMPLES];
        moving |= s_view.bt_ready && (p->rx || p->tx) && s_view.now_ms - h->last_ms < 2000;
    }
    unsigned phase = moving ? s_view.now_ms / 180 % 4 : 0;
    int cx = a.x + a.w / 2, cy = a.y + a.h / 2;
    for (int n = 1; n <= 3; n++) {
        uint8_t attr = INK((unsigned)n == phase ? TUI_YELLOW | TUI_BRIGHT : TUI_CYAN);
        tui_put_char(sf, a, cx - 4 - n * 3, cy, '(', attr);
        tui_put_char(sf, a, cx + 4 + n * 3, cy, ')', attr);
    }
    tui_put_str(sf, a, cx - 3, cy, s_view.bt_ready ? "LINKED" : "  BT  ", WHITE);
    line(sf, a, a.h - 2, s_view.bt_ready ? "Live commands + radio telemetry" :
         s_view.stock_head_seen ? "Open LakeShark on the Flipper" : "SCAN searches for LakeShark heads", LS_ATTR_DIM);
}

static void draw(tui_surface *sf, tui_rect area)
{
    uint32_t old_scan = s_view.scan_revision;
    char selected[33] = "";
    if (s_selected < s_view.ap_count) snprintf(selected, sizeof(selected), "%s", s_view.aps[s_selected].ssid);
    ls_wireless_get(&s_view);
    s_saved_selected = -1;
    for (int i = 0; i < s_view.saved_count; ++i)
        if (!strcmp(s_saved_name, s_view.saved[i])) s_saved_selected = i;
    if (old_scan != s_view.scan_revision) {
        s_selected = 0;
        for (int i = 0; i < s_view.ap_count; i++)
            if (!strcmp(selected, s_view.aps[i].ssid)) { s_selected = i; break; }
    }
    if (s_selected >= s_view.ap_count) s_selected = 0;
    s_hit_count = 0;
    if (area.w < 32 || area.h < 22) {
        ls_panel_notice(sf, area, "LINK", LS_TEXT(t_scr_wireless_more_screen_space_needed, "More screen space needed"), LS_TEXT(t_scr_wireless_use_a_smaller_display, "Use a smaller display font"));
        return;
    }
    bool wide = ls_tui_is_wide();
    int tab_h = wide ? 3 : 4, action_h = wide ? 3 : (s_tab ? 5 : 8), msg_h = wide ? 1 : 2;
    int tab_w = area.w / 5;
    button(sf, tui_rect_make(area.x, area.y, tab_w, tab_h), "WI-FI", 'W', s_tab == 0, false, -1);
    button(sf, tui_rect_make(area.x + tab_w, area.y, tab_w, tab_h),
           tab_w >= 12 ? "BLUETOOTH" : "BT", 'B', s_tab == 1, false, -1);
    button(sf, tui_rect_make(area.x + tab_w * 2, area.y, tab_w, tab_h),
           "SURVEY", 'V', s_tab == 2, false, -1);
    button(sf, tui_rect_make(area.x + tab_w * 3, area.y, tab_w, tab_h),
           "REMOTE", 'E', s_tab == 3, false, -1);
    button(sf, tui_rect_make(area.x + tab_w * 4, area.y, area.w - tab_w * 4, tab_h),
           "DRONES", 'G', false, false, -1);
    if (s_tab == 3) {
        remote(sf, tui_rect_make(area.x, area.y + tab_h, area.w, area.h - tab_h), wide);
        return;
    }
    if (s_tab == 2) {
        ls_survey_draw(sf, tui_rect_make(area.x, area.y + tab_h, area.w, area.h - tab_h));
        return;
    }
    tui_rect status = tui_rect_make(area.x, area.y + tab_h, area.w, wide ? 5 : 6);
    ls_panel_box(sf, status, s_tab ? "BLUETOOTH STATUS" : "WI-FI STATUS", TUI_CYAN);
    char text[128];
    if (!s_tab) {
        const char *state = s_view.wifi_available ? s_view.wifi_status : "Wi-Fi unavailable in this build";
        int row = 1, width = status.w - 2;
        while (*state && row < status.h - 2) {
            int n = (int)strlen(state);
            if (n > width) {
                n = width;
                while (n > 0 && state[n] != ' ') n--;
                if (!n) n = width;
            }
            snprintf(text, sizeof(text), "%.*s", n, state);
            line(sf, status, row++, text, WHITE);
            state += n;
            while (*state == ' ') state++;
        }
        if (s_view.wifi_connected) LS_SNPRINTF(t_scr_wireless_ip_s_d_dbm, text, sizeof(text), "IP %s  %d dBm  CH %d", s_view.ip, s_view.wifi_rssi, s_view.channel);
        else LS_SNPUTS(t_scr_wireless_station_mode_saved_reconnect, text, sizeof(text), "Station mode / saved reconnect");
    } else {
        snprintf(text, sizeof(text), "%s  %s", s_view.bt_available ? s_view.bt_status : "Bluetooth unavailable", s_view.bt_state ? s_view.peer : "");
        line(sf, status, 1, text, WHITE);
        if (s_view.bt_signal) snprintf(text, sizeof(text), "%s  %d dBm", s_view.address, s_view.bt_rssi);
        else LS_SNPUTS(t_scr_wireless_lakeshark_app_on_flipper, text, sizeof(text), "LakeShark app on Flipper / BLE");
    }
    line(sf, status, s_tab ? 2 : status.h - 2, text, INK(TUI_YELLOW | TUI_BRIGHT));
    if (s_tab) LS_SNPRINTF(t_scr_wireless_rx_lu_commands_tx, text, sizeof(text), "RX %lu commands  TX %lu frames  drop %lu",
        (unsigned long)s_view.rx, (unsigned long)s_view.tx, (unsigned long)s_view.drops);
    else LS_SNPUTS(t_scr_wireless_tap_a_network_or, text, sizeof(text), "Tap a network or use MANUAL for a hidden SSID");
    if (!wide && s_tab) line(sf, status, 4, text, LS_ATTR_DIM);
    tui_rect content = tui_rect_make(area.x, status.y + status.h, area.w,
        area.h - tab_h - status.h - action_h - msg_h);
    tui_rect first, second;
    ls_tui_split(content, &first, &second);
    if (s_tab) {
        if (wide) activity(sf, first);
        else {
            int h = first.h > 14 ? 7 : 5;
            activity(sf, tui_rect_make(first.x, first.y, first.w, h));
            trace(sf, tui_rect_make(first.x, first.y + h, first.w, first.h - h), true, false);
        }
        if (wide) {
            int h = second.h / 2;
            trace(sf, tui_rect_make(second.x, second.y, second.w, h), true, false);
            trace(sf, tui_rect_make(second.x, second.y + h, second.w, second.h - h), true, true);
        } else trace(sf, second, true, true);
    } else {
        networks(sf, first);
        int h = second.h / 2;
        trace(sf, tui_rect_make(second.x, second.y, second.w, h), false, false);
        saved_networks(sf, tui_rect_make(second.x, second.y + h, second.w, second.h - h));
    }
    int msg_y = area.y + area.h - action_h - msg_h;
    bool bt_message = s_view.operation >= LS_WIRELESS_BT_START;
    const char *message = s_note[0] ? s_note : bt_message == (s_tab == 1) ? s_view.message : "";
    if (s_view.busy && bt_message == (s_tab == 1)) {
        snprintf(text, sizeof(text), "%c %s", "|/-\\"[(s_view.now_ms / 180) % 4],
            s_view.operation == LS_WIRELESS_SCAN ? "Scanning nearby networks..." : "Updating connection...");
        message = text;
    }
    tui_put_str(sf, area, area.x + 1, msg_y, message, WHITE);
    static const char *labels[] = {"SCAN", "AUTO", "MANUAL", "JOIN", "LEAVE", "FORGET"};
    static const char keys[] = {'S', 'R', 'M', 'J', 'D', 'F'};
    int count = s_tab ? 3 : 6, columns = wide ? count : 3;
    int rows = (count + columns - 1) / columns, h = action_h / rows;
    if (h > 5) h = 5;
    int w = area.w / columns;
    for (int i = 0; i < count; i++) {
        bool dim = s_view.busy || (s_tab ? !s_view.bt_available : !s_view.wifi_available);
        if (!s_tab && i == 3 && !s_view.ap_count) dim = true;
        if (!s_tab && i == 5 && s_saved_selected < 0) dim = true;
        button(sf, tui_rect_make(area.x + i % columns * w, msg_y + msg_h + i / columns * h,
                i % columns == columns - 1 ? area.w - (columns - 1) * w : w, h),
               s_tab ? (i == 2 ? "OPTIONS" : i ? "STOP" : "SCAN") : i == 1 && s_saved_selected >= 0 ? "CONNECT" : labels[i], s_tab ? (i == 2 ? 'O' : i ? 'D' : 'S') : keys[i],
               false, dim, -1);
    }
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_TAB) { act(s_tab ? 'w' : 'b'); return true; }
    if (k == LS_TK_CHAR && ch && strchr("gGvVwWbBeE", ch)) { act(ch); return true; }
    if (s_tab == 3) {
        if (k == LS_TK_CHAR && ch && strchr("tThHnNpPkKsSrRlLfFoOxX", ch)) { act(ch); return true; }
        return false;
    }
    if (s_tab == 2) return ls_survey_key(k, ch);
    if (!s_tab && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        if (s_view.ap_count) s_selected = (s_selected + s_view.ap_count + (k == LS_TK_UP ? -1 : 1)) % s_view.ap_count;
        return true;
    }
    if (!s_tab && k == LS_TK_ENTER) { join_ap(s_selected); return true; }
    if (k == LS_TK_CHAR && strchr("wWbBsSrRmMjJdDfFkKpPnNoO[]", ch) && ch) { act(ch); return true; }
    return false;
}

static bool touch(int x, int y)
{
    for (int i = 0; i < s_hit_count; i++) {
        hit_t hit = s_hits[i];
        if (x < hit.rect.x || x >= hit.rect.x + hit.rect.w || y < hit.rect.y || y >= hit.rect.y + hit.rect.h) continue;
        if (hit.ap <= -2) {
            s_saved_selected = -2 - hit.ap;
            snprintf(s_saved_name, sizeof(s_saved_name), "%s", s_view.saved[s_saved_selected]);
        } else if (hit.ap >= 0) join_ap(hit.ap);
        else if (hit.key) act(hit.key);
        return true;
    }
    if (s_tab == 2) return ls_survey_touch(x, y);
    return true;
}

static void enter(void) {
    s_show_pass = false;
    s_hit_count = 0; s_note[0] = 0; s_saved_name[0] = 0; s_saved_selected = -1;
    ls_wireless_set_active(true);
}
static void leave(void) { s_hit_count = 0; ls_wireless_set_active(false); }

const ls_tui_screen_t ls_scr_wireless = {
    .name = "LINK", .hint = "W Wi-Fi  B Bluetooth  V Survey  E Remote  G Drones",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
};
