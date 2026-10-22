/* RADIOS: what is transmitting, receiving, or drawing current, and a way to stop it. */

#include "../../ls_tui_screen.h"
#include "board/ls_board_hw.h"
#include "core/settings.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"

#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"
#include "../../ls_i18n.h"
#include "../../ls_quick.h"
#include "../../ls_options.h"
#include "../../ls_app.h"
#include "../../ls_wireless.h"

/* ls_board.h and not ls_caps.h: the caps header has an #error in it saying
   exactly that, which is the right way for a header to be private. */
#include "ls_board.h"
#include "ls_gps.h"
#ifdef LS_BOARD_MIX_CC_CS
#include "ls_mixrf.h"
#include "ls_nfc_suite.h"
#endif

/* Presence and activity come from the endpoint that owns each USB radio. */
#include "radio_endpoint.h"

#if LS_HAS_LORA
#include "ls_mesh.h"
#endif

#define A(fg, bg) TUI_ATTR((fg), (bg))

#define DIM_FG LS_DIM_FG

typedef enum {
    RS_ABSENT = 0,   /* the part is not on this board, or not driven yet */
    RS_OFF,
    RS_ON,
    RS_BUSY,         /* on and actually moving data                     */
    RS_STARTING,
    RS_FAILED,
} radio_state_t;

typedef struct {
    const char *name;
    const char *what;                 /* one line: what it costs you     */
    radio_state_t (*read)(void);
    void (*set)(bool on);             /* NULL when it cannot be switched */

    const char *(*state_text)(void);  /* NULL: the on/off words          */
    const char *(*action_text)(void); /* NULL: TURN ON / TURN OFF        */
    bool        no_power;             /* selects a path, draws nothing   */
    bool        at_boot;              /* a preference for the next boot,
                                         not a load running now          */
} radio_row_t;

/* ------------------------------------------------------------------ rows -- */

static const char *s_error;
static char s_error_text[80];
static tui_rect s_options_hit;

static int mmcx_use_get(const ls_opt_t *o)
{ (void)o; return ls_board_hw_antenna_mmcx_tx(); }
static void mmcx_use_set(const ls_opt_t *o, int v)
{ (void)o; ls_board_hw_antenna_set_mmcx_tx(v != 0); settings_set_antenna_mmcx_tx(v != 0); }
static const char *const MMCX_USE_NAMES[] = { "RX ONLY", "RX+TX" };
static void options_back(const ls_opt_t *o)
{ (void)o; ls_opt_close(); }
#if LS_HAS_C6
static int ble_boot_get(const ls_opt_t *o) { (void)o; return settings_get_ble_at_boot(); }
static void ble_boot_set(const ls_opt_t *o, int v) { (void)o; settings_set_ble_at_boot(v != 0); }
static int wifi_boot_get(const ls_opt_t *o) { (void)o; return settings_get_wifi_at_boot(); }
static void wifi_boot_set(const ls_opt_t *o, int v) { (void)o; settings_set_wifi_at_boot(v != 0); }
#endif
#if LS_HAS_C6
static const char *const WARNING_NAMES[] = { "OFF", "ON" };
#endif
static const ls_opt_t RADIO_OPTIONS[] = {
    { .label = "MMCX1 USE", .kind = LS_OPT_TOGGLE,
      .names = MMCX_USE_NAMES, .get = mmcx_use_get, .set = mmcx_use_set },
#if LS_HAS_C6
    { .label = "BLE AT BOOT", .kind = LS_OPT_TOGGLE,
      .names = WARNING_NAMES, .get = ble_boot_get, .set = ble_boot_set },
    { .label = "WI-FI AT BOOT", .kind = LS_OPT_TOGGLE,
      .names = WARNING_NAMES, .get = wifi_boot_get, .set = wifi_boot_set },
#endif
    { .label = "BACK", .kind = LS_OPT_ACTION, .act = options_back, .leaves = true },
};
static const ls_opt_ctx_t RADIO_CONTEXT = {
    .name = "RADIOS", .job = -1, .radio = LS_RSEL_NONE,
    LS_OPT_ROWS(RADIO_OPTIONS),
};

