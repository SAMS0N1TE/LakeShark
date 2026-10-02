/* RADIO: which radio an app uses, chosen the same way in every app.

   Every app that touches a radio carries one RADIO button on 'r', with the
   radio's real name on it, and that button opens one list: every radio the
   board knows, in the same order, each saying whether it is the one in use,
   ready, or why not. A radio that is missing or cannot do the job stays in
   the list with its reason rather than being left out, and choosing it
   changes nothing.

   The choice is per job (radio_choice.h) and survives a reboot. What runs is
   the saved radio when it is fitted and can do the job, otherwise the first
   one in the job's own order that can - ls_rsel_effective - and the button,
   the list and the receiver all ask that one question. */

#ifndef LS_RADIO_SELECT_H
#define LS_RADIO_SELECT_H

#include <stdbool.h>
#include <stdint.h>

#include "radio_choice.h"
#include "ls_tui_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The device's own name: "RTL-SDR", "HackRF", the LoRa socket's chip
   ("SX1262", "LR2021"; "LoRa" when none answered), "CC1101", "nRF24", "NFC",
   "Wi-Fi", "Bluetooth", "GPS". Never NULL. */
const char *ls_rsel_name(ls_rsel_radio_t r);

/* NULL when the radio is fitted and answering, else why it is not. */
const char *ls_rsel_absent(ls_rsel_radio_t r);

/* NULL when the radio can do the job in this firmware, else why not. Says
   nothing about whether it is fitted: that is ls_rsel_absent. */
const char *ls_rsel_cant(ls_rsel_job_t job, ls_rsel_radio_t r);

/* The saved choice, or the job's default when nothing is saved. */
ls_rsel_radio_t ls_rsel_get(ls_rsel_job_t job);
/* Save a choice. Only a change reaches the settings write queue. */
void ls_rsel_set(ls_rsel_job_t job, ls_rsel_radio_t r);

/* What will actually be used now: the saved choice when it is fitted and
   can do the job, otherwise the first that can in the job's order. When
   none can, the job's first radio, so there is always a name to show. */
ls_rsel_radio_t ls_rsel_effective(ls_rsel_job_t job);

/* What the choice comes to on this board now, without asking a tracker:
   the saved radio when it can be used, otherwise the fallback. For an app
   deciding what to start, where ls_rsel_effective would only repeat what is
   already running. */
ls_rsel_radio_t ls_rsel_resolved(ls_rsel_job_t job);

/* For an app whose own state already says which radio is in use - a slot
   that is running, a capture source - so the button and the list cannot
   disagree with it. While set, ls_rsel_effective returns what it returns,
   unless that is LS_RSEL_NONE. NULL removes it. */
void ls_rsel_track(ls_rsel_job_t job, ls_rsel_radio_t (*in_use)(void));

/* The one picker, titled RADIO. `done` is called with the radio chosen, and
   only when it can be used. Choosing one that cannot leaves the list open
   with the reason written above it, and changes nothing. */
void ls_rsel_open(ls_rsel_job_t job, void (*done)(ls_rsel_radio_t chosen));
/* The same picker with rows of the app's own below the radios, for a choice
   that is not a radio (REC's mesh traffic log). `extra_done` gets the row's
   index among the extras; the saved radio is left alone. */
typedef struct { const char *label, *detail; } ls_rsel_extra_t;
void ls_rsel_open_with(ls_rsel_job_t job, void (*done)(ls_rsel_radio_t chosen),
                       const ls_rsel_extra_t *extra, int n, void (*extra_done)(int row));

/* The SDR a receiver holds right now, by the owner name it acquired with
   ("fm", "p25", "adsb"), or LS_RSEL_NONE. What a tracker answers for a
   receiver: the dongle it really has, not the one it was asked for. */
ls_rsel_radio_t ls_rsel_sdr_held_by(const char *owner);

/* { "RADIO", <name of the effective radio>, 'r' }. */
ls_btn_t ls_rsel_button(ls_rsel_job_t job);

/* Ask the running SDR receiver to start again, so it takes the endpoint now
   chosen. Does nothing when no SDR receiver is running. */
void ls_rsel_restart_sdr(void);

/* What the board answers when ls_radio_select asks. ls_radio_select_hw.c on
   the device; the bench has its own, which it can set. */
typedef struct {
    bool present[LS_RSEL_RADIOS];
    uint32_t lora_caps;          /* ls_lora_caps()                  */
    bool lora_lr20xx;            /* the socket holds an LR20xx part */
    const char *lora_name;       /* ls_lora_chip_name()             */
    char sdr_owner[2][16];       /* who holds the RTL-SDR, the HackRF; "" for nobody */
    bool mixrf_unknown;          /* the keyboard board not probed yet, or probing */
} ls_rsel_hw_t;
void ls_rsel_hw(ls_rsel_hw_t *out);
void ls_rsel_hw_restart(void);
/* Drop what ls_rsel remembers about the board, so the next question asks it
   again. For a test that changes the board under it. */
void ls_rsel_forget(void);

#ifdef __cplusplus
}
#endif

#endif /* LS_RADIO_SELECT_H */
