/* LS_TEST_SOURCES: ls_radio_select.c and radio_choice.c against a board the
   test sets, and the RADIO picker against its own drawn output */

#include "ls_test.h"

#include "ls_radio_select.h"
#include "radio_choice.h"
#include "ls_picker.h"
#include "ls_lora.h"
#include "radio_endpoint.h"
#include "tui_core.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- board -- */

static ls_rsel_hw_t s_board;
static int s_restarts;

void ls_rsel_hw(ls_rsel_hw_t *out) { *out = s_board; }
void ls_rsel_hw_restart(void) { s_restarts++; }

#define SX_CAPS (LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_OOK_RX | LS_LORA_CAP_RSSI_INST)
#define LR_CAPS (SX_CAPS | LS_LORA_CAP_MODES_RX | LS_LORA_CAP_WIDE_RX_BW | LS_LORA_CAP_FSK_STREAM)

/* The usual bench: an RTL-SDR, an SX1262 and the GPS. */
static void board(bool rtl, bool hackrf, int lora)   /* lora: 0 none, 1 SX1262, 2 LR2021 */
{
    memset(&s_board, 0, sizeof(s_board));
    s_board.present[LS_RSEL_SDR_RTL] = rtl;
    s_board.present[LS_RSEL_SDR_HACKRF] = hackrf;
    s_board.present[LS_RSEL_LORA] = lora != 0;
    s_board.present[LS_RSEL_GPS] = true;
    s_board.lora_caps = lora == 2 ? LR_CAPS : lora == 1 ? SX_CAPS : 0;
    s_board.lora_lr20xx = lora == 2;
    s_board.lora_name = lora == 2 ? "LR2021" : lora == 1 ? "SX126x" : "none";
    ls_rsel_forget();
}

/* ------------------------------------------------------------- settings -- */

/* The packed word settings.c keeps, through the same pack and unpack. */
static uint64_t s_packed = LS_RSEL_PACKED_NONE;
static int s_writes;

int settings_get_radio_choice(int job) { return ls_rsel_unpack(s_packed, job); }
bool settings_set_radio_choice(int job, int radio)
{
    const uint64_t now = ls_rsel_pack(s_packed, job, radio);
    if (now != s_packed) s_writes++;
    s_packed = now;
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

static void fresh(bool rtl, bool hackrf, int lora)
{
    s_packed = LS_RSEL_PACKED_NONE;
    s_writes = 0;
    s_restarts = 0;
    for (int j = 0; j < LS_RSEL_JOBS; j++) ls_rsel_track((ls_rsel_job_t)j, NULL);
    board(rtl, hackrf, lora);
}

/* ---------------------------------------------------------------- names -- */

LS_CASE(the_lora_socket_is_named_for_the_chip_in_it)
{
    fresh(true, false, 1);
    LS_EQ_STR(ls_rsel_name(LS_RSEL_LORA), "SX1262");
    fresh(true, false, 2);
    LS_EQ_STR(ls_rsel_name(LS_RSEL_LORA), "LR2021");
    fresh(true, false, 0);
    LS_EQ_STR(ls_rsel_name(LS_RSEL_LORA), "LoRa");
}

LS_CASE(every_other_radio_has_its_device_name)
{
    fresh(true, false, 1);
    LS_EQ_STR(ls_rsel_name(LS_RSEL_SDR_RTL), "RTL-SDR");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_SDR_HACKRF), "HackRF");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_CC1101), "CC1101");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_NRF24), "nRF24");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_NFC), "NFC");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_WIFI), "Wi-Fi");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_BLE), "Bluetooth");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_GPS), "GPS");
    LS_EQ_STR(ls_rsel_name(LS_RSEL_NONE), "none");
}

/* ------------------------------------------------------- what and why -- */

LS_CASE(a_missing_radio_says_why_and_a_fitted_one_says_nothing)
{
    fresh(true, false, 0);
    LS_CHECK(ls_rsel_absent(LS_RSEL_SDR_RTL) == NULL);
    LS_EQ_STR(ls_rsel_absent(LS_RSEL_SDR_HACKRF), "Plug a HackRF into the USB-A port");
    LS_EQ_STR(ls_rsel_absent(LS_RSEL_LORA), "No LoRa chip answered at boot");
    LS_EQ_STR(ls_rsel_absent(LS_RSEL_CC1101), "Needs the T-MixRF keyboard board");
    LS_CHECK(ls_rsel_absent(LS_RSEL_GPS) == NULL);
}