static void options_open(void)
{
    ls_opt_open(&RADIO_CONTEXT);
}

/* A parked endpoint does not establish that its hardware is drawing power. */
static radio_state_t sdr_endpoint_read(const char *id)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present)
        return RS_ABSENT;
    if (info.paused) return RS_OFF;
    /* Success clears current faults; ordinary stream events are not faults. */
    if (info.last_error != LS_RADIO_OK && info.last_error != LS_RADIO_ERR_STOPPED &&
        info.last_error != LS_RADIO_ERR_TIMEOUT && info.last_error != LS_RADIO_ERR_BUSY)
        return RS_FAILED;
    if (info.streaming) return RS_BUSY;
    return info.leased && !info.configured ? RS_STARTING : RS_OFF;
}

static radio_state_t rtl_read(void) { return sdr_endpoint_read(LS_RADIO_ENDPOINT_RTL_USB); }
static radio_state_t hackrf_read(void) { return sdr_endpoint_read(LS_RADIO_ENDPOINT_HACKRF_USB); }
static const char *sdr_word(radio_state_t st);
static const char *rtl_state(void) { return sdr_word(rtl_read()); }
static const char *hackrf_state(void) { return sdr_word(hackrf_read()); }

static void sdr_set_endpoint(const char *id)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present) return;
    const ls_radio_err_t err = ls_radio_endpoint_pause(id, info.owner, !info.paused);
    if (err != LS_RADIO_OK) {
        snprintf(s_error_text, sizeof(s_error_text), "%s: %s (%s)",
                 info.name, ls_radio_err_name(err), info.owner);
        s_error = s_error_text;
    }
}
static void rtl_set(bool on) { (void)on; sdr_set_endpoint(LS_RADIO_ENDPOINT_RTL_USB); }
static void hackrf_set(bool on) { (void)on; sdr_set_endpoint(LS_RADIO_ENDPOINT_HACKRF_USB); }
static const char *sdr_action(const char *id)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present) return "unavailable";
    return info.paused ? "ALLOW RX" : "STOP / BLOCK RX";
}
static const char *rtl_action(void) { return sdr_action(LS_RADIO_ENDPOINT_RTL_USB); }
static const char *hackrf_action(void) { return sdr_action(LS_RADIO_ENDPOINT_HACKRF_USB); }

static radio_state_t gps_read(void)
{

    if (!ls_gps_running()) return RS_OFF;
    ls_gps_state_t g;
    ls_gps_get(&g);
    return g.fix ? RS_BUSY : RS_ON;
}

static const char *gps_state(void)
{
    if (!ls_gps_running()) return "off";
    ls_gps_state_t g;
    ls_gps_get(&g);
    return g.fix ? "fix" : "searching";
}

static void gps_set(bool on)
{
    if (on) {
        const esp_err_t err = ls_gps_start();
        s_error = err == ESP_OK ? NULL : "GPS start failed (memory / UART)";
    }
    else    ls_gps_stop();
}

#if LS_HAS_LORA
static radio_state_t mesh_read(void)
{
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (!s.present && !ls_mesh_running()) return RS_OFF;
    return s.receiving || s.transmitting ? RS_BUSY : s.present ? RS_ON : RS_OFF;
}

