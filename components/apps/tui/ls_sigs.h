/* The signature table: what a heard device is (a plate reader, a body
   camera, a tracker, a car...), from what the radios report: name, address
   prefix, advertising company and service IDs, the kind and the radio.

   One table in the firmware, shared by every app that classifies devices
   (WARDRIVE's HUNT, SWEEP). It is built from three layers, later ones
   replacing earlier rows with the same id:
     1. the built-in rows (the same as data/wardrive/sigs.txt),
     2. rows an app adds in the card format (wd_sig_set_extra: SWEEP folds
        the rows of an old /sdcard/sweep/rules.json in here),
     3. the user's card file, /sdcard/wardrive/sigs.txt (format in
        data/wardrive/sigs.txt), so new targets need no firmware.

   The names keep WARDRIVE's wd_sig_ prefix: the table speaks WARDRIVE's
   tag and kind words (WD_T_*, WD_* in wardrive/wd_core.h, types only), and
   WARDRIVE, its tests and the simulator call it unchanged.

   Thread safety: the table has its own lock. A match walks it under that
   lock; a load builds the new table outside it and swaps it in. A pointer
   from wd_sig_at() stays valid until the second load after it was taken.
   Pure C, no board calls besides the allocator: the bench tests it
   directly. */
#ifndef LS_SIGS_H
#define LS_SIGS_H

#include "wardrive/wd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;             /* short stable id: "flock-cam", "axon" */
    const char *label;          /* what the screen calls it: "FLOCK CAMERA" */
    uint16_t tags;              /* WD_T_* it sets */
    uint8_t kinds;              /* 1 << WD_* it applies to */
    uint8_t alert;              /* 0 log only (a lead), 1 alert, 2 alert, high confidence */
    const char *source;         /* where the pattern comes from, for the HUNT page's detail */
} wd_sig_info_t;

/* What a Bluetooth advert says beyond its name, when the feed reports it.
   0 / -1 / NULL: not reported. Designated or zero initialisers keep the
   later fields off. */
typedef struct {
    uint16_t company;           /* manufacturer data's company ID */
    int16_t mfg0;               /* the first byte after it, -1 none */
    uint16_t uuid16;            /* a 16-bit service UUID (service data's first, else the list's) */
    int16_t svc0;               /* the first byte of that UUID's service data, -1 none */
    const uint8_t *uuid128;     /* n128 128-bit service UUIDs, 16 bytes each, as on air (little endian) */
    uint8_t n128;
    bool no_oui;                /* the address is random or locally administered: oui: rows skip it */
} wd_sig_ble_t;

/* Load the built-ins and the card's file (<dir>/sigs.txt); call again to
   reload the file (only reloads when it changed). The directory is
   WARDRIVE's wd_feed_dir() when that is linked in, else /sdcard/wardrive on
   the board (wd_sig_set_dir changes it). Card IO: not under any lock. */
void wd_sig_init(void);
/* The built-ins at least: loads them when no table exists yet. Cheap after
   the first call; for apps that match before WARDRIVE ever opened. */
void wd_sig_ensure(void);
/* Where wd_sig_init looks for sigs.txt when wd_feed_dir() is not there;
   NULL or "" goes back to the default. */
void wd_sig_set_dir(const char *dir);
/* The matching signature's index + 1, or 0. ORs its tags into *tags. No card
   reads, no allocation: safe under an app's own lock. */
uint16_t wd_sig_match(const wd_obs_t *o, uint16_t *tags);
/* The same with the advert's IDs (NULL: none). */
uint16_t wd_sig_match_ble(const wd_obs_t *o, const wd_sig_ble_t *b, uint16_t *tags);
int wd_sig_count(void);
const wd_sig_info_t *wd_sig_at(int i);

/* The built-in rows, in the card file's format. */
const char *wd_sig_builtin(void);
/* Build the table from the built-ins, the extra layer and `user` (a card
   file's text, NULL none). The number of rows, or -1 with wd_sig_note()
   saying why. */
int wd_sig_load(const char *user, size_t len);
/* Replace the extra layer (rows in the card format an app adds, NULL or 0
   none) and rebuild with the card text last loaded. The number of rows, or
   -1. Bad extra lines are skipped, never noted. `from` names the app for
   wd_sig_note(): "44 signatures, 3 from SWEEP". */
int wd_sig_set_extra(const char *text, size_t len, const char *from);
/* Rows the extra layer holds now. */
int wd_sig_extra_rows(void);
/* The last load in words: "41 signatures, 2 from the card" or the first bad
   line of the card's file. */
const char *wd_sig_note(void);
/* A tag word of the file ("ALPR", "GLASSES") as WD_T_*, 0 unknown. */
uint16_t wd_sig_tag(const char *word, size_t n);
/* Whole-string glob, any case: * ? # (hex digit) % (digit) \ (literal). */
bool wd_sig_glob(const char *pat, const char *s);

#ifdef __cplusplus
}
#endif
#endif