LS_CASE(the_keyboard_radios_count_as_there_until_the_board_is_probed)
{
    fresh(true, false, 1);
    s_board.mixrf_unknown = true;
    s_board.mixrf_seen = true;
    ls_rsel_forget();
    LS_CHECK(ls_rsel_absent(LS_RSEL_CC1101) == NULL);
    LS_CHECK(ls_rsel_absent(LS_RSEL_NFC) == NULL);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_MIXRF), LS_RSEL_CC1101);
    s_board.mixrf_unknown = false;
    ls_rsel_forget();
    LS_CHECK(ls_rsel_absent(LS_RSEL_CC1101) != NULL);
}

LS_CASE(p25_on_the_lora_chip_is_the_lr2021s)
{
    fresh(false, false, 1);
    LS_CHECK(ls_rsel_cant(LS_RSEL_P25, LS_RSEL_LORA) != NULL);
    fresh(false, false, 2);
    LS_CHECK(ls_rsel_cant(LS_RSEL_P25, LS_RSEL_LORA) == NULL);
    /* No dongle: P25 goes to the LR2021. A dongle comes first. */
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_LORA);
    fresh(true, false, 2);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_SDR_RTL);
    ls_rsel_set(LS_RSEL_P25, LS_RSEL_LORA);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_LORA);
}

LS_CASE(retired_fsk_split_is_unavailable_even_with_both_radios)
{
    fresh(true, false, 2);
    LS_EQ_STR(ls_rsel_name(LS_RSEL_P25_SPLIT), "split");
    LS_CHECK(ls_rsel_absent(LS_RSEL_P25_SPLIT) != NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_P25, LS_RSEL_P25_SPLIT) != NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_FM, LS_RSEL_P25_SPLIT) != NULL);
    fresh(true, false, 1);
    LS_CHECK(ls_rsel_absent(LS_RSEL_P25_SPLIT) != NULL);
    fresh(false, true, 2);
    LS_CHECK(ls_rsel_absent(LS_RSEL_P25_SPLIT) != NULL);
}

LS_CASE(mode_s_is_the_lr2021s_and_not_the_sx1262s)
{
    fresh(false, false, 1);
    LS_CHECK(ls_rsel_cant(LS_RSEL_ADSB, LS_RSEL_LORA) != NULL);
    fresh(false, false, 2);
    LS_CHECK(ls_rsel_cant(LS_RSEL_ADSB, LS_RSEL_LORA) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_ADSB, LS_RSEL_SDR_RTL) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_ADSB, LS_RSEL_CC1101) != NULL);
}

LS_CASE(each_job_takes_only_the_radios_that_can_run_it)
{
    fresh(true, true, 1);
    /* Audio wants an IQ stream; P25 one, or the LR2021's bit stream. */
    LS_CHECK(ls_rsel_cant(LS_RSEL_FM, LS_RSEL_SDR_HACKRF) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_FM, LS_RSEL_LORA) != NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_P25, LS_RSEL_LORA) != NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_ACARS, LS_RSEL_CC1101) != NULL);
    /* POCSAG is two-level FSK, which the LoRa chip demodulates. */
    LS_CHECK(ls_rsel_cant(LS_RSEL_PAGER, LS_RSEL_LORA) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_PAGER, LS_RSEL_CC1101) != NULL);
    /* SUB-GHZ READ captures from either SDR, the CC1101 or the LoRa chip. */
    LS_CHECK(ls_rsel_cant(LS_RSEL_SUBGHZ_READ, LS_RSEL_SDR_RTL) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_SUBGHZ_READ, LS_RSEL_CC1101) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_SUBGHZ_READ, LS_RSEL_LORA) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_SUBGHZ_READ, LS_RSEL_SDR_HACKRF) == NULL);
    /* ANALYZER sweeps with the LoRa chip and only that. */
    LS_CHECK(ls_rsel_cant(LS_RSEL_SUBGHZ_SWEEP, LS_RSEL_LORA) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_SUBGHZ_SWEEP, LS_RSEL_SDR_RTL) != NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_LABS, LS_RSEL_SDR_RTL) != NULL);
    /* FIND aims everything but the two that cannot give a bearing. */
    LS_CHECK(ls_rsel_cant(LS_RSEL_FIND, LS_RSEL_BLE) == NULL);
    LS_EQ_STR(ls_rsel_cant(LS_RSEL_FIND, LS_RSEL_NFC), "NFC reaches a few centimetres: hold the board to the tag");
    LS_CHECK(ls_rsel_cant(LS_RSEL_FIND2, LS_RSEL_GPS) != NULL);
    /* Everything records something and shows something. */
    for (int r = 0; r < LS_RSEL_P25_SPLIT; r++) {
        LS_CHECK(ls_rsel_cant(LS_RSEL_REC, (ls_rsel_radio_t)r) == NULL);
        LS_CHECK(ls_rsel_cant(LS_RSEL_WATERFALL, (ls_rsel_radio_t)r) == NULL);
    }
    LS_CHECK(ls_rsel_cant(LS_RSEL_MIXRF, LS_RSEL_NRF24) == NULL);
    LS_CHECK(ls_rsel_cant(LS_RSEL_MIXRF, LS_RSEL_LORA) != NULL);
}

