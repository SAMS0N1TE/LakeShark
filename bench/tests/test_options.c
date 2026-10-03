/* LS_TEST_SOURCES: ls_options.c with the RADIO picker's board under it, a
   table of every kind of row, and the OPTIONS list against its own drawn
   output */

#include "ls_test.h"

#include "ls_options.h"
#include "ls_radio_select.h"
#include "radio_choice.h"
#include "ls_picker.h"
#include "ls_numpad.h"
#include "ls_keyboard.h"
#include "ls_lora.h"
#include "radio_endpoint.h"
#include "tui_core.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- board -- */

static ls_rsel_hw_t s_board;

void ls_rsel_hw(ls_rsel_hw_t *out) { *out = s_board; }
void ls_rsel_hw_restart(void) { }

static uint64_t s_packed = LS_RSEL_PACKED_NONE;
int settings_get_radio_choice(int job) { return ls_rsel_unpack(s_packed, job); }
bool settings_set_radio_choice(int job, int radio)
{
    s_packed = ls_rsel_pack(s_packed, job, radio);
    return true;
}

ls_radio_err_t ls_radio_endpoint_get(const char *id, ls_radio_endpoint_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!strcmp(id, LS_RADIO_ENDPOINT_RTL_USB)) out->present = s_board.present[LS_RSEL_SDR_RTL];
    else if (!strcmp(id, LS_RADIO_ENDPOINT_HACKRF_USB)) out->present = s_board.present[LS_RSEL_SDR_HACKRF];
    else return LS_RADIO_ERR_UNAVAILABLE;
    return LS_RADIO_OK;
}

/* The screen the list was opened on, which a test moves. */
static int s_screen;
int ls_tui_screen_current(void) { return s_screen; }

/* An RTL-SDR on USB and an LR2021 in the socket; `chip` saved for paging
   puts the pager on the chip. */
static void fresh(bool chip)
{
    memset(&s_board, 0, sizeof(s_board));
    s_board.present[LS_RSEL_SDR_RTL] = true;
    s_board.present[LS_RSEL_LORA] = true;
    s_board.lora_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_MODES_RX;
    s_board.lora_lr20xx = true;
    s_board.lora_name = "LR2021";
    s_packed = LS_RSEL_PACKED_NONE;
    for (int j = 0; j < LS_RSEL_JOBS; j++) ls_rsel_track((ls_rsel_job_t)j, NULL);
    ls_rsel_forget();
    if (chip) ls_rsel_set(LS_RSEL_PAGER, LS_RSEL_LORA);
    s_screen = 3;
    ls_opt_close();
    ls_picker_close();
    ls_numpad_close();
    ls_keyboard_close();
}

/* -------------------------------------------------------------- settings -- */

static int s_baud;            /* index into BAUD           */
static int s_polarity;        /* index into POLARITY       */
static int s_alert;           /* 0 or 1                    */
static double s_squelch = 30;
static char s_name[24] = "base";
static int s_acts;
static bool s_open_list;      /* the action opens a list of its own */
static int s_sub_chosen = -1;
static bool s_blocked;

static const char *const BAUD[] = { "AUTO", "512", "1200", "2400" };
static const char *const POLARITY[] = { "AUTO", "NORMAL", "INVERTED" };

