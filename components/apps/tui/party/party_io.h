/* What PARTY needs from the board, in one place: the clock, a position, the
   battery, the vibration motor, the card, memory, a lock and a worker. Banners go out through the
   screen's ls_notify probe (party_notice_pop), on the UI task.
   party_io_board.c is the board's; bench/tools/lssim_party.c is the
   simulator's (three members walking about a town). */
#ifndef PARTY_IO_H
#define PARTY_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "party_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool fix;                  /* a live position */
    double lat, lon;
    float acc_m;               /* <= 0 unknown */
    float speed_mps;           /* < 0 unknown */
    float course_deg;          /* < 0 unknown */
    float heading_deg;         /* compass, < 0 none */
} party_fix_t;

uint32_t party_io_ms(void);
bool party_io_fix(party_fix_t *f);
int  party_io_batt(void);                  /* percent, -1 unknown */
bool party_io_vibe(int ms, int strength);  /* false: no motor, or switched off */
bool party_io_vibe_present(void);
int  party_io_local_hour(void);            /* 0..23, -1 when the clock is not set */
const char *party_io_dir(void);            /* "/sdcard/party" */
bool party_io_card(void);
void *party_io_alloc(size_t bytes);        /* zeroed, PSRAM on the board */
void party_io_lock(void);
void party_io_unlock(void);
/* Run fn on a worker, sleeping the ms it returns, until it returns 0. */
bool party_io_spawn(int (*fn)(void));
bool party_io_mkdir(const char *path);
bool party_io_worker_alive(void);
const party_link_ops_t *party_io_link(void);
uint32_t party_io_random(void);
#ifdef __cplusplus
}
#endif
#endif