/* -------------------------------------------------- defaults, fallback -- */

LS_CASE(with_nothing_saved_each_job_takes_the_first_radio_that_can)
{
    fresh(true, true, 2);
    LS_EQ_INT(ls_rsel_get(LS_RSEL_ADSB), LS_RSEL_SDR_RTL);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_ADSB), LS_RSEL_SDR_RTL);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_SUBGHZ_SWEEP), LS_RSEL_LORA);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_SUBGHZ_READ), LS_RSEL_SDR_RTL);
    fresh(false, false, 2);
    /* No dongle: ADS-B goes to the LR2021, and so does a pager. */
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_ADSB), LS_RSEL_LORA);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_PAGER), LS_RSEL_LORA);
    fresh(false, true, 1);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_FM), LS_RSEL_SDR_HACKRF);
    /* Nothing can: the job's first radio, so the button still has a name. */
    fresh(false, false, 1);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_ADSB), LS_RSEL_SDR_RTL);
    LS_CHECK(s_writes == 0);
}

LS_CASE(a_saved_radio_that_is_gone_falls_back_without_being_forgotten)
{
    fresh(true, true, 1);
    ls_rsel_set(LS_RSEL_P25, LS_RSEL_SDR_HACKRF);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_SDR_HACKRF);
    board(true, false, 1);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_SDR_RTL);
    LS_EQ_INT(ls_rsel_get(LS_RSEL_P25), LS_RSEL_SDR_HACKRF);
    board(true, true, 1);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_SDR_HACKRF);
    /* A saved choice that cannot do the job is passed over the same way. */
    ls_rsel_set(LS_RSEL_ADSB, LS_RSEL_LORA);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_ADSB), LS_RSEL_SDR_RTL);
}

LS_CASE(a_choice_survives_in_the_packed_word_and_repeats_are_not_writes)
{
    fresh(true, true, 1);
    ls_rsel_set(LS_RSEL_FM, LS_RSEL_SDR_HACKRF);
    ls_rsel_set(LS_RSEL_MIXRF, LS_RSEL_NFC);
    LS_EQ_INT(s_writes, 2);
    ls_rsel_set(LS_RSEL_FM, LS_RSEL_SDR_HACKRF);
    LS_EQ_INT(s_writes, 2);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_FM), LS_RSEL_SDR_HACKRF);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_MIXRF), LS_RSEL_NFC);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_P25), LS_RSEL_NONE);
    /* Jobs do not leak into their neighbours. */
    uint64_t w = LS_RSEL_PACKED_NONE;
    for (int j = 0; j < 16; j++) w = ls_rsel_pack(w, j, j % 15);
    for (int j = 0; j < 16; j++) LS_EQ_INT(ls_rsel_unpack(w, j), j % 15);
    w = ls_rsel_pack(w, 3, -1);
    LS_EQ_INT(ls_rsel_unpack(w, 3), -1);
    LS_EQ_INT(ls_rsel_unpack(w, 4), 4);
    LS_EQ_UINT(ls_rsel_pack(w, 16, 1), w);
    LS_EQ_UINT(ls_rsel_pack(w, 2, 15), w);
}

