/* The board as ls_radio_select sees it, for the simulator and the screen
   tests: an RTL-SDR on USB-A, an SX1262 in the LoRa socket and the GPS - the
   bench a board usually sits on - and the keyboard board's radios once the
   simulated one has been probed (lssim_watch.c), as on the device.
   LSSIM_LORA=lr2021 puts the other chip in the socket; a test sets the
   board outright. The choices live here too, in RAM, where settings.c would
   keep them. */
#include <stdlib.h>
#include <string.h>

#include "ls_radio_select.h"
#include "ls_lora.h"
#include "ls_mixrf.h"

static ls_rsel_hw_t s_board;
static bool s_board_set, s_board_fixed;
static int s_choice[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
int lssim_rsel_restarts;
int lssim_rsel_writes;

static void board_default(void)
{
    memset(&s_board, 0, sizeof(s_board));
    s_board.present[LS_RSEL_SDR_RTL] = true;
    s_board.present[LS_RSEL_LORA] = true;
    s_board.present[LS_RSEL_GPS] = true;
    const char *chip = getenv("LSSIM_LORA");
    if (chip && !strcmp(chip, "lr2021")) {
        s_board.lora_lr20xx = true;
        s_board.lora_name = "LR2021";
        s_board.lora_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_OOK_RX |
                            LS_LORA_CAP_MODES_RX | LS_LORA_CAP_WIDE_RX_BW | LS_LORA_CAP_RSSI_INST;
    } else {
        s_board.lora_name = "SX126x";
        s_board.lora_caps = LS_LORA_CAP_LORA | LS_LORA_CAP_FSK | LS_LORA_CAP_OOK_RX |
                            LS_LORA_CAP_RSSI_INST;
    }
    s_board_set = true;
}

void lssim_rsel_board(const ls_rsel_hw_t *hw)
{
    if (hw) { s_board = *hw; s_board_set = true; }
    else board_default();
    s_board_fixed = hw != NULL;
    ls_rsel_forget();
}

void lssim_rsel_clear_choices(void)
{
    for (int i = 0; i < 16; i++) s_choice[i] = -1;
    lssim_rsel_writes = 0;
}

void ls_rsel_hw(ls_rsel_hw_t *out)
{
    if (!s_board_set) board_default();
    *out = s_board;
    if (s_board_fixed) return;
    ls_mixrf_status_t m;
    ls_mixrf_snapshot(&m);
    out->present[LS_RSEL_CC1101] = m.cc;
    out->present[LS_RSEL_NRF24] = m.nrf;
    out->present[LS_RSEL_NFC] = m.nfc;
    out->mixrf_unknown = !m.ready || m.busy;
}

void ls_rsel_hw_restart(void) { lssim_rsel_restarts++; }

int settings_get_radio_choice(int job)
{
    return job >= 0 && job < 16 ? s_choice[job] : -1;
}

bool settings_set_radio_choice(int job, int radio)
{
    if (job < 0 || job >= 16 || radio < -1 || radio > 14) return false;
    if (s_choice[job] != radio) lssim_rsel_writes++;
    s_choice[job] = radio;
    return true;
}

/* radio_choice.c's reader, on the same store. */
ls_rsel_radio_t ls_rsel_saved(ls_rsel_job_t job)
{
    const int r = settings_get_radio_choice((int)job);
    return r >= 0 && r < LS_RSEL_RADIOS ? (ls_rsel_radio_t)r : LS_RSEL_NONE;
}
