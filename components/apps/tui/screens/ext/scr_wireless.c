/* LINK: Wi-Fi, the Bluetooth control head, the Wi-Fi survey and the ways
   into the board from a phone or a PC.

   One header says what is up before anything is tapped: the network, its
   address and level, the head, and which remote doors are open. The things
   done on the move - scan, join, leave; scan, stop - are the big keys at
   the bottom, under the thumb. Everything done once and then left alone
   (a hidden network, forgetting a saved one, passwords, ports, SSH keys)
   is in OPTIONS, where every app keeps that kind of thing. DRONES was a
   fifth tab that was not a tab at all but a jump to the DRONES app; it is
   on HOME, on G here, and a row in Wi-Fi's OPTIONS. */
#include "../../ls_rid_ui.h"
#include "../../ls_tui_screen.h"
#include "../../ls_tui_ui.h"
#include "../../ls_keyboard.h"
#include "../../ls_wireless.h"
#include "../../ls_survey_ui.h"
#include "../../ls_options.h"
#include "../../ls_motion.h"
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

enum { TAB_WIFI, TAB_BT, TAB_SURVEY, TAB_REMOTE };

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
/* The raised bar: which letter each key stands for, and the keyboard focus. */
static char s_bar_keys[6];
static int s_bar_n, s_focus = -1, s_slot;

static void act(char ch);

static tui_rect inset(tui_rect a)
{
    return tui_rect_make(a.x + 1, a.y + 1, a.w - 2, a.h - 2);
}

static void line(tui_surface *sf, tui_rect a, int row, const char *text, uint8_t attr)
{
    tui_put_str(sf, tui_rect_make(a.x + 1, a.y, a.w - 2, a.h), a.x + 1, a.y + row, text, attr);
}

/* A tab or a pager key: a field you may press, lit when it is the one. */
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

/* ---- Wi-Fi OPTIONS: done once, then left alone ---------------------------- */

static void wifi_opt(const ls_opt_t *o) { act((char)o->arg); }
static void wifi_saved_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o;
    snprintf(out, n, "%s", s_saved_selected >= 0 ? s_saved_name : s_view.saved_count ? "any saved" : "none saved");
}
static const char *wifi_why(const ls_opt_t *o)
{
    (void)o; ls_wireless_get(&s_view);
    if (!s_view.wifi_available) return "Wi-Fi unavailable";
    if (s_view.busy) return "Connection update running";
    return NULL;
}
static const char *wifi_forget_why(const ls_opt_t *o)
{
    const char *why = wifi_why(o);
    if (why) return why;
    return s_saved_selected >= 0 ? NULL : "Tap a saved network first";
}
static const ls_opt_t WIFI_OPTIONS[] = {
    {.label="CONNECT SAVED", .kind=LS_OPT_ACTION, .arg='r', .act=wifi_opt, .show=wifi_saved_show, .why_not=wifi_why},
    {.label="HIDDEN NETWORK", .kind=LS_OPT_ACTION, .arg='m', .act=wifi_opt, .leaves=true, .why_not=wifi_why},
    {.label="FORGET SAVED", .kind=LS_OPT_ACTION, .arg='f', .act=wifi_opt, .show=wifi_saved_show, .leaves=true, .why_not=wifi_forget_why},
    {.label="DRONE WATCH", .kind=LS_OPT_ACTION, .arg='g', .act=wifi_opt, .leaves=true},
};
static const ls_opt_ctx_t WIFI_CTX = {.name="WI-FI", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(WIFI_OPTIONS), .refresh_ms=250};

/* ---- Bluetooth OPTIONS ----------------------------------------------------- */

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