/* The task holding the chip, as the app it belongs to. */
static const char *holder_name(const char *task)
{
    static const struct { const char *prefix, *name; } NAMES[] = {
        { "adsb", "ADSB" }, { "fm", "FM/P25" }, { "p25", "P25" }, { "catch", "CATCH" },
        { "drone", "DRONES" }, { "subghz", "SUB-GHZ" }, { "exp", "EXPERIMENTS" },
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        if (!strncmp(task, NAMES[i].prefix, strlen(NAMES[i].prefix))) return NAMES[i].name;
    return task;
}

static const char *mesh_state(void)
{
    static char text[64];
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (s.blocked)
        snprintf(text, sizeof(text), "%s %s", ls_tr(s.chip), ls_tr("blocked"));
    else if (s.held && strcmp(s.owner, "MeshCore") && strcmp(s.owner, "none"))
        snprintf(text, sizeof(text), "%s %s %s", ls_tr(s.chip), ls_tr("held by"), holder_name(s.owner));
    else
        snprintf(text, sizeof(text), "%s %s", ls_tr(s.chip),
                 ls_tr(s.transmitting ? "TX" : s.receiving ? "RX" : s.held ? "held" :
                       s.armed ? "armed" : "parked"));
    return text;
}

static const char *mesh_action(void)
{
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (s.blocked) return "ALLOW LORA";
    if (s.held) return "STOP / BLOCK LORA";
    return ls_mesh_running() ? "TURN OFF" : "TURN ON";
}

/* Held by an app (ADSB or P25 on the LoRa chip, a sweep, CATCH...), the tap
   takes the chip back and keeps it free until ALLOW: the holder sees it
   wanted and lets go, as it does for any other foreground use. */
static void mesh_set(bool on)
{
    ls_mesh_socket_t s;
    ls_mesh_socket_get(&s);
    if (s.blocked) {
        ls_mesh_radio_block(false);
        s_error = "LORA allowed: apps can take it again";
        return;
    }
    if (s.held) {
        ls_mesh_radio_block(true);
        snprintf(s_error_text, sizeof(s_error_text), ls_tr("LORA taken back from %s; ALLOW gives it back"),
                 holder_name(s.owner));
        s_error = s_error_text;
        return;
    }
    on = !ls_mesh_running();
    const esp_err_t err = on ? ls_mesh_start() : ls_mesh_stop();
    s_error = err == ESP_OK ? NULL : on ? "LORA start failed" : "LORA stop failed";
}
#endif

#ifdef LS_BOARD_MIX_CC_CS
static radio_state_t cc_read(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.cc?RS_ABSENT:(s.receiving||s.transmitting)?RS_BUSY:RS_ON;}
static const char *cc_state(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.cc?"absent":s.transmitting?"TX replay":s.receiving?"receiving":"on";}
static radio_state_t nrf_read(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.nrf?RS_ABSENT:s.scanning?RS_BUSY:RS_ON;}
static radio_state_t nfc_read(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.nfc?RS_ABSENT:(s.card_scanning||s.nfc_watching||ls_nfc_suite_busy())?RS_BUSY:RS_ON;}
static void open_mix(bool on){(void)on;for(int i=0;i<ls_app_count();i++){const ls_app_t *a=ls_app_at(i);if(a && !strcmp(a->id,"mixrf")){ls_app_open(i);return;}}}
static const char *mix_action(void){return "OPEN MIX-RF";}
#else
static radio_state_t absent_read(void) { return RS_ABSENT; }
#endif

static radio_state_t ant_read(void)
{
    return ls_board_hw_antenna_is_external() ? RS_ON : RS_OFF;
}
/* One tap, no dialog. MMCX1 receives; whether it transmits too is the
   MMCX1 USE option, and the box's second line says which. */
static void ant_set(bool external)
{
    const esp_err_t err = ls_board_hw_antenna_external(external);
    s_error = err == ESP_OK ? NULL : "Antenna: transmit in progress, try again";
    if (err == ESP_OK) settings_set_antenna_external(external);
}
static const char *ant_state(void)
{
    if (!ls_board_hw_antenna_is_external()) return "internal";
    return ls_board_hw_antenna_mmcx_tx() ? "MMCX1 rx+tx" : "MMCX1 rx";
}
static const char *ant_action(void)
{
    return ls_board_hw_antenna_is_external() ? "USE INTERNAL" : "USE MMCX1";
}

#if LS_HAS_C6
/* The worker owns the C6; keep its snapshot off the TUI stack. */
static EXT_RAM_BSS_ATTR ls_wireless_snapshot_t s_wireless;
static radio_state_t ble_read(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.bt_available ? RS_ABSENT : s_wireless.bt_failed ? RS_FAILED : !s_wireless.bt_running ? RS_OFF : s_wireless.bt_ready ? RS_BUSY : RS_ON; }
static radio_state_t wifi_read(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.wifi_available ? RS_ABSENT : !s_wireless.wifi_running ? RS_OFF : s_wireless.wifi_connected ? RS_BUSY : RS_ON; }
static const char *ble_state(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.bt_available ? "unavailable" : s_wireless.bt_failed ? "failed" : !s_wireless.bt_running ? "off" : s_wireless.bt_status[0] ? s_wireless.bt_status : "starting"; }
static const char *wifi_state(void)
{ ls_wireless_get(&s_wireless); return !s_wireless.wifi_available ? "unavailable" : !s_wireless.wifi_running ? "off" : s_wireless.wifi_connected ? "connected" : s_wireless.busy ? "busy" : "disconnected"; }
#endif
static void open_link(bool on)
{ (void)on; for (int i = 0; i < ls_app_count(); i++) { const ls_app_t *a = ls_app_at(i); if (a && !strcmp(a->id, "link")) { ls_app_open(i); return; } } }
static const char *link_action(void) { return "OPEN LINK"; }

static const radio_row_t ROWS[] = {
    { "RTL-SDR", "ALLOW RX permits owner retry / app start", rtl_read, rtl_set, rtl_state, rtl_action },
    { "HackRF", "ALLOW RX permits owner retry / app start", hackrf_read, hackrf_set, hackrf_state, hackrf_action },
#if LS_HAS_LORA
    { "LORA", "MeshCore from boot; STOP frees it from any app",
      mesh_read, mesh_set, mesh_state, mesh_action },
#endif
    { "GPS",  "keeps a receiver fed to hold a fix",
      gps_read, gps_set, gps_state },
    { "ANTENNA", "internal, or MMCX1 for receive",
      ant_read, ant_set, ant_state, ant_action, true },
#if LS_HAS_C6
    { "BLE",  "Live C6 / runtime controls in LINK",
      ble_read, open_link, ble_state, link_action },
    { "WI-FI", "Live C6 / runtime controls in LINK",
      wifi_read, open_link, wifi_state, link_action },
#endif
#ifdef LS_BOARD_MIX_CC_CS
    { "CC1101", "Keyboard sub-GHz receive monitor",cc_read,open_mix,cc_state,mix_action },
    { "NRF24", "Keyboard 2.4 GHz energy survey",nrf_read,open_mix,NULL,mix_action },
    { "NFC", "Keyboard card reader / workbench",nfc_read,open_mix,NULL,mix_action },
#else
    { "CC1101", "sub-GHz front end, no driver yet",
      absent_read, NULL },
    { "NFC",  "reader front end, no driver yet",
      absent_read, NULL },
#endif
};
#define N_ROWS ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

static int s_sel;

/* ------------------------------------------------------------------ draw -- */

static const char *state_word(radio_state_t s)
{
    switch (s) {
    case RS_BUSY:   return "ACTIVE";
    case RS_ON:     return "on";
    case RS_OFF:    return "off";
    case RS_STARTING: return "starting";
    case RS_FAILED: return "failed";
    default:        return "absent";
    }
}

static const char *sdr_word(radio_state_t st)
{ return st == RS_OFF ? "parked" : st == RS_ABSENT ? "unavailable" : state_word(st); }

/* What pressing it would do, which is not the same as what it is.

   A switch labelled with its own state makes the reader do the inversion,
   and half of them get it wrong - the classic "does ON mean it is on, or
   that pressing it turns it on". The border says the state and the key says
   the action, so neither has to be guessed from the other. */
static const char *action_word(radio_state_t s, bool can)
{
    if (!can) return "no driver";
    return (s == RS_OFF) ? "TURN ON" : "TURN OFF";
}

static int count_live(void)
{
    int n = 0;
    for (int i = 0; i < N_ROWS; i++) {
        if (ROWS[i].no_power) continue;   /* a path, not a load */
        if (ROWS[i].at_boot) continue;    /* next boot, not now */
        const radio_state_t s = ROWS[i].read();
        if (s == RS_ON || s == RS_BUSY) n++;
    }
    return n;
}

/* Each radio is a box you press, not a row you select. */

static tui_rect s_hit[12];
static int s_hit_row[12];
static int s_page_first, s_page_size;
static tui_rect s_page_prev, s_page_next;
static int      s_hit_n;

static void draw_one(tui_surface *sf, tui_rect a, int i, int bh)
{
    const uint8_t dim  = A(DIM_FG, TUI_BLACK);
    const radio_state_t st = ROWS[i].read();
    const bool can = ROWS[i].set && (st != RS_ABSENT || ROWS[i].set == open_link);
    const bool sel = (i == s_sel);

    const uint8_t edge = can ? A(TUI_CYAN, TUI_BLACK) : dim;
    const uint8_t name = can ? A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK) : dim;
    const uint8_t val  = !can              ? dim
                       : st == RS_BUSY     ? A(TUI_GREEN | TUI_BRIGHT, TUI_BLACK)
                       : st == RS_ON       ? A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK)
                                           : dim;

    ls_panel_box(sf, a, NULL, can ? TUI_CYAN : (TUI_BLACK | TUI_BRIGHT));

    /* Name and state in the top border, the way every panel on this
       interface is labelled. */
    tui_put_char(sf, a, a.x + 1, a.y, ' ', edge);
    const char *row_name = ls_tr(ROWS[i].name);
    tui_put_str(sf, a, a.x + 2, a.y, row_name, name);
    tui_put_char(sf, a, a.x + 2 + (int)strlen(row_name), a.y, ' ', edge);

    const char *w = ls_tr(ROWS[i].state_text ? ROWS[i].state_text() : state_word(st));
    const int wn = (int)strlen(w);
    if (a.w > wn + 8) {
        tui_put_char(sf, a, a.x + a.w - wn - 3, a.y, ' ', edge);
        tui_put_str(sf, a, a.x + a.w - wn - 2, a.y, w, val);
        tui_put_char(sf, a, a.x + a.w - 2, a.y, ' ', edge);

    }

    int y = a.y + 1;
    bool detail = false;
    char what[72];