LS_CASE(a_receiver_asks_for_the_dongle_the_picker_shows)
{
    fresh(true, true, 1);
    LS_EQ_STR(ls_rsel_sdr_endpoint(LS_RSEL_FM), LS_RADIO_ENDPOINT_RTL_USB);
    ls_rsel_set(LS_RSEL_FM, LS_RSEL_SDR_HACKRF);
    LS_EQ_STR(ls_rsel_sdr_endpoint(LS_RSEL_FM), LS_RADIO_ENDPOINT_HACKRF_USB);
    board(true, false, 1);
    LS_EQ_STR(ls_rsel_sdr_endpoint(LS_RSEL_FM), LS_RADIO_ENDPOINT_RTL_USB);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_FM), LS_RSEL_SDR_RTL);
    board(false, false, 1);
    LS_CHECK(ls_rsel_sdr_endpoint(LS_RSEL_FM) == NULL);
}

static ls_rsel_radio_t journal_in_use(void) { return LS_RSEL_BLE; }
static ls_rsel_radio_t nothing_in_use(void) { return LS_RSEL_NONE; }

LS_CASE(an_app_that_knows_its_radio_is_believed)
{
    fresh(true, false, 1);
    ls_rsel_track(LS_RSEL_JOURNAL, journal_in_use);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_JOURNAL), LS_RSEL_BLE);
    const ls_btn_t b = ls_rsel_button(LS_RSEL_JOURNAL);
    LS_EQ_STR(b.label, "RADIO");
    LS_EQ_STR(b.value, "Bluetooth");
    LS_EQ_INT(b.key, 'r');
    ls_rsel_track(LS_RSEL_JOURNAL, nothing_in_use);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_JOURNAL), LS_RSEL_GPS);
}

/* --------------------------------------------------------------- picker -- */

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

/* The row that carries `label`, or -1. */
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

static ls_rsel_radio_t s_chosen;
static int s_done_calls;
static void on_done(ls_rsel_radio_t r) { s_chosen = r; s_done_calls++; }

/* The row a radio sits on: its number, less the radios before it that the
   list leaves out because they have not been plugged in (dongles) or the
   keyboard board has not been on (CC1101, nRF24, NFC). */
static int row_for(ls_rsel_radio_t radio)
{
    int row = (int)radio;
    for (int r = 0; r < (int)radio; r++) {
        bool hidden = false;
        if (r == LS_RSEL_SDR_RTL) hidden = !s_board.present[r] && !s_board.sdr_seen[0];
        if (r == LS_RSEL_SDR_HACKRF) hidden = !s_board.present[r] && !s_board.sdr_seen[1];
        if (r == LS_RSEL_CC1101 || r == LS_RSEL_NRF24 || r == LS_RSEL_NFC)
            hidden = !s_board.present[r] && !s_board.mixrf_seen;
        if (hidden) row--;
    }
    return row;
}

static void choose(int index)
{
    for (int i = 0; i < 20; i++) ls_picker_key(LS_TK_UP, 0);
    for (int i = 0; i < index; i++) ls_picker_key(LS_TK_DOWN, 0);
    ls_picker_key(LS_TK_ENTER, 0);
}

LS_CASE(the_picker_lists_every_radio_with_its_state)
{
    fresh(true, false, 1);
    s_done_calls = 0;
    ls_rsel_open(LS_RSEL_ADSB, on_done);
    LS_CHECK(ls_picker_active());
    draw_picker();
    LS_CHECK(strstr(g_text[0], "RADIO") != NULL);
    LS_CHECK(row_says("RTL-SDR", "selected"));
    /* A HackRF that was never plugged in is not offered. */
    LS_CHECK(row_of("HackRF") < 0);
    LS_CHECK(row_says("SX1262", "No Mode S"));
    /* In enum order, so every app's list reads the same. */
    LS_CHECK(row_of("RTL-SDR") < row_of("SX1262"));
    ls_picker_close();
}