static int get_baud(const ls_opt_t *o) { (void)o; return s_baud; }
static void set_baud(const ls_opt_t *o, int v) { (void)o; s_baud = v; }
static int get_pol(const ls_opt_t *o) { (void)o; return s_polarity; }
static void set_pol(const ls_opt_t *o, int v) { (void)o; s_polarity = v; }
static int get_alert(const ls_opt_t *o) { (void)o; return s_alert; }
static void set_alert(const ls_opt_t *o, int v) { (void)o; s_alert = v; }
static double get_sql(const ls_opt_t *o) { (void)o; return s_squelch; }
static void set_sql(const ls_opt_t *o, double v) { (void)o; s_squelch = v; }
static const char *get_name(const ls_opt_t *o) { (void)o; return s_name; }
static void set_name(const ls_opt_t *o, const char *t) { (void)o; snprintf(s_name, sizeof(s_name), "%s", t); }
static void sub_done(int i) { s_sub_chosen = i; }
static void act(const ls_opt_t *o)
{
    (void)o;
    s_acts++;
    if (s_open_list) {
        ls_picker_open("ADDRESS", sub_done);
        ls_picker_add("ONE", "");
        ls_picker_add("TWO", "");
    }
}
static void act_show(const ls_opt_t *o, char *out, size_t n) { (void)o; snprintf(out, n, "%d done", s_acts); }
static const char *blocked(const ls_opt_t *o) { (void)o; return s_blocked ? "Stop WATCH first" : NULL; }

static const ls_opt_t ROWS[] = {
    { .label = "BAUD", .kind = LS_OPT_CYCLE, .radios = LS_OPT_SDR, .names = BAUD, .n = 4,
      .get = get_baud, .set = set_baud },
    { .label = "POLARITY", .kind = LS_OPT_CYCLE, .radios = LS_OPT_LORA, .names = POLARITY, .n = 3,
      .get = get_pol, .set = set_pol },
    { .label = "ALERT", .kind = LS_OPT_TOGGLE, .get = get_alert, .set = set_alert },
    { .label = "SQUELCH", .kind = LS_OPT_NUMBER, .radios = LS_OPT_SDR, .num = get_sql, .set_num = set_sql,
      .lo = 0, .hi = 100, .unit = "0 to 100" },
    { .label = "NAME", .kind = LS_OPT_TEXT, .text = get_name, .set_text = set_name, .max = 16 },
    { .label = "FILTER", .kind = LS_OPT_ACTION, .act = act, .show = act_show, .why_not = blocked },
};
static const ls_opt_ctx_t CTX = { .name = "POCSAG", .job = LS_RSEL_PAGER, .radio = LS_RSEL_NONE,
                                  LS_OPT_ROWS(ROWS) };

static const ls_opt_t CHIP_ONLY[] = {
    { .label = "POLARITY", .kind = LS_OPT_CYCLE, .radios = LS_OPT_LORA, .names = POLARITY, .n = 3,
      .get = get_pol, .set = set_pol },
};
static const ls_opt_ctx_t CTX_CHIP = { .name = "FLEX", .job = LS_RSEL_PAGER, .radio = LS_RSEL_NONE,
                                       LS_OPT_ROWS(CHIP_ONLY) };
static const ls_opt_ctx_t CTX_FIXED = { .name = "MESH", .job = -1, .radio = LS_RSEL_LORA,
                                        LS_OPT_ROWS(CHIP_ONLY), .tag = "RADIO" };

static void settings_reset(void)
{
    s_baud = 0; s_polarity = 0; s_alert = 0; s_squelch = 30;
    snprintf(s_name, sizeof(s_name), "base");
    s_acts = 0; s_open_list = false; s_sub_chosen = -1; s_blocked = false;
}

/* ------------------------------------------------------------- drawing -- */

#define W 52
#define H 70
static tui_cell g_back[W * H];
static tui_cell g_front[W * H];
static tui_surface g_sf;
static char g_text[H][W + 1];

static void draw_picker(void)
{
    tui_surface_setup(&g_sf, g_back, g_front, W, H);
    tui_frame_begin(&g_sf);
    ls_picker_draw(&g_sf, tui_rect_make(0, 0, W, H));
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) g_text[y][x] = g_back[y * W + x].ch;
        g_text[y][W] = 0;
    }
}

static int row_of(const char *label)
{
    for (int y = 0; y < H; y++) if (strstr(g_text[y], label)) return y;
    return -1;
}

static bool row_says(const char *label, const char *detail)
{
    const int y = row_of(label);
    return y >= 0 && strstr(g_text[y], detail) != NULL;
}

