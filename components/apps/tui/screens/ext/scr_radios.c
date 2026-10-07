/* RADIOS: what is transmitting, receiving, or drawing current, and a way to stop it. */

#include "../../ls_tui_screen.h"
#include "board/ls_board_hw.h"
#include "core/settings.h"

#include <stdio.h>
#include <string.h>

#include "../../ls_tui.h"
#include "../../ls_tui_ui.h"
#include "../../ls_motion.h"
#include "../../ls_quick.h"

/* ls_board.h and not ls_caps.h: the caps header has an #error in it saying
   exactly that, which is the right way for a header to be private. */
#include "ls_board.h"
#include "ls_gps.h"
#ifdef LS_BOARD_MIX_CC_CS
#include "ls_mixrf.h"
#include "ls_nfc_suite.h"
#include "../../ls_app.h"
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
static bool s_ant_confirm, s_ant_accept;
static tui_rect s_ant_yes, s_ant_no;

/* A parked endpoint does not establish that its hardware is drawing power. */
static radio_state_t sdr_endpoint_read(const char *id)
{
    ls_radio_endpoint_info_t info;
    if (ls_radio_endpoint_get(id, &info) != LS_RADIO_OK || !info.present)
        return RS_ABSENT;
    /* last_error is the last thing that went wrong in the endpoint and is
       never cleared by success. A read that ends because the stream was
       stopped, timed out or found the device busy is an ordinary event, and
       reading it as failure showed a receiver that was streaming fine, and
       one just turned off, as failed. Only a real device fault counts. */
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

static void sdr_set(bool on)
{
    /* Off is immediate. On hands the decision back to the screen, which
       asks again on its next change; a screen showing a receiver gets it
       back the moment it is looked at. */
    if (!on) ls_tui_radio_want(NULL);
}

static radio_state_t gps_read(void)
{

    if (!ls_gps_running()) return RS_OFF;
    ls_gps_state_t g;
    ls_gps_get(&g);
    return g.fix ? RS_BUSY : RS_ON;
}

static void gps_set(bool on)
{
    if (on) ls_gps_start();
    else    ls_gps_stop();
}

#if LS_HAS_LORA
static radio_state_t mesh_read(void)
{
    if (!ls_mesh_running()) return RS_OFF;
    return ls_mesh_tx_enabled() ? RS_BUSY : RS_ON;
}

static void mesh_set(bool on)
{
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
static void ant_set(bool external)
{
    if (external) {
        s_ant_confirm = true; s_ant_accept = false;
        s_ant_yes = s_ant_no = tui_rect_make(0, 0, 0, 0);
        return;
    }
    const esp_err_t err = ls_board_hw_antenna_external(false);
    s_error = err == ESP_OK ? NULL : "Antenna busy / switch failed";
    if (err == ESP_OK) settings_set_antenna_external(false);
}
static const char *ant_state(void)
{
    return ls_board_hw_antenna_is_external() ? "MMCX1" : "internal";
}
static const char *ant_action(void)
{
    return ls_board_hw_antenna_is_external() ?
        (ls_board_hw_antenna_tx_allowed() ? "USE INTERNAL" : "CONFIRM MMCX1") : "USE MMCX1";
}

#if LS_HAS_C6
/* BLE and Wi-Fi are decided once, at boot, before settings load - so these
   rows switch the stored preference and say so, the same byte 'radios ble'
   and 'radios wifi' write on the console. Stopping a running BLE stack from
   here is a different control with different risks, and is not this one. */
static radio_state_t ble_read(void)  { return settings_get_ble_at_boot()  ? RS_ON : RS_OFF; }
static radio_state_t wifi_read(void) { return settings_get_wifi_at_boot() ? RS_ON : RS_OFF; }
static void ble_set(bool on)  { settings_set_ble_at_boot(on); }
static void wifi_set(bool on) { settings_set_wifi_at_boot(on); }
static const char *ble_state(void)  { return settings_get_ble_at_boot()  ? "on at boot" : "off at boot"; }
static const char *wifi_state(void) { return settings_get_wifi_at_boot() ? "on at boot" : "off at boot"; }
static const char *ble_action(void)  { return settings_get_ble_at_boot()  ? "OFF AT BOOT" : "ON AT BOOT"; }
static const char *wifi_action(void) { return settings_get_wifi_at_boot() ? "OFF AT BOOT" : "ON AT BOOT"; }
#endif

static const radio_row_t ROWS[] = {
    { "RTL-SDR", "USB receiver endpoint", rtl_read, sdr_set, rtl_state },
    { "HackRF", "USB receiver endpoint", hackrf_read, sdr_set, hackrf_state },
#if LS_HAS_LORA
    { "LORA", "MeshCore listens from boot",
      mesh_read, mesh_set },
#endif
    { "GPS",  "keeps a receiver fed to hold a fix",
      gps_read, gps_set },
    { "ANTENNA", "internal, or external through MMCX1",
      ant_read, ant_set, ant_state, ant_action, true },
#if LS_HAS_C6
    { "BLE",  "scans for a control head all day; next reboot",
      ble_read, ble_set, ble_state, ble_action, false, true },
    { "WI-FI", "rejoins the saved network; next reboot",
      wifi_read, wifi_set, wifi_state, wifi_action, false, true },
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
static int      s_hit_n;

static void draw_one(tui_surface *sf, tui_rect a, int i, int bh)
{
    const uint8_t dim  = A(DIM_FG, TUI_BLACK);
    const radio_state_t st = ROWS[i].read();
    const bool can = (ROWS[i].set != NULL);
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
    tui_put_str(sf, a, a.x + 2, a.y, ROWS[i].name, name);
    tui_put_char(sf, a, a.x + 2 + (int)strlen(ROWS[i].name), a.y, ' ', edge);

    const char *w = ROWS[i].state_text ? ROWS[i].state_text() : state_word(st);
    const int wn = (int)strlen(w);
    if (a.w > wn + 8) {
        tui_put_char(sf, a, a.x + a.w - wn - 3, a.y, ' ', edge);
        tui_put_str(sf, a, a.x + a.w - wn - 2, a.y, w, val);
        tui_put_char(sf, a, a.x + a.w - 2, a.y, ' ', edge);

        /* A turning mark beside ACTIVE. */

        if (st == RS_BUSY && a.w > wn + 10)
            tui_put_char(sf, a, a.x + a.w - wn - 4, a.y,
                         ls_motion_pip(true), val);
    }

    int y = a.y + 1;
    if (bh >= 6) {
        char what[72];
        snprintf(what, sizeof(what), "%.*s", a.w - 4, ROWS[i].what);
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

        const char *act = ROWS[i].action_text ? ROWS[i].action_text()
                                             : action_word(st, can);
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
        s_hit[s_hit_n++] = a;
}

static void draw(tui_surface *sf, tui_rect area)
{
    const uint8_t dim = A(DIM_FG, TUI_BLACK);
    char buf[64];

    s_hit_n = 0;
    ls_panel_box(sf, area, "RADIOS", TUI_CYAN);
    if (area.h < 8 || area.w < 20) return;
    if (s_ant_confirm) {
        const uint8_t warn = A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK);
        tui_put_str(sf, area, area.x + 2, area.y + 2, "MMCX1 EXTERNAL ANTENNA", warn);
        tui_put_str(sf, area, area.x + 2, area.y + 4, "TX without an antenna", warn);
        tui_put_str(sf, area, area.x + 2, area.y + 5, "can damage the radio.", warn);
        tui_put_str(sf, area, area.x + 2, area.y + 7, "Attach a suitable antenna", dim);
        tui_put_str(sf, area, area.x + 2, area.y + 8, "before confirming.", dim);
        s_ant_no = tui_rect_make(area.x + 2, area.y + 10, area.w - 4, 3);
        s_ant_yes = tui_rect_make(area.x + 2, area.y + 14, area.w - 4, 3);
        tui_put_str(sf, area, s_ant_no.x, s_ant_no.y, s_ant_accept ? "  CANCEL" : "> CANCEL", dim);
        tui_put_str(sf, area, s_ant_yes.x, s_ant_yes.y, s_ant_accept ? "> ANTENNA ATTACHED" : "  ANTENNA ATTACHED", warn);
        return;
    }

    const int live = count_live();
    int loads = 0;
    for (int i = 0; i < N_ROWS; i++)
        if (!ROWS[i].no_power && !ROWS[i].at_boot) loads++;
    snprintf(buf, sizeof(buf), "%d of %d powered", live, loads);
    tui_put_str(sf, area, area.x + 2, area.y + 1, buf,
                live ? A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : dim);
    if (s_error) tui_put_str(sf, area, area.x + 2, area.y + 2, s_error, A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK));

    tui_rect body = tui_rect_make(area.x + 1, area.y + 3,
                                  area.w - 2, area.h - 4);
    if (body.h < 4) return;

    /* Two columns when the width is there and the height is not.

       Landscape is 115 columns and about 24 rows: five boxes down it would
       be four rows each, which is the target size problem again. Beside each
       other they stay tall. */
    if (ls_tui_is_wide() && body.w >= 48) {
        const int ncols = body.w >= 72 && body.h < ((N_ROWS + 1) / 2) * 4 ? 3 : 2;
        const int per = (N_ROWS + ncols - 1) / ncols;
        const int gap = body.h >= per * 5 - 1 ? 1 : 0;
        const int cw = body.w / ncols;
        const int bh_l = (body.h - (per - 1) * gap) / per;
        int bh = bh_l;
        if (bh > 9) bh = 9;
        if (bh < 4) bh = 4;
        for (int i = 0; i < N_ROWS; i++) {
            const tui_rect col = tui_rect_make(body.x + (i / per) * cw,
                                               body.y, cw, body.h);
            const int slot = i % per;
            const int y = col.y + slot * (bh + gap);
            if (y + bh > col.y + col.h) break;
            draw_one(sf, tui_rect_make(col.x, y, col.w - 1, bh), i, bh);
        }
        return;
    }

    int bh = (body.h - (N_ROWS - 1)) / N_ROWS;
    /* Nine rows is 11.6 mm, which is past generous for a thumb; taller buys
       nothing and a screen of four enormous slabs reads as a mistake. */
    if (bh > 9) bh = 9;
    if (bh < 4) bh = 4;

    const int used = N_ROWS * bh + (N_ROWS - 1);
    int top = body.y + (body.h > used ? (body.h - used) / 2 : 0);

    for (int i = 0; i < N_ROWS; i++) {
        const int y = top + i * (bh + 1);
        if (y + bh > body.y + body.h) break;
        draw_one(sf, tui_rect_make(body.x, y, body.w, bh), i, bh);
    }
}

/* ----------------------------------------------------------------- input -- */

static void toggle(int i)
{
    if (i < 0 || i >= N_ROWS) return;
    if (!ROWS[i].set) return;
    s_error = NULL;
    if (ROWS[i].set == ant_set && ls_board_hw_antenna_is_external() &&
        !ls_board_hw_antenna_tx_allowed()) { ant_set(true); return; }
    ROWS[i].set(ROWS[i].read() == RS_OFF);
}

static void ant_confirm(bool accept)
{
    s_ant_confirm = false;
    if (!accept) return;
    const esp_err_t err = ls_board_hw_antenna_confirm_external();
    s_error = err == ESP_OK ? NULL : "Antenna busy / switch failed";
    if (err == ESP_OK) settings_set_antenna_external(true);
}

static void enter(void) { s_ant_confirm = false; s_error = NULL; }
static void leave(void) { s_ant_confirm = false; }

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    if (s_ant_confirm) {
        if (k == LS_TK_UP) s_ant_accept = false;
        else if (k == LS_TK_DOWN) s_ant_accept = true;
        else if (k == LS_TK_ENTER) ant_confirm(s_ant_accept);
        else if (k == LS_TK_ESC) ant_confirm(false);
        return true;
    }
    switch (k) {
    case LS_TK_UP:    if (s_sel > 0) s_sel--; return true;
    case LS_TK_DOWN:  if (s_sel < N_ROWS - 1) s_sel++; return true;
    case LS_TK_ENTER: toggle(s_sel); return true;
    default: return false;
    }
}

static bool touch(int col, int row)
{
    if (s_ant_confirm) {
        const tui_rect r = s_ant_yes;
        if (col >= r.x && col < r.x + r.w && row >= r.y && row < r.y + r.h)
            ant_confirm(true);
        else if (col >= s_ant_no.x && col < s_ant_no.x + s_ant_no.w &&
                 row >= s_ant_no.y && row < s_ant_no.y + s_ant_no.h)
            ant_confirm(false);
        return true;
    }
    for (int i = 0; i < s_hit_n && i < N_ROWS; i++) {
        const tui_rect r = s_hit[i];
        if (col >= r.x && col < r.x + r.w &&
            row >= r.y && row < r.y + r.h) {
            s_sel = i;
            toggle(i);
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