#if LS_HAS_LORA
    if (ROWS[i].set == mesh_set) {
        ls_mesh_socket_t socket;
        ls_mesh_socket_get(&socket);
        snprintf(what, sizeof(what), "%s / %s", socket.owner, socket.mode);
        detail = true;
    }
#endif
    if (ROWS[i].set == ant_set && ls_board_hw_antenna_is_external()) {
        snprintf(what, sizeof(what), "%s", ls_tr(ls_board_hw_antenna_mmcx_tx() ?
                 "TX on MMCX1: band antenna on it" : "TX goes out the internal antenna"));
        detail = true;
    }
    if (ROWS[i].set == rtl_set || ROWS[i].set == hackrf_set) {
        ls_radio_endpoint_info_t info;
        if (ls_radio_endpoint_get(ROWS[i].set == rtl_set ? LS_RADIO_ENDPOINT_RTL_USB :
                                  LS_RADIO_ENDPOINT_HACKRF_USB, &info) == LS_RADIO_OK && info.leased) {
            snprintf(what, sizeof(what), ls_tr("Owner: %s"), info.owner);
            detail = true;
        }
    }
    if (bh >= 6 || detail) {
        if (!detail) snprintf(what, sizeof(what), "%.*s", a.w - 4, ls_tr(ROWS[i].what));
        else if (a.w - 4 >= 0 && a.w - 4 < (int)sizeof(what)) what[a.w - 4] = '\0';
        tui_put_str(sf, a, a.x + 2, y, what, dim);
        y++;
    }

    /* The key: the interior, in the quarter-block shade the rest of this
       interface uses for a pressable field, with what pressing it does
       written across the middle. Not a solid rectangle - that is the pixel
       toolkit's answer and this has an alphabet. */
    const int kh = a.y + a.h - 1 - y;
    if (kh >= 1) {
        const uint8_t field = !can ? dim
                            : sel  ? A(TUI_BLACK, TUI_CYAN | TUI_BRIGHT)
                                   : A(TUI_CYAN, TUI_BLACK);
        for (int r = 0; r < kh; r++)
            for (int c = 1; c < a.w - 1; c++)
                tui_put_char(sf, a, a.x + c, y + r, LS_TUI_SHADE_25, field);

        const char *act = ls_tr(ROWS[i].action_text ? ROWS[i].action_text()
                                                    : action_word(st, can));
        const int an = (int)strlen(act);
        const int ax = a.x + (a.w - an) / 2;
        const int ay = y + (kh - 1) / 2;
        const uint8_t at = !can ? dim
                         : sel  ? A(TUI_BLACK, TUI_CYAN | TUI_BRIGHT)
                                : A(TUI_WHITE | TUI_BRIGHT, TUI_BLACK);
        if (ax - 1 > a.x) tui_put_char(sf, a, ax - 1, ay, ' ', at);
        if (ax + an < a.x + a.w - 1) tui_put_char(sf, a, ax + an, ay, ' ', at);
        tui_put_str(sf, a, ax, ay, act, at);
    }

    if (s_hit_n < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
    {
        s_hit_row[s_hit_n] = i;
        s_hit[s_hit_n++] = a;
    }
}