/* ENTER on the list's row `index`, from the top. */
static void choose(int index)
{
    for (int i = 0; i < 20; i++) ls_picker_key(LS_TK_UP, 0);
    for (int i = 0; i < index; i++) ls_picker_key(LS_TK_DOWN, 0);
    ls_picker_key(LS_TK_ENTER, 0);
}

/* ENTER where the cursor is: the list kept it on the row last chosen. */
static void again(void) { ls_picker_key(LS_TK_ENTER, 0); }

/* -------------------------------------------------------------- context -- */

LS_CASE(nothing_to_set_is_no_button)
{
    fresh(false);
    settings_reset();
    LS_EQ_INT(ls_opt_count(NULL), 0);
    /* Only the chip's polarity, and the pager is on the RTL-SDR. */
    LS_EQ_INT(ls_opt_count(&CTX_CHIP), 0);
    LS_CHECK(!ls_opt_key(&CTX_CHIP, 'o'));
    LS_CHECK(!ls_picker_active());
    ls_opt_open(&CTX_CHIP);
    LS_CHECK(!ls_picker_active());
    /* On the chip it has one. */
    fresh(true);
    LS_EQ_INT(ls_opt_count(&CTX_CHIP), 1);
}

LS_CASE(only_the_radio_in_use_has_rows)
{
    fresh(false);
    settings_reset();
    LS_EQ_INT(ls_opt_radio(&CTX), LS_RSEL_SDR_RTL);
    LS_EQ_INT(ls_opt_count(&CTX), 5);
    LS_CHECK(ls_opt_applies(&CTX, 0));
    LS_CHECK(!ls_opt_applies(&CTX, 1));
    ls_opt_open(&CTX);
    draw_picker();
    LS_CHECK(row_of("BAUD") >= 0);
    LS_CHECK(row_of("SQUELCH") >= 0);
    LS_CHECK(row_of("POLARITY") < 0);
    ls_picker_close();

    fresh(true);
    LS_EQ_INT(ls_opt_radio(&CTX), LS_RSEL_LORA);
    LS_EQ_INT(ls_opt_count(&CTX), 4);
    ls_opt_open(&CTX);
    draw_picker();
    LS_CHECK(row_of("POLARITY") >= 0);
    LS_CHECK(row_of("BAUD") < 0);
    LS_CHECK(row_of("SQUELCH") < 0);
    LS_CHECK(row_of("ALERT") >= 0);
    ls_picker_close();
}

LS_CASE(the_title_names_what_is_running_and_the_radio)
{
    fresh(false);
    settings_reset();
    ls_opt_open(&CTX);
    draw_picker();
    LS_CHECK(strstr(g_text[0], "POCSAG OPTIONS / RTL-SDR") != NULL);
    ls_picker_close();
    fresh(true);
    ls_opt_open(&CTX);
    draw_picker();
    LS_CHECK(strstr(g_text[0], "POCSAG OPTIONS / LR2021") != NULL);
    ls_picker_close();
    ls_opt_open(&CTX_FIXED);
    draw_picker();
    LS_CHECK(strstr(g_text[0], "MESH OPTIONS / LR2021") != NULL);
    ls_picker_close();
}

LS_CASE(the_button_is_options_on_o_with_the_context_under_it)
{
    fresh(false);
    const ls_btn_t b = ls_opt_button(&CTX);
    LS_EQ_STR(b.label, "OPTIONS");
    LS_EQ_STR(b.value, "POCSAG");
    LS_EQ_INT(b.key, 'o');
    LS_EQ_STR(ls_opt_button(&CTX_FIXED).value, "RADIO");
    LS_CHECK(!ls_opt_key(&CTX, 'r'));
    LS_CHECK(!ls_picker_active());
    LS_CHECK(ls_opt_key(&CTX, 'O'));
    LS_CHECK(ls_picker_active());
    ls_picker_close();
    LS_CHECK(ls_opt_key(&CTX, 'o'));
    LS_CHECK(ls_picker_active());
    ls_picker_close();
}

/* ---------------------------------------------------------------- kinds -- */