LS_CASE(choosing_a_radio_that_cannot_keeps_the_list_and_the_choice)
{
    fresh(true, false, 1);
    s_done_calls = 0;
    ls_rsel_open(LS_RSEL_ADSB, on_done);
    choose(row_for(LS_RSEL_LORA));
    LS_EQ_INT(s_done_calls, 0);
    LS_CHECK(ls_picker_active());
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_ADSB), LS_RSEL_NONE);
    draw_picker();
    LS_CHECK(row_of("SX1262: No Mode S") >= 0);
    /* The cursor stays on the row that was refused. */
    ls_picker_key(LS_TK_ENTER, 0);
    LS_EQ_INT(s_done_calls, 0);
    LS_CHECK(ls_picker_active());
    /* Then a radio that can. */
    choose(LS_RSEL_SDR_RTL);
    LS_EQ_INT(s_done_calls, 1);
    LS_EQ_INT(s_chosen, LS_RSEL_SDR_RTL);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_ADSB), LS_RSEL_SDR_RTL);
    LS_CHECK(!ls_picker_active());
}

LS_CASE(the_lr2021_is_ready_for_ads_b_in_the_same_list)
{
    fresh(false, false, 2);
    s_done_calls = 0;
    ls_rsel_open(LS_RSEL_ADSB, on_done);
    draw_picker();
    LS_CHECK(row_says("LR2021", "selected"));
    LS_CHECK(row_of("RTL-SDR") < 0);
    choose(row_for(LS_RSEL_LORA));
    LS_EQ_INT(s_done_calls, 1);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_ADSB), LS_RSEL_LORA);
}

LS_CASE(dual_aircraft_picker_requires_both_rtl_and_lr2021)
{
    fresh(true, false, 2);
    LS_CHECK(ls_rsel_absent(LS_RSEL_ADSB_DUAL) == NULL);
    ls_rsel_open(LS_RSEL_ADSB, on_done);
    draw_picker();
    LS_CHECK(row_of("1090 LR + 978 RTL") >= 0);
    choose(row_for(LS_RSEL_ADSB_DUAL));
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_ADSB), LS_RSEL_ADSB_DUAL);
    fresh(true, false, 1);
    LS_CHECK(ls_rsel_absent(LS_RSEL_ADSB_DUAL) != NULL);
    ls_rsel_open(LS_RSEL_ADSB, on_done);
    draw_picker();
    LS_CHECK(row_of("1090 LR + 978 RTL") < 0);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_ADSB), LS_RSEL_SDR_RTL);
    ls_picker_close();
}

LS_CASE(lr_fm_picker_range_modes_and_no_dongle)
{
    fresh(false, false, 2);
    ls_rsel_fm_context(162550000, true);
    LS_CHECK(ls_rsel_cant(LS_RSEL_FM, LS_RSEL_LORA) == NULL);
    LS_EQ_INT(ls_rsel_resolved(LS_RSEL_FM), LS_RSEL_LORA);
    board(true, false, 2);
    ls_rsel_set(LS_RSEL_FM, LS_RSEL_LORA);
    LS_EQ_INT(ls_rsel_resolved(LS_RSEL_FM), LS_RSEL_LORA);
    ls_rsel_fm_context(144000000, true);
    LS_CHECK(ls_rsel_cant(LS_RSEL_FM, LS_RSEL_LORA) == NULL);   /* 2 m: inside 104-2740 */
    ls_rsel_fm_context(91000000, true);
    const char *why = ls_rsel_cant(LS_RSEL_FM, LS_RSEL_LORA);
    LS_CHECK(why && strstr(why, "104-2740") != NULL);
    LS_EQ_INT(ls_rsel_resolved(LS_RSEL_FM), LS_RSEL_SDR_RTL);
    ls_rsel_fm_context(91000000, false);
    LS_CHECK(ls_rsel_cant(LS_RSEL_FM, LS_RSEL_LORA) != NULL);
    ls_rsel_fm_context(162550000, false);
    LS_CHECK(ls_rsel_cant(LS_RSEL_FM, LS_RSEL_LORA) != NULL);
    ls_rsel_fm_context(154785000, true);
}

/* ----------------------------------------------- dongles nobody plugged -- */

LS_CASE(an_lr2021_board_with_no_dongle_lists_no_dongle)
{
    fresh(false, false, 2);
    ls_rsel_open(LS_RSEL_P25, on_done);
    draw_picker();
    LS_CHECK(row_of("RTL-SDR") < 0);
    LS_CHECK(row_of("HackRF") < 0);
    LS_CHECK(row_says("LR2021", "selected"));
    /* What else the board has still says why it cannot. */
    /* The keyboard board's radios are not listed either, with it not on. */
    LS_CHECK(row_of("CC1101") < 0);
    LS_CHECK(row_of("nRF24") < 0);
    LS_CHECK(row_of("NFC") < 0);
    LS_CHECK(row_of("GPS") >= 0);
    LS_CHECK(row_of("Wi-Fi") >= 0);
    LS_CHECK(row_of("Bluetooth") >= 0);
    LS_CHECK(row_of("split") >= 0);
    /* The first row is the LR2021, and it is what a tap picks. */
    s_done_calls = 0;
    choose(0);
    LS_EQ_INT(s_done_calls, 1);
    LS_EQ_INT(s_chosen, LS_RSEL_LORA);
}

