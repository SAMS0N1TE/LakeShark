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

/* The receiver's own status, through the module that owns it. This screen
   asks the same question ls_wf_source asks and must get the same answer. */
#include "p25_state.h"
#include "iq_app_control.h"

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
} radio_state_t;

typedef struct {
    const char *name;
    const char *what;                 /* one line: what it costs you     */
    radio_state_t (*read)(void);
    void (*set)(bool on);             /* NULL when it cannot be switched */

    const char *(*state_text)(void);  /* NULL: the on/off words          */
    const char *(*action_text)(void); /* NULL: TURN ON / TURN OFF        */
    bool        no_power;             /* selects a path, draws nothing   */
} radio_row_t;

/* ------------------------------------------------------------------ rows -- */

static bool s_sdr_held_off;

static radio_state_t sdr_read(void)
{
    ls_iq_control_status_t st;
    memset(&st, 0, sizeof(st));
    p25_get_receiver_status(&st);
    if (st.receiver_streaming) return RS_BUSY;
    return s_sdr_held_off ? RS_OFF : RS_ON;
}

static void sdr_set(bool on)
{
    s_sdr_held_off = !on;
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
    if (on) ls_mesh_start();
    else    ls_mesh_stop();
}
#endif

#ifdef LS_BOARD_MIX_CC_CS
static radio_state_t cc_read(void){ls_mixrf_status_t s;ls_mixrf_snapshot(&s);return !s.cc?RS_ABSENT:s.receiving?RS_BUSY:RS_ON;}
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
    if (ls_board_hw_antenna_external(external) != ESP_OK) return;
    settings_set_antenna_external(external);
}
static const char *ant_state(void)
{
    return ls_board_hw_antenna_is_external() ? "MMCX1" : "internal";
}
static const char *ant_action(void)
{
    return ls_board_hw_antenna_is_external() ? "USE INTERNAL" : "USE MMCX1";
}

static const radio_row_t ROWS[] = {
    { "SDR",  "USB dongle, the biggest draw here",
      sdr_read, sdr_set },
#if LS_HAS_LORA
    { "LORA", "MeshCore listens from boot",
      mesh_read, mesh_set },
#endif
    { "GPS",  "keeps a receiver fed to hold a fix",
      gps_read, gps_set },
    { "ANTENNA", "internal, or external through MMCX1",
      ant_read, ant_set, ant_state, ant_action, true },
#ifdef LS_BOARD_MIX_CC_CS
    { "CC1101", "Keyboard sub-GHz receive monitor",cc_read,open_mix,NULL,mix_action },
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
    default:        return "absent";
    }
}

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
        const radio_state_t s = ROWS[i].read();
        if (s == RS_ON || s == RS_BUSY) n++;
    }
    return n;
}

/* Each radio is a box you press, not a row you select. */

static tui_rect s_hit[8];
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

    const int live = count_live();
    snprintf(buf, sizeof(buf), "%d of %d powered", live, N_ROWS);
    tui_put_str(sf, area, area.x + 2, area.y + 1, buf,
                live ? A(TUI_YELLOW | TUI_BRIGHT, TUI_BLACK) : dim);

    tui_rect body = tui_rect_make(area.x + 1, area.y + 3,
                                  area.w - 2, area.h - 4);
    if (body.h < 4) return;

    /* Two columns when the width is there and the height is not.

       Landscape is 115 columns and about 24 rows: five boxes down it would
       be four rows each, which is the target size problem again. Beside each
       other they stay tall. */
    if (ls_tui_is_wide() && body.w >= 48) {
        tui_rect left, right;
        ls_tui_split(body, &left, &right);
        const int half = (N_ROWS + 1) / 2;
        const int bh_l = (left.h - (half - 1)) / (half ? half : 1);
        int bh = bh_l;
        if (bh > 9) bh = 9;
        if (bh < 4) bh = 4;
        for (int i = 0; i < N_ROWS; i++) {
            const tui_rect col = (i < half) ? left : right;
            const int slot = (i < half) ? i : i - half;
            const int y = col.y + slot * (bh + 1);
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
    ROWS[i].set(ROWS[i].read() == RS_OFF);
}

static bool key(ls_tk_t k, char ch)
{
    (void)ch;
    switch (k) {
    case LS_TK_UP:    if (s_sel > 0) s_sel--; return true;
    case LS_TK_DOWN:  if (s_sel < N_ROWS - 1) s_sel++; return true;
    case LS_TK_ENTER: toggle(s_sel); return true;
    default: return false;
    }
}

static bool touch(int col, int row)
{
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
    .enter = NULL,
    .leave = NULL,
    .draw = draw,
    .key = key,
    .touch = touch,
};