LS_CASE(each_row_shows_its_value)
{
    fresh(false);
    settings_reset();
    s_baud = 2;
    s_alert = 1;
    s_squelch = 45;
    ls_opt_open(&CTX);
    draw_picker();
    LS_CHECK(row_says("BAUD", "1200"));
    LS_CHECK(row_says("ALERT", "ON"));
    LS_CHECK(row_says("SQUELCH", "45"));
    LS_CHECK(row_says("NAME", "base"));
    LS_CHECK(row_says("FILTER", "0 done"));
    ls_picker_close();
    char v[40];
    ls_opt_value(&ROWS[3], v, sizeof(v));
    LS_EQ_STR(v, "45");
}

LS_CASE(a_choice_moves_in_place_and_the_list_stays_on_its_row)
{
    fresh(false);
    settings_reset();
    ls_opt_open(&CTX);
    choose(0);
    LS_EQ_INT(s_baud, 1);
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(row_says("BAUD", "512"));
    /* The cursor stayed: ENTER again moves the same setting on. */
    again(); again(); again();
    LS_EQ_INT(s_baud, 0);              /* wrapped past 2400 back to AUTO */
    draw_picker();
    LS_CHECK(row_says("BAUD", "AUTO"));
    /* And a second setting without leaving the list. */
    choose(1);
    LS_EQ_INT(s_alert, 1);
    again();
    LS_EQ_INT(s_alert, 0);
    LS_CHECK(ls_picker_active());
    LS_CHECK(ls_picker_key(LS_TK_ESC, 0));
    LS_CHECK(!ls_picker_active());
    ls_opt_poll();
    LS_CHECK(!ls_picker_active());
}

LS_CASE(a_number_is_typed_and_the_list_comes_back_with_it)
{
    fresh(false);
    settings_reset();
    ls_opt_open(&CTX);
    choose(2);                          /* SQUELCH, third row on the SDR */
    LS_CHECK(ls_numpad_active());
    LS_CHECK(!ls_picker_active());
    LS_CHECK(ls_opt_returning());
    ls_numpad_key(LS_TK_CHAR, '6');
    ls_numpad_key(LS_TK_CHAR, '2');
    ls_numpad_key(LS_TK_ENTER, 0);
    LS_CHECK(s_squelch == 62);
    LS_CHECK(ls_picker_active());
    LS_CHECK(!ls_opt_returning());
    draw_picker();
    LS_CHECK(row_says("SQUELCH", "62"));
    /* On the same row: ENTER is the keypad again. */
    again();
    LS_CHECK(ls_numpad_active());
    /* Out of range: the list says so and nothing changes. */
    ls_numpad_key(LS_TK_CHAR, '5');
    ls_numpad_key(LS_TK_CHAR, '0');
    ls_numpad_key(LS_TK_CHAR, '0');
    ls_numpad_key(LS_TK_ENTER, 0);
    LS_CHECK(s_squelch == 62);
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(row_of("SQUELCH: 0 to 100") >= 0);
    /* Cancelled: the list comes back on the next frame. */
    again();
    LS_CHECK(ls_numpad_active());
    ls_numpad_key(LS_TK_ESC, 0);
    LS_CHECK(!ls_picker_active());
    ls_opt_poll();
    LS_CHECK(ls_picker_active());
    again();
    LS_CHECK(ls_numpad_active());       /* still on SQUELCH */
    ls_numpad_close();
    ls_opt_close();
}

LS_CASE(words_are_typed_on_the_keyboard)
{
    fresh(false);
    settings_reset();
    ls_opt_open(&CTX);
    choose(3);                          /* NAME */
    LS_CHECK(ls_keyboard_active());
    ls_keyboard_key(LS_TK_CHAR, '2');
    ls_keyboard_key(LS_TK_ENTER, 0);
    LS_EQ_STR(s_name, "base2");
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(row_says("NAME", "base2"));
    ls_picker_close();
}

