/* Which radio each job runs on, as stored.

   The RADIO picker in the TUI (apps/tui/ls_radio_select.h) is where a choice
   is made and explained; the receivers read it here, so a back end and the
   button above it name the same radio. Both numbers are stored, four bits a
   job, so both lists are append-only. */

#ifndef RADIO_CHOICE_H
#define RADIO_CHOICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LS_RSEL_SDR_RTL,
    LS_RSEL_SDR_HACKRF,
    LS_RSEL_LORA,          /* whichever chip is in the LoRa socket */
    LS_RSEL_CC1101,
    LS_RSEL_NRF24,
    LS_RSEL_NFC,
    LS_RSEL_WIFI,
    LS_RSEL_BLE,
    LS_RSEL_GPS,
    LS_RSEL_RADIOS
} ls_rsel_radio_t;

/* No radio: nothing saved, or nothing that can do the job. */
#define LS_RSEL_NONE LS_RSEL_RADIOS

/* One per thing an app does with a radio. */
typedef enum {
    LS_RSEL_FM,            /* FM, WFM and AM audio                    */
    LS_RSEL_PAGER,         /* POCSAG and FLEX                         */
    LS_RSEL_ACARS,
    LS_RSEL_P25,
    LS_RSEL_ADSB,
    LS_RSEL_SUBGHZ_READ,   /* SUB-GHZ READ: the WATCH receiver        */
    LS_RSEL_SUBGHZ_SWEEP,  /* SUB-GHZ ANALYZER                        */
    LS_RSEL_REC,           /* REC's recording source                  */
    LS_RSEL_WATERFALL,     /* FALLS                                   */
    LS_RSEL_LABS,
    LS_RSEL_JOURNAL,       /* the radio a note attaches               */
    LS_RSEL_FIND,          /* COMPASS FIND, first slot                */
    LS_RSEL_FIND2,         /* COMPASS FIND, second slot               */
    LS_RSEL_CELL,
    LS_RSEL_MIXRF,         /* which keyboard-board radio MIX-RF shows */
    LS_RSEL_EXPERIMENT,    /* EXPERIMENTS: the LoRa chip              */
    LS_RSEL_JOBS
} ls_rsel_job_t;

/* The stored form: one 64-bit word, four bits a job, 0xF for none. Pure, so
   the bench checks the packing settings.c keeps. */
#define LS_RSEL_PACKED_NONE UINT64_MAX
int      ls_rsel_unpack(uint64_t packed, int job);              /* -1: none */
uint64_t ls_rsel_pack(uint64_t packed, int job, int radio);     /* -1 clears */

/* The radio saved for a job, or LS_RSEL_NONE. RAM only: safe from any task,
   including one whose stack is in PSRAM. */
ls_rsel_radio_t ls_rsel_saved(ls_rsel_job_t job);

/* The IQ endpoint a receiver doing `job` should ask ls_radio_acquire for:
   the saved SDR when it is plugged in, otherwise the RTL-SDR and then the
   HackRF - the same order the picker falls back in. NULL when neither is
   attached, so the acquire takes whatever matches. */
const char *ls_rsel_sdr_endpoint(ls_rsel_job_t job);

#ifdef __cplusplus
}
#endif

#endif