LS_CASE(a_plugged_dongle_is_listed_and_a_tap_picks_it)
{
    fresh(true, true, 2);
    ls_rsel_open(LS_RSEL_P25, on_done);
    draw_picker();
    LS_CHECK(row_says("RTL-SDR", "selected"));
    LS_CHECK(row_says("HackRF", "ready"));
    ls_picker_close();
    /* Only the HackRF: the RTL-SDR row goes, the HackRF is the first row. */
    fresh(false, true, 2);
    ls_rsel_open(LS_RSEL_P25, on_done);
    draw_picker();
    LS_CHECK(row_of("RTL-SDR") < 0);
    LS_CHECK(row_says("HackRF", "selected"));
    s_done_calls = 0;
    choose(0);
    LS_EQ_INT(s_chosen, LS_RSEL_SDR_HACKRF);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_P25), LS_RSEL_SDR_HACKRF);
}

LS_CASE(a_dongle_that_dropped_off_stays_listed_while_it_reconnects)
{
    fresh(false, false, 2);
    s_board.sdr_seen[0] = true;
    ls_rsel_forget();
    ls_rsel_open(LS_RSEL_P25, on_done);
    draw_picker();
    LS_CHECK(row_says("RTL-SDR", "Plug an RTL-SDR"));
    LS_CHECK(row_of("HackRF") < 0);
    ls_picker_close();
    /* Back on the bus: listed as ready. */
    s_board.present[LS_RSEL_SDR_RTL] = true;
    ls_rsel_forget();
    ls_rsel_open(LS_RSEL_P25, on_done);
    draw_picker();
    LS_CHECK(row_says("RTL-SDR", "selected"));
    ls_picker_close();
}

LS_CASE(a_saved_absent_dongle_falls_back_without_a_write)
{
    fresh(true, false, 2);
    ls_rsel_set(LS_RSEL_P25, LS_RSEL_SDR_RTL);
    LS_EQ_INT(s_writes, 1);
    /* Rebooted with no dongle plugged in. */
    board(false, false, 2);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_LORA);
    LS_EQ_INT(ls_rsel_resolved(LS_RSEL_P25), LS_RSEL_LORA);
    ls_rsel_open(LS_RSEL_P25, on_done);
    draw_picker();
    LS_CHECK(row_of("RTL-SDR") < 0);
    LS_CHECK(row_says("LR2021", "selected"));
    ls_picker_close();
    LS_EQ_INT(s_writes, 1);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_P25), LS_RSEL_SDR_RTL);
    /* The dongle comes back and is the choice again. */
    board(true, false, 2);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_P25), LS_RSEL_SDR_RTL);
}

LS_CASE(a_job_only_a_dongle_can_do_keeps_the_dongle_rows)
{
    /* ACARS has no receiver on an SX1262 board: the dongle row saying what
       to plug in is the one sentence that helps. */
    fresh(false, false, 1);
    ls_rsel_open(LS_RSEL_ACARS, on_done);
    draw_picker();
    LS_CHECK(row_says("RTL-SDR", "Plug an RTL-SDR"));
    LS_CHECK(row_says("HackRF", "Plug a HackRF"));
    ls_picker_close();
    /* The LR2021 tunes the ACARS band itself since its range was measured. */
    fresh(false, false, 2);
    LS_CHECK(ls_rsel_cant(LS_RSEL_ACARS, LS_RSEL_LORA) == NULL);
}