LS_CASE(a_list_of_the_apps_own_brings_options_back_when_it_closes)
{
    fresh(false);
    settings_reset();
    s_open_list = true;
    ls_opt_open(&CTX);
    choose(4);                          /* FILTER */
    LS_EQ_INT(s_acts, 1);
    LS_CHECK(ls_picker_active());
    LS_CHECK(ls_opt_returning());
    /* Up: nothing to bring back yet. */
    ls_opt_poll();
    draw_picker();
    LS_CHECK(strstr(g_text[0], "ADDRESS") != NULL);
    choose(1);
    LS_EQ_INT(s_sub_chosen, 1);
    ls_opt_poll();
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(strstr(g_text[0], "POCSAG OPTIONS") != NULL);
    LS_CHECK(row_says("FILTER", "1 done"));
    /* An action with nothing of its own comes straight back. */
    s_open_list = false;
    again();
    LS_EQ_INT(s_acts, 2);
    LS_CHECK(ls_picker_active());
    LS_CHECK(!ls_opt_returning());
    ls_picker_close();
}

LS_CASE(a_row_that_cannot_be_used_says_why_and_changes_nothing)
{
    fresh(false);
    settings_reset();
    s_blocked = true;
    ls_opt_open(&CTX);
    draw_picker();
    LS_CHECK(row_says("FILTER", "Stop WATCH first"));
    choose(4);
    LS_EQ_INT(s_acts, 0);
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(row_of("FILTER: Stop WATCH first") >= 0);
    ls_picker_close();
}

LS_CASE(values_follow_the_setting_while_the_list_is_up)
{
    fresh(false);
    settings_reset();
    ls_opt_open(&CTX);
    s_baud = 3;                         /* the receiver took a request */
    ls_opt_poll();
    draw_picker();
    LS_CHECK(row_says("BAUD", "2400"));
    ls_picker_close();
}

LS_CASE(the_list_does_not_come_back_over_another_app)
{
    fresh(false);
    settings_reset();
    ls_opt_open(&CTX);
    choose(2);
    LS_CHECK(ls_numpad_active());
    ls_numpad_key(LS_TK_ESC, 0);
    s_screen = 5;                       /* the console moved the screen */
    ls_opt_poll();
    LS_CHECK(!ls_picker_active());
    s_screen = 3;
    ls_opt_poll();
    LS_CHECK(!ls_picker_active());
}

LS_CASE(an_action_that_leaves_keeps_the_list_shut)
{
    static const ls_opt_t LEAVES[] = {
        { .label = "ALL SETTINGS", .kind = LS_OPT_ACTION, .act = act, .leaves = true },
    };
    static const ls_opt_ctx_t C = { .name = "P25", .job = -1, .radio = LS_RSEL_NONE, LS_OPT_ROWS(LEAVES) };
    fresh(false);
    settings_reset();
    ls_opt_open(&C);
    draw_picker();
    LS_CHECK(strstr(g_text[0], "P25 OPTIONS") != NULL);
    LS_CHECK(strstr(g_text[0], " / ") == NULL);
    choose(0);
    LS_EQ_INT(s_acts, 1);
    LS_CHECK(!ls_picker_active());
    ls_opt_poll();
    LS_CHECK(!ls_picker_active());
}

/* ------------------------------------------------------- menus, levels -- */

static double s_level = 50;
static double get_level(const ls_opt_t *o) { (void)o; return s_level; }
static void set_level(const ls_opt_t *o, double v) { (void)o; s_level = v; }

static const ls_opt_t LEVEL_ROWS[] = {
    { .label = "ALERT", .kind = LS_OPT_TOGGLE, .get = get_alert, .set = set_alert },
    { .label = "LEVEL", .kind = LS_OPT_LEVEL, .num = get_level, .set_num = set_level,
      .lo = 0, .hi = 100, .step = 5, .unit = "0 to 100" },
};
static const ls_opt_ctx_t CTX_LEVEL = { .name = "VOICE", .job = -1, .radio = LS_RSEL_NONE,
                                        LS_OPT_ROWS(LEVEL_ROWS) };