/* 'o' used to be the web screen's switch; it is OPTIONS everywhere else,
   so the web screen moved to 'm' (the phone) and 'o' opens the list. */
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
    case 'm':
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
    case 'r': {
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

/* The REMOTE list: every one of the small keys the page used to carry,
   each row calling the same letter it answered to. */
static void remote_opt(const ls_opt_t *o) { remote_act((char)o->arg); }
static void remote_pass_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o; ls_rterm_status_t st; ls_rterm_status(&st);
    if (!st.pass[0]) { snprintf(out, n, "none yet"); return; }
    if (s_show_pass) { snprintf(out, n, "%s", st.pass); return; }
    size_t i = 0;
    for (; i < strlen(st.pass) && i + 1 < n; i++) out[i] = '*';
    out[i] = 0;
}
static void remote_port_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o; ls_rterm_status_t st; ls_rterm_status(&st);
    snprintf(out, n, "%d", st.ssh_port);
}
static void remote_keys_show(const ls_opt_t *o, char *out, size_t n)
{
    (void)o; ls_rterm_status_t st; ls_rterm_status(&st);
    if (st.ssh_learn_s) snprintf(out, n, "LEARNING %ds", st.ssh_learn_s);
    else snprintf(out, n, "%d of %d", st.ssh_keys, st.ssh_keys_max);
}
static const char *remote_why(const ls_opt_t *o)
{
    ls_rterm_status_t st; ls_rterm_status(&st);
    switch (o->arg) {
    case 'h': return st.pass[0] ? NULL : "No password yet";
    case 'x': return st.web_client ? NULL : "Nobody watching";
    case 'k': return st.client || st.ssh_client ? NULL : "Nobody connected";
    case 'l': return st.ssh_on ? NULL : "Turn SSH on first";
    case 'f': return st.ssh_keys ? NULL : "No keys kept";
    }
    return NULL;
}
static const ls_opt_t REMOTE_OPTIONS[] = {
    {.label="PASSWORD", .kind=LS_OPT_ACTION, .arg='h', .act=remote_opt, .show=remote_pass_show, .why_not=remote_why},
    {.label="NEW PASSWORD", .kind=LS_OPT_ACTION, .arg='n', .act=remote_opt},
    {.label="SET PASSWORD", .kind=LS_OPT_ACTION, .arg='p', .act=remote_opt, .leaves=true},
    {.label="SIGN OUT BROWSERS", .kind=LS_OPT_ACTION, .arg='x', .act=remote_opt, .why_not=remote_why},
    {.label="KICK TERMINAL", .kind=LS_OPT_ACTION, .arg='k', .act=remote_opt, .why_not=remote_why},
    {.label="SSH PORT", .kind=LS_OPT_ACTION, .arg='r', .act=remote_opt, .show=remote_port_show, .leaves=true},
    {.label="LEARN SSH KEYS", .kind=LS_OPT_ACTION, .arg='l', .act=remote_opt, .show=remote_keys_show, .why_not=remote_why},
    {.label="FORGET SSH KEYS", .kind=LS_OPT_ACTION, .arg='f', .act=remote_opt, .leaves=true, .why_not=remote_why},
};
static const ls_opt_ctx_t REMOTE_CTX = {.name="REMOTE", .job=-1, .radio=LS_RSEL_NONE, LS_OPT_ROWS(REMOTE_OPTIONS), .refresh_ms=250};