static void draw(tui_surface *sf, tui_rect area)
{
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    char buf[64];

    s_hit_n = 0;
    s_options_hit = s_page_prev = s_page_next = tui_rect_make(0, 0, 0, 0);
    ls_panel_box(sf, area, "RADIOS", TUI_CYAN);
    if (area.h < 8 || area.w < 20) return;

    const int live = count_live();
    int loads = 0;
    for (int i = 0; i < N_ROWS; i++)
        if (!ROWS[i].no_power && !ROWS[i].at_boot) loads++;
    snprintf(buf, sizeof(buf), ls_tr("%d of %d powered"), live, loads);
    tui_put_str(sf, area, area.x + 2, area.y + 1, buf,
                live ? A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : dim);
    if (s_error) tui_put_str(sf, area, area.x + 2, area.y + 2, ls_tr(s_error), A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));

    tui_rect body = tui_rect_make(area.x + 1, area.y + 3,
                                  area.w - 2, area.h - 8);
    if (body.h < 4) return;

    const int ncols = ls_tui_is_wide() && body.w >= 48 ?
        (body.w >= 72 ? 3 : 2) : 1;
    const int total_per = (N_ROWS + ncols - 1) / ncols;
    int bh = (body.h - (total_per - 1)) / total_per;
    if (bh > 9) bh = 9;
    if (bh < 4) bh = 4;
    const int gap = body.h >= total_per * bh + total_per - 1 ? 1 : 0;
    const int per = (body.h + gap) / (bh + gap);
    s_page_size = per * ncols;
    if (s_sel < N_ROWS) s_page_first = (s_sel / s_page_size) * s_page_size;
    const int cw = body.w / ncols;
    for (int slot = 0; slot < s_page_size && s_page_first + slot < N_ROWS; slot++) {
        const int i = s_page_first + slot;
        const int x = body.x + (slot / per) * cw;
        const int y = body.y + (slot % per) * (bh + gap);
        draw_one(sf, tui_rect_make(x, y, cw - (ncols > 1), bh), i, bh);
    }
    s_page_prev = s_page_next = tui_rect_make(0, 0, 0, 0);
    if (s_page_size < N_ROWS) {
        const int y = area.y + area.h - 5;
        snprintf(buf, sizeof(buf), "%d/%d",
                 s_page_first / s_page_size + 1, (N_ROWS + s_page_size - 1) / s_page_size);
        tui_put_str(sf, area, area.x + 2, y, ls_tr("< PREV"), A(TUI_CYAN, TUI_BLACK));
        tui_put_str(sf, area, area.x + (area.w - (int)strlen(buf)) / 2, y, buf, A(TUI_CYAN, TUI_BLACK));
        tui_put_str(sf, area, area.x + area.w - 2 - (int)strlen(ls_tr("NEXT >")), y, ls_tr("NEXT >"), A(TUI_CYAN, TUI_BLACK));
        s_page_prev = tui_rect_make(area.x + 1, y, area.w / 2, 1);
        s_page_next = tui_rect_make(area.x + 1 + area.w / 2, y, area.w / 2 - 1, 1);
    }

    s_options_hit = tui_rect_make(area.x + 2, area.y + area.h - 4, area.w - 4, 3);
    const ls_btn_t opt = ls_opt_button(&RADIO_CONTEXT);
    ls_btn_bar_raised(sf, s_options_hit, &opt, 1, s_sel == N_ROWS ? 0 : -1);

}