LS_CASE(the_fixed_radio_picker_hides_absent_dongles_too)
{
    fresh(false, false, 2);
    ls_rsel_open_fixed(LS_RSEL_LORA, on_done);
    draw_picker();
    LS_CHECK(row_of("RTL-SDR") < 0);
    LS_CHECK(row_of("HackRF") < 0);
    LS_CHECK(row_says("LR2021", "protocol radio"));
    s_done_calls = 0;
    choose(0);
    LS_EQ_INT(s_done_calls, 1);
    LS_EQ_INT(s_chosen, LS_RSEL_LORA);
    /* With a dongle in, it is listed and refused as before. */
    fresh(true, false, 2);
    ls_rsel_open_fixed(LS_RSEL_LORA, on_done);
    draw_picker();
    LS_CHECK(row_says("RTL-SDR", "not used by this protocol"));
    ls_picker_close();
}

/* ------------------------------------------ the keyboard board's radios -- */

LS_CASE(the_keyboard_board_radios_appear_when_it_is_attached)
{
    fresh(false, false, 2);
    ls_rsel_open(LS_RSEL_REC, on_done);
    draw_picker();
    LS_CHECK(row_of("CC1101") < 0 && row_of("nRF24") < 0 && row_of("NFC") < 0);
    ls_picker_close();
    /* The keyboard goes on: not probed yet, so all three are there. */
    s_board.mixrf_seen = true;
    s_board.mixrf_unknown = true;
    ls_rsel_forget();
    ls_rsel_open(LS_RSEL_REC, on_done);
    draw_picker();
    LS_CHECK(row_of("CC1101") >= 0 && row_of("nRF24") >= 0 && row_of("NFC") >= 0);
    ls_picker_close();
    /* Probed: the parts that answered are ready, the rest say why. */
    s_board.mixrf_unknown = false;
    s_board.present[LS_RSEL_CC1101] = true;
    ls_rsel_forget();
    ls_rsel_open(LS_RSEL_REC, on_done);
    draw_picker();
    LS_CHECK(row_says("CC1101", "selected"));
    LS_CHECK(row_says("nRF24", "Needs the T-MixRF"));
    ls_picker_close();
}

LS_CASE(a_keyboard_that_was_attached_keeps_its_rows_while_it_comes_back)
{
    fresh(false, false, 2);
    s_board.mixrf_seen = true;
    ls_rsel_forget();
    ls_rsel_open(LS_RSEL_REC, on_done);
    draw_picker();
    LS_CHECK(row_says("CC1101", "Needs the T-MixRF"));
    ls_picker_close();
}

LS_CASE(a_saved_mixrf_radio_with_no_keyboard_falls_back_without_a_write)
{
    fresh(false, false, 2);
    s_board.mixrf_seen = true;
    s_board.present[LS_RSEL_CC1101] = true;
    ls_rsel_forget();
    ls_rsel_set(LS_RSEL_SUBGHZ_READ, LS_RSEL_CC1101);
    LS_EQ_INT(s_writes, 1);
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_SUBGHZ_READ), LS_RSEL_CC1101);
    /* Rebooted without the keyboard. */
    board(false, false, 2);
    s_board.mixrf_unknown = true;
    ls_rsel_forget();
    LS_EQ_INT(ls_rsel_effective(LS_RSEL_SUBGHZ_READ), LS_RSEL_LORA);
    ls_rsel_open(LS_RSEL_SUBGHZ_READ, on_done);
    draw_picker();
    LS_CHECK(row_of("CC1101") < 0);
    LS_CHECK(row_says("LR2021", "selected"));
    ls_picker_close();
    LS_EQ_INT(s_writes, 1);
    LS_EQ_INT(ls_rsel_saved(LS_RSEL_SUBGHZ_READ), LS_RSEL_CC1101);
}

LS_CASE(the_mixrf_job_keeps_its_rows_and_their_reason_with_no_keyboard)
{
    fresh(false, false, 2);
    ls_rsel_open(LS_RSEL_MIXRF, on_done);
    draw_picker();
    LS_CHECK(row_says("CC1101", "Needs the T-MixRF"));
    LS_CHECK(row_says("nRF24", "Needs the T-MixRF"));
    LS_CHECK(row_says("NFC", "Needs the T-MixRF"));
    ls_picker_close();
}

LS_CASE(wifi_bluetooth_gps_and_split_stay_whatever_the_keyboard_does)
{
    fresh(false, false, 2);
    ls_rsel_open(LS_RSEL_REC, on_done);
    draw_picker();
    LS_CHECK(row_of("Wi-Fi") >= 0 && row_of("Bluetooth") >= 0);
    LS_CHECK(row_of("GPS") >= 0 && row_of("split") >= 0);
    ls_picker_close();
}