static const ls_opt_ctx_t CTX_EMPTY = { .name = "EMPTY", .job = LS_RSEL_PAGER, .radio = LS_RSEL_NONE,
                                        LS_OPT_ROWS(CHIP_ONLY) };
static const ls_opt_t TOP_ROWS[] = {
    { .label = "BAUD", .kind = LS_OPT_CYCLE, .names = BAUD, .n = 4, .get = get_baud, .set = set_baud },
    { .label = "VOICE", .kind = LS_OPT_MENU, .sub = &CTX_LEVEL },
    { .label = "CHIP ONLY", .kind = LS_OPT_MENU, .sub = &CTX_EMPTY },
};
static const ls_opt_ctx_t CTX_TOP = { .name = "MESH", .job = -1, .radio = LS_RSEL_NONE,
                                      LS_OPT_ROWS(TOP_ROWS) };

LS_CASE(a_menu_opens_its_own_list_and_back_returns_to_its_row)
{
    fresh(false);
    settings_reset();
    /* The menu whose rows are all for another radio is left out. */
    LS_EQ_INT(ls_opt_count(&CTX_TOP), 2);
    ls_opt_open(&CTX_TOP);
    draw_picker();
    LS_CHECK(row_says("VOICE", ">"));
    LS_CHECK(row_of("CHIP ONLY") < 0);
    LS_CHECK(row_of("BACK") < 0);

    choose(1);
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(row_of("MESH > VOICE") >= 0);
    LS_CHECK(row_of("< BACK") >= 0);
    LS_CHECK(row_of("LEVEL") >= 0);

    /* ESC goes back a level, on the row that opened it, then shuts. */
    ls_picker_key(LS_TK_ESC, 0);
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(row_of("MESH OPTIONS") >= 0);
    again();
    draw_picker();
    LS_CHECK(row_of("MESH > VOICE") >= 0);
    ls_picker_key(LS_TK_ESC, 0);
    ls_picker_key(LS_TK_ESC, 0);
    LS_CHECK(!ls_picker_active());
}

LS_CASE(a_level_steps_with_left_right_and_its_arrows_and_stays_in_range)
{
    fresh(false);
    settings_reset();
    s_level = 50;
    ls_opt_open(&CTX_LEVEL);
    for (int i = 0; i < 20; i++) ls_picker_key(LS_TK_UP, 0);
    ls_picker_key(LS_TK_DOWN, 0);
    ls_picker_key(LS_TK_RIGHT, 0);
    LS_CHECK(s_level == 55);
    ls_picker_key(LS_TK_LEFT, 0);
    ls_picker_key(LS_TK_LEFT, 0);
    LS_CHECK(s_level == 45);
    LS_CHECK(ls_picker_active());

    /* The drawn arrows do the same, and the list stays up. */
    draw_picker();
    const int y = row_of("LEVEL");
    LS_CHECK(y >= 0);
    const char *gt = strrchr(g_text[y], '>'), *lt = strrchr(g_text[y], '<');
    LS_CHECK(gt && lt);
    if (!gt || !lt) return;
    ls_picker_touch((int)(gt - g_text[y]), y);
    LS_CHECK(s_level == 50);
    ls_picker_touch((int)(lt - g_text[y]), y);
    LS_CHECK(s_level == 45);
    LS_CHECK(ls_picker_active());

    /* Held to its range. */
    s_level = 100;
    ls_picker_key(LS_TK_RIGHT, 0);
    LS_CHECK(s_level == 100);
    s_level = 0;
    ls_picker_key(LS_TK_LEFT, 0);
    LS_CHECK(s_level == 0);

    /* On a row that is not a level, LEFT and RIGHT still page. */
    ls_picker_key(LS_TK_UP, 0);
    s_alert = 0;
    ls_picker_key(LS_TK_RIGHT, 0);
    LS_EQ_INT(s_alert, 0);
    ls_picker_close();
}