/* ----------------------------------------------------------------- input -- */

static void toggle(int i)
{
    if (i < 0 || i >= N_ROWS) return;
    if (!ROWS[i].set) return;
    if (ROWS[i].read() == RS_ABSENT && ROWS[i].set != open_link) return;
    s_error = NULL;
    ROWS[i].set(ROWS[i].read() == RS_OFF);
}

static void enter(void)
{
    s_error = NULL; s_page_first = 0; s_sel = 0;
#if LS_HAS_C6
    ls_wireless_observe(true);
#endif
}
static void leave(void)
{
#if LS_HAS_C6
    ls_wireless_observe(false);
#endif
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    if (k == LS_TK_CHAR && (ch == LS_OPT_KEY || ch == 'O')) {
        options_open(); return true;
    }
    switch (k) {
    case LS_TK_UP:    if (s_sel > 0) s_sel--; return true;
    case LS_TK_DOWN:  if (s_sel < N_ROWS) s_sel++; return true;
    case LS_TK_TAB: s_sel = (s_sel + 1) % (N_ROWS + 1); return true;
    case LS_TK_ENTER:
        if (s_sel == N_ROWS) options_open();
        else toggle(s_sel);
        return true;
    default: return false;
    }
}

static bool touch(int col, int row)
{
    if (col >= s_options_hit.x && col < s_options_hit.x + s_options_hit.w &&
        row >= s_options_hit.y && row < s_options_hit.y + s_options_hit.h) {
        options_open(); return true;
    }
    if (s_page_size > 0 && row == s_page_prev.y && s_page_prev.w > 0) {
        if (col >= s_page_prev.x && col < s_page_prev.x + s_page_prev.w) {
            s_sel = s_page_first >= s_page_size ? s_page_first - s_page_size : 0;
            return true;
        }
        if (col >= s_page_next.x && col < s_page_next.x + s_page_next.w) {
            if (s_page_first + s_page_size < N_ROWS) s_sel = s_page_first + s_page_size;
            return true;
        }
    }
    for (int i = 0; i < s_hit_n && i < N_ROWS; i++) {
        const tui_rect r = s_hit[i];
        if (col >= r.x && col < r.x + r.w &&
            row >= r.y && row < r.y + r.h) {
            s_sel = s_hit_row[i];
            toggle(s_sel);
            return true;
        }
    }
    /* A miss is a miss. */

    return true;
}

const ls_tui_screen_t ls_scr_radios = {
    .name = "RADIOS",
    .hint = "TAP to switch  UP DOWN ENTER",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .key = key,
    .touch = touch,
};
