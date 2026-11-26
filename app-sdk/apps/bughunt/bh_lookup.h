/* BUGHUNT's name lookups off the card (bh_ids.c reads the format; this does
   the card reads, one job at a time, and keeps the answers). A name asked for
   the first time comes back NULL and turns up a frame or two later. */
#ifndef BH_LOOKUP_H
#define BH_LOOKUP_H
#include "bh_ids.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BHL_FILE1 "/sdcard/lakeshark/packs/bughunt-ids.bin"   /* from PACKS */
#define BHL_FILE2 "/sdcard/bughunt/ids.bin"                     /* put by hand */

void bhl_open(void);
void bhl_close(void);
void bhl_pump(void);                    /* once a frame */
/* The name, or NULL: not known yet (asked for now) or not in the file. */
const char *bhl_get(int kind, uint32_t key);
/* The maker of a public MAC address (NULL for random ones). */
const char *bhl_maker(const uint8_t mac_be[6]);
/* 0 no file, 1 loading, 2 ready; text for INFO. */
int bhl_state(char *out, size_t cap);
#endif