static void remote_panels(tui_surface *sf, tui_rect area, const ls_rterm_status_t *st, bool wide)
{
    char text[96];
    const uint8_t *ip = (const uint8_t *)&st->client_ip;
    /* Wide: the web screen and the terminal on the left, SSH and USB on the
       right; there is no height for four panels in a column. */
    tui_rect left = area, right = area;
    if (wide) ls_tui_split(area, &left, &right);
    int y = left.y;

    /* The web screen first: the way in for a phone, no app and real touch. */
    tui_rect web = tui_rect_make(left.x, y, left.w, 6);
    if (web.y + web.h > left.y + left.h) return;
    ls_panel_box(sf, web, "WEB SCREEN", st->web_on ? TUI_GREEN : TUI_CYAN);
    if (!st->web_on) {
        line(sf, web, 1, "OFF", LS_ATTR_DIM);
        line(sf, web, 2, LS_TEXT(t_scr_wireless_the_screen_in_a, "The screen in a browser: real touch, no app"), WHITE);
    } else if (!s_view.wifi_connected) {
        line(sf, web, 1, LS_TEXT(t_scr_wireless_on_waiting_for_wi, "ON  waiting for Wi-Fi (WI-FI tab)"), INK(TUI_YELLOW | TUI_BRIGHT));
    } else {
        /* The name first: it is what to save to a home screen, and it
           outlives the address the router gives out today. */
        int row = 1;
        if (st->web_named) {
            line(sf, web, row++, "ON   http://" LS_RTERM_WEB_NAME ".local/", INK(TUI_GREEN | TUI_BRIGHT));
            snprintf(text, sizeof(text), "  or http://%s/", s_view.ip);
            line(sf, web, row++, text, WHITE);
        } else {
            snprintf(text, sizeof(text), "ON   http://%s/", s_view.ip);
            line(sf, web, row++, text, INK(TUI_GREEN | TUI_BRIGHT));
        }
    }
    if (st->web_on) {
        const uint8_t *wip = (const uint8_t *)&st->web_ip;
        if (st->web_client) LS_SNPRINTF(t_scr_wireless_u_u_u_u, text, sizeof(text), "%u.%u.%u.%u is watching", wip[0], wip[1], wip[2], wip[3]);
        else snprintf(text, sizeof(text), "Nobody watching");
        line(sf, web, 3, text, st->web_client ? CYAN : LS_ATTR_DIM);
    }
    line(sf, web, 4, LS_TEXT(t_scr_wireless_the_terminal_s_password, "The terminal's password, asked once"), LS_ATTR_DIM);
    y += web.h;

    tui_rect box = tui_rect_make(left.x, y, left.w, 6);
    if (box.y + box.h > left.y + left.h) return;
    ls_panel_box(sf, box, "NETWORK TERMINAL", st->net_on ? TUI_GREEN : TUI_CYAN);
    if (!st->net_on) {
        line(sf, box, 1, "OFF", LS_ATTR_DIM);
        line(sf, box, 2, LS_TEXT(t_scr_wireless_turn_it_on_to, "Turn it on to use the board from a phone"), WHITE);
    } else if (!s_view.wifi_connected) {
        line(sf, box, 1, LS_TEXT(t_scr_wireless_on_waiting_for_wi, "ON  waiting for Wi-Fi (WI-FI tab)"), INK(TUI_YELLOW | TUI_BRIGHT));
    } else {
        snprintf(text, sizeof(text), "ON   %s   port %d", s_view.ip, st->port);
        line(sf, box, 1, text, INK(TUI_GREEN | TUI_BRIGHT));
    }
    if (st->pass[0]) {
        if (s_show_pass) snprintf(text, sizeof(text), "PASSWORD  %s", st->pass);
        else {
            int n = snprintf(text, sizeof(text), "PASSWORD  ");
            for (size_t i = 0; i < strlen(st->pass) && n < (int)sizeof(text) - 1; i++) text[n++] = '*';
            text[n] = 0;
        }
        line(sf, box, st->net_on ? 2 : 3, text, s_show_pass ? INK(TUI_YELLOW | TUI_BRIGHT) : WHITE);
    }
    if (st->net_on) {
        if (st->client)
            snprintf(text, sizeof(text), "%u.%u.%u.%u %s", ip[0], ip[1], ip[2], ip[3],
                     st->authed ? "is watching" : "is at the password");
        else snprintf(text, sizeof(text), "Nobody connected");
        line(sf, box, 3, text, st->client ? CYAN : LS_ATTR_DIM);
    }
    line(sf, box, 4, LS_TEXT(t_scr_wireless_telnet_is_not_encrypted, "Telnet is not encrypted: your own network only"), LS_ATTR_DIM);
    y += box.h;

    if (wide) y = right.y;
    tui_rect ssh = tui_rect_make(right.x, y, right.w, 6);
    if (ssh.y + ssh.h > right.y + right.h) return;
    ls_panel_box(sf, ssh, "SSH", st->ssh_on ? TUI_GREEN : TUI_CYAN);
    if (!st->ssh_on) {
        line(sf, ssh, 1, LS_TEXT(t_scr_wireless_off_encrypted_a_saved, "OFF   encrypted; a saved key needs no password"), LS_ATTR_DIM);
    } else if (!s_view.wifi_connected || !st->ssh_listening) {
        LS_SNPRINTF(t_scr_wireless_on_port_d_waiting, text, sizeof(text), "ON  port %d, waiting for Wi-Fi", st->ssh_port);
        line(sf, ssh, 1, text, INK(TUI_YELLOW | TUI_BRIGHT));
    } else {
        snprintf(text, sizeof(text), "ON   %s   port %d", s_view.ip, st->ssh_port);
        line(sf, ssh, 1, text, INK(TUI_GREEN | TUI_BRIGHT));
    }
    const uint8_t *sip = (const uint8_t *)&st->ssh_ip;
    if (st->ssh_client)
        snprintf(text, sizeof(text), "%u.%u.%u.%u %s", sip[0], sip[1], sip[2], sip[3],
                 st->ssh_authed ? "is in" : "is logging in");
    else snprintf(text, sizeof(text), "Nobody connected");
    if (st->ssh_on) line(sf, ssh, 2, text, st->ssh_client ? CYAN : LS_ATTR_DIM);
    if (st->ssh_learn_s)
        LS_SNPRINTF(t_scr_wireless_keys_d_of_d, text, sizeof(text), "KEYS %d of %d   LEARNING %ds: log in now", st->ssh_keys, st->ssh_keys_max,
                 st->ssh_learn_s);
    else snprintf(text, sizeof(text), "KEYS %d of %d%s", st->ssh_keys, st->ssh_keys_max,
                  st->ssh_keys ? "" : "   OPTIONS: LEARN, then log in once");
    line(sf, ssh, 3, text, st->ssh_learn_s ? INK(TUI_YELLOW | TUI_BRIGHT) : WHITE);
    line(sf, ssh, 4, st->ssh_fp[0] ? st->ssh_fp : "Host key: made when first turned on", LS_ATTR_DIM);
    y += ssh.h;

    tui_rect usb = tui_rect_make(right.x, y, right.w, 4);
    if (usb.y + usb.h > right.y + right.h) return;
    ls_panel_box(sf, usb, "USB SCREEN", st->serial_on ? TUI_GREEN : TUI_CYAN);
    if (st->serial_on) {
        line(sf, usb, 1, st->serial_z ? "ON   the website is watching (compressed)" : "ON   a PC is watching (lsterm)",
             INK(TUI_GREEN | TUI_BRIGHT));
        line(sf, usb, 2, LS_TEXT(t_scr_wireless_ends_by_itself_when, "Ends by itself when the cable or page goes"), LS_ATTR_DIM);
    } else {
        line(sf, usb, 1, "Idle", LS_ATTR_DIM);
        line(sf, usb, 2, LS_TEXT(t_scr_wireless_terminalbay_com_connect_in, "terminalbay.com: CONNECT in the sidebar"), WHITE);
    }
    y += usb.h;

    int help_h = right.y + right.h - y;
    if (help_h > 10) help_h = 10;
    tui_rect help = tui_rect_make(right.x, y, right.w, help_h);
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
    const bool by_web = st->web_on || !st->net_on;
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
        s_tab = ch == 'e' ? TAB_REMOTE : ch == 'v' ? TAB_SURVEY : ch == 'b';
        s_hit_count = 0; s_note[0] = 0; s_show_pass = false; s_focus = -1;
        return;
    }
    if (s_tab == TAB_REMOTE) {
        if (ch == LS_OPT_KEY) ls_opt_open(&REMOTE_CTX);
        else remote_act(ch);
        return;
    }
    if (ch == '[' || ch == ']') {
        int p = s_selected / s_page_size + (ch == ']' ? 1 : -1);
        int pages = (s_view.ap_count + s_page_size - 1) / s_page_size;
        if (pages > 0) s_selected = ((p + pages) % pages) * s_page_size;
        return;
    }
    /* OPTIONS opens while a scan runs: its rows say why they will wait. */
    if (ch == LS_OPT_KEY) { ls_opt_open(s_tab == TAB_BT ? &BT_CTX : &WIFI_CTX); return; }
    if (s_view.busy) return;
    if (s_tab == TAB_BT) {
        if (!s_view.bt_available) return;
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

/* ---- drawing ---------------------------------------------------------------- */

/* The emblem: a board under its fan of arcs. An arc is lit for every bar
   of the network it is on, so the picture is the level; while a scan or a
   join runs the arcs step outward instead, because a scan is the one
   time the board is the thing moving. Nothing else here moves. */
static void art(tui_surface *sf, tui_rect a, int x, int y, bool wide, int lit, bool busy, bool linked)
{
    static const char *const ARC[3] = { "    .-~-.    ", "   .-~~~-.   ", "  .-~~~~~-.  " };
    const uint8_t on = INK(TUI_CYAN | TUI_BRIGHT), off = LS_ATTR_FAINT;
    const uint8_t dot = linked ? INK(TUI_GREEN | TUI_BRIGHT) : busy ? INK(TUI_YELLOW | TUI_BRIGHT) : off;
    if (wide) {   /* landscape: two arcs over the board, three rows */
        tui_put_str(sf, a, x, y, ARC[1] + 2, lit >= 2 ? on : off);
        tui_put_str(sf, a, x, y + 1, ARC[0] + 2, lit >= 1 ? on : off);
        tui_put_str(sf, a, x + 3, y + 2, "[", INK(TUI_WHITE));
        tui_put_char(sf, a, x + 4, y + 2, 'o', dot);
        tui_put_str(sf, a, x + 5, y + 2, "]", INK(TUI_WHITE));
        return;
    }
    for (int i = 0; i < 3; i++) tui_put_str(sf, a, x, y + i, ARC[2 - i], lit >= 3 - i ? on : off);
    tui_put_char(sf, a, x + 6, y + 3, 'o', dot);
    tui_put_str(sf, a, x + 4, y + 4, "[=|=]", INK(TUI_WHITE));
}

static int bars(void)
{
    if (!s_view.wifi_connected) return 0;
    return s_view.wifi_rssi >= -55 ? 3 : s_view.wifi_rssi >= -70 ? 2 : 1;
}

/* What is up, before anything is tapped. Returns the rows it took. */
static int header(tui_surface *sf, tui_rect a, const ls_rterm_status_t *st, bool wide)
{
    char s[128];
    const bool bt_busy = s_view.busy && s_view.operation >= LS_WIRELESS_BT_START;
    const bool wifi_busy = s_view.busy && !bt_busy;
    const int lit = wifi_busy ? ls_motion_phase(4, 1200) : bars();
    /* The portrait emblem's own strings carry their margin; the text column
       starts one clear of the widest arc so a refusal gets 43 columns. */
    art(sf, a, wide ? a.x + 1 : a.x, a.y, wide, lit, wifi_busy, s_view.wifi_connected);
    const int tx = a.x + (wide ? 11 : 12), tw = a.w - (tx - a.x) - 1;
    const tui_rect clip = tui_rect_make(tx, a.y, tw > 0 ? tw : 0, wide ? 4 : 6);

    tui_put_str(sf, clip, tx, a.y, "LINK", WHITE);
    int row = 1;
    if (s_view.wifi_connected) {
        snprintf(s, sizeof(s), "%.32s", s_view.ssid);
        tui_put_str(sf, clip, tx + 6, a.y, s, INK(TUI_GREEN | TUI_BRIGHT));
        snprintf(s, sizeof(s), "IP %s  %d dBm  CH %d", s_view.ip, s_view.wifi_rssi, s_view.channel);
        tui_put_str(sf, clip, tx, a.y + row++, s, WHITE);
    } else {
        tui_put_str(sf, clip, tx + 6, a.y, s_view.wifi_available ? "NO WI-FI" : "WI-FI UNAVAILABLE",
                    s_view.wifi_available ? INK(TUI_YELLOW | TUI_BRIGHT) : LS_ATTR_DIM);
        /* A refusal says why and what to do; it wraps rather than being
           cut, over up to four rows in portrait, ahead of the BT and
           REMOTE lines - while it shows, it is the thing to read. */
        const char *state = s_view.wifi_available ? s_view.wifi_status : "not in this build";
        const int limit = wide ? 1 : 4;
        for (int k = 0; *state && k < limit && tw > 0; k++) {
            int n = (int)strlen(state);
            if (n > tw) {
                n = tw;
                while (n > 0 && state[n] != ' ') n--;
                if (!n) n = tw;
            }
            snprintf(s, sizeof(s), "%.*s", n, state);
            tui_put_str(sf, clip, tx, a.y + row++, s, LS_ATTR_DIM);
            state += n;
            while (*state == ' ') state++;
        }
    }
    /* Wide has three rows beside the art; a message takes the third. */
    const char *message = s_note[0] ? s_note : (bt_busy == (s_tab == TAB_BT)) ? s_view.message : "";
    if (s_view.busy && bt_busy == (s_tab == TAB_BT)) {
        snprintf(s, sizeof(s), "%c %s", "|/-\\"[(s_view.now_ms / 180) % 4],
                 s_view.operation == LS_WIRELESS_SCAN ? "Scanning nearby networks..." : "Updating connection...");
        message = s;
    }
    if ((!wide && row < 5) || (wide && !message[0])) {
        char t[96];
        if (s_view.bt_ready) snprintf(t, sizeof(t), "BT  LINKED  %.24s", s_view.peer);
        else snprintf(t, sizeof(t), "BT  %.40s", s_view.bt_available ? s_view.bt_status : "unavailable");
        tui_put_str(sf, clip, tx, a.y + row++, t, s_view.bt_ready ? INK(TUI_GREEN | TUI_BRIGHT) : LS_ATTR_DIM);
    }
    if (!wide && row < 5) {
        const int doors = st->web_on + st->net_on + st->ssh_on;
        const int watching = (st->web_client ? 1 : 0) + (st->client ? 1 : 0) + (st->ssh_client ? 1 : 0);
        if (!doors) snprintf(s, sizeof(s), "REMOTE  off");
        else snprintf(s, sizeof(s), "REMOTE  %s%s%s/ %s", st->web_on ? "WEB " : "", st->net_on ? "TERM " : "",
                      st->ssh_on ? "SSH " : "", watching ? "watched" : "nobody in");
        tui_put_str(sf, clip, tx, a.y + row++, s, doors ? (watching ? CYAN : INK(TUI_GREEN)) : LS_ATTR_DIM);
        /* The message gets the row under the art, the whole width. */
        if (message[0]) tui_put_str(sf, a, a.x + 1, a.y + 5, message, s_view.busy ? INK(TUI_YELLOW | TUI_BRIGHT) : WHITE);
        return 6;
    }
    if (message[0]) tui_put_str(sf, clip, tx, a.y + row, message, s_view.busy ? INK(TUI_YELLOW | TUI_BRIGHT) : WHITE);
    return 4;
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

/* A tap picks a saved network; a second tap on the picked one connects. */
static void saved_networks(tui_surface *sf, tui_rect a)
{
    char title[48]; snprintf(title, sizeof(title), "SAVED / %d / TAP TWICE TO CONNECT", s_view.saved_count);
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
        if (i == s_saved_selected) ls_fill_dither(sf, tui_rect_make(a.x + 1, y, a.w - 2, 1), LS_DITHER_LIGHT, TUI_CYAN);
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

static void bt_page(tui_surface *sf, tui_rect content, bool wide)
{
    tui_rect first, second;
    ls_tui_split(content, &first, &second);
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
}

static void wifi_page(tui_surface *sf, tui_rect content)
{
    tui_rect first, second;
    ls_tui_split(content, &first, &second);
    networks(sf, first);
    int h = second.h / 2;
    trace(sf, tui_rect_make(second.x, second.y, second.w, h), false, false);
    saved_networks(sf, tui_rect_make(second.x, second.y + h, second.w, second.h - h));
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
    s_hit_count = 0; s_bar_n = 0;
    if (area.w < 32 || area.h < 22) {
        ls_panel_notice(sf, area, "LINK", LS_TEXT(t_scr_wireless_more_screen_space_needed, "More screen space needed"), LS_TEXT(t_scr_wireless_use_a_smaller_display, "Use a smaller display font"));
        return;
    }
    const bool wide = ls_tui_is_wide();
    ls_rterm_status_t st;
    ls_rterm_status(&st);
    int y = area.y;
    /* The survey is a dashboard of its own and wants the rows. */
    if (s_tab != TAB_SURVEY) y += header(sf, area, &st, wide);

    const int tab_h = wide ? 3 : 4, tab_w = area.w / 4;
    button(sf, tui_rect_make(area.x, y, tab_w, tab_h), "WI-FI", 'W', s_tab == TAB_WIFI, false, -1);
    button(sf, tui_rect_make(area.x + tab_w, y, tab_w, tab_h),
           tab_w >= 13 ? "BLUETOOTH" : "BT", 'B', s_tab == TAB_BT, false, -1);
    button(sf, tui_rect_make(area.x + tab_w * 2, y, tab_w, tab_h), "SURVEY", 'V', s_tab == TAB_SURVEY, false, -1);
    button(sf, tui_rect_make(area.x + tab_w * 3, y, area.w - tab_w * 3, tab_h), "REMOTE", 'E', s_tab == TAB_REMOTE, false, -1);
    y += tab_h;
    tui_rect body = tui_rect_make(area.x, y, area.w, area.y + area.h - y);
    if (s_tab == TAB_SURVEY) { ls_survey_draw(sf, body); return; }

    /* The big keys, under the thumb: what is done on the move. */
    ls_btn_t bar[4];
    int n = 0;
    char selected_ap[12] = "", found[12] = "";
    if (s_tab == TAB_REMOTE) {
        bar[n++] = (ls_btn_t){"WEB", st.web_on ? "ON" : "OFF", 'm', st.web_on, false};
        bar[n++] = (ls_btn_t){"TERMINAL", st.net_on ? "ON" : "OFF", 't', st.net_on, false};
        bar[n++] = (ls_btn_t){"SSH", st.ssh_on ? "ON" : "OFF", 's', st.ssh_on, false};
        bar[n++] = ls_opt_button(&REMOTE_CTX);
    } else if (s_tab == TAB_BT) {
        const bool dim = s_view.busy || !s_view.bt_available;
        bar[n++] = (ls_btn_t){"SCAN", "HEADS", 's', false, dim};
        bar[n++] = (ls_btn_t){"STOP", NULL, 'd', false, dim};
        bar[n++] = ls_opt_button(&BT_CTX);
    } else {
        const bool dim = s_view.busy || !s_view.wifi_available;
        if (s_view.ap_count) snprintf(found, sizeof(found), "%d FOUND", s_view.ap_count);
        if (s_selected < s_view.ap_count) snprintf(selected_ap, sizeof(selected_ap), "%.11s", s_view.aps[s_selected].ssid);
        bar[n++] = (ls_btn_t){"SCAN", found[0] ? found : NULL, 's', false, dim};
        bar[n++] = (ls_btn_t){"JOIN", selected_ap[0] ? selected_ap : NULL, 'j', false, dim || !s_view.ap_count};
        /* LEAVE stays live while disconnected: it is how a join that is
           taking too long is called off. */
        bar[n++] = (ls_btn_t){"LEAVE", s_view.wifi_connected ? NULL : "OFF", 'd', false, dim};
        bar[n++] = ls_opt_button(&WIFI_CTX);
    }
    for (int i = 0; i < n; i++) s_bar_keys[i] = bar[i].key;
    s_bar_n = n;
    const int bar_h = ls_btn_raised_height(body, n);
    tui_rect content = tui_rect_make(body.x, body.y, body.w, body.h - bar_h - 1);
    if (s_tab == TAB_REMOTE) remote_panels(sf, content, &st, wide);
    else if (s_tab == TAB_BT) bt_page(sf, content, wide);
    else wifi_page(sf, content);
    ls_btn_bar_raised(sf, tui_rect_make(body.x, body.y + body.h - bar_h, body.w, bar_h), bar, n, s_focus);
}

static bool key(ls_tk_t k, char ch)
{
    if (k == LS_TK_TAB) {
        static const char NEXT[] = { 'b', 'v', 'e', 'w' };   /* the tabs, left to right */
        act(NEXT[s_tab == TAB_WIFI ? 0 : s_tab == TAB_BT ? 1 : s_tab == TAB_SURVEY ? 2 : 3]);
        return true;
    }
    if (k == LS_TK_CHAR && ch && strchr("gGvVwWbBeE", ch)) { act(ch); return true; }
    if (s_tab == TAB_SURVEY) return ls_survey_key(k, ch);
    if (k == LS_TK_LEFT || k == LS_TK_RIGHT) return ls_btn_navigate(k, &s_slot, &s_focus, false);
    if (k == LS_TK_ENTER && s_focus >= 0 && s_focus < s_bar_n) {
        if (ls_btn_enabled(0, s_focus)) act(s_bar_keys[s_focus]);
        return true;
    }
    if (s_tab == TAB_REMOTE) {
        if (k == LS_TK_CHAR && ch && strchr("tThHnNpPkKsSrRlLfFoOxXmM", ch)) { act(ch); return true; }
        return false;
    }
    if (s_tab == TAB_WIFI && (k == LS_TK_UP || k == LS_TK_DOWN)) {
        s_focus = -1;
        if (s_view.ap_count) s_selected = (s_selected + s_view.ap_count + (k == LS_TK_UP ? -1 : 1)) % s_view.ap_count;
        return true;
    }
    if (s_tab == TAB_WIFI && k == LS_TK_ENTER) { join_ap(s_selected); return true; }
    if (k == LS_TK_CHAR && ch && strchr("sSrRmMjJdDfFkKpPnNoO[]", ch)) { act(ch); return true; }
    return false;
}

static bool touch(int x, int y)
{
    const int b = ls_btn_hit(x, y);
    if (b >= 0 && b < s_bar_n) { act(s_bar_keys[b]); return true; }
    for (int i = 0; i < s_hit_count; i++) {
        hit_t hit = s_hits[i];
        if (x < hit.rect.x || x >= hit.rect.x + hit.rect.w || y < hit.rect.y || y >= hit.rect.y + hit.rect.h) continue;
        if (hit.ap <= -2) {
            const int pick = -2 - hit.ap;
            if (pick == s_saved_selected) act('r');
            else {
                s_saved_selected = pick;
                snprintf(s_saved_name, sizeof(s_saved_name), "%s", s_view.saved[s_saved_selected]);
            }
        } else if (hit.ap >= 0) join_ap(hit.ap);
        else if (hit.key) act(hit.key);
        return true;
    }
    if (s_tab == TAB_SURVEY) return ls_survey_touch(x, y);
    return true;
}

static void enter(void) {
    s_show_pass = false;
    s_hit_count = 0; s_bar_n = 0; s_focus = -1; s_slot = 0;
    s_note[0] = 0; s_saved_name[0] = 0; s_saved_selected = -1;
    ls_wireless_set_active(true);
}
static void leave(void) { s_hit_count = 0; ls_wireless_set_active(false); }

const ls_tui_screen_t ls_scr_wireless = {
    .name = "LINK", .hint = "W Wi-Fi  B Bluetooth  V Survey  E Remote  O options  G Drones",
    .enter = enter, .leave = leave, .draw = draw, .key = key, .touch = touch,
};
