/* FREQS on the desktop, the part with no screen in it: the table's columns
   and their order, the star, duplicates, an entry typed field by field and
   checked, whole banks (new, rename, to the trash), copying and moving
   between banks, and CHIRP's CSV going out. Everything goes through the
   store in ls_freqs.c, so all of it runs on the bench. The window is
   screens/ext/scr_freqs_desk.c. */
#ifndef LS_FREQS_DESK_H
#define LS_FREQS_DESK_H

#include "ls_freqs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- columns -- */

enum { LS_FC_STAR, LS_FC_FREQ, LS_FC_NAME, LS_FC_MODE, LS_FC_BAND, LS_FC_GROUP, LS_FC_TONE,
       LS_FC_DIST, LS_FC_BANK, LS_FC_TAGS, LS_FC_NOTES, LS_FC__COUNT };
#define LS_FC_LISTED (-1)       /* the order of the files */

const char *ls_freqd_col_name(int col);
/* The band an entry is in, LS_FB_HF..LS_FB_L, or LS_FB_ALL for none of them. */
int  ls_freqd_band(uint32_t hz);
/* `idx` (loaded entries) put in the order of one column, ties left as they
   were. Distance wants a fix; without one, and for an entry with no place,
   it sorts last. */
void ls_freqd_sort(uint16_t *idx, int n, int col, bool desc, bool fix, double lat, double lon);

/* The star is the tag "fav", so it is in the CSV and travels with it. */
bool ls_freqd_starred(const ls_freq_t *e);
void ls_freqd_set_star(ls_freq_t *e, bool on);

/* Entries of `idx` that are the same channel as another one of them: one
   frequency and mode in one place (within about a kilometre; no place
   matches no place). Sets their bit (by loaded index) in `bits`, cleared
   first; answers how many. */
int  ls_freqd_dups(const uint16_t *idx, int n, uint8_t *bits, size_t nbytes);

/* -------------------------------------------------------------- fields -- */

enum { LS_FF_FREQ, LS_FF_NAME, LS_FF_MODE, LS_FF_BW, LS_FF_GROUP, LS_FF_TONE, LS_FF_TAGS,
       LS_FF_NOTES, LS_FF_LAT, LS_FF_LON, LS_FF_RADIUS, LS_FF__COUNT };
#define LS_FF_TEXT 56           /* room for the longest field and its NUL */

const char *ls_freqd_field_label(int f);
const char *ls_freqd_field_hint(int f);
int  ls_freqd_field_max(int f);                 /* characters it holds */
void ls_freqd_field_get(const ls_freq_t *e, int f, char *out, size_t n);
/* What was typed into a field, checked. False leaves `e` alone and says
   why in a sentence. */
bool ls_freqd_field_set(ls_freq_t *e, int f, const char *text, char *why, size_t wn);
/* An entry with nothing set, the way SAVE makes one. */
void ls_freqd_blank(ls_freq_t *e);

/* --------------------------------------------------------------- banks -- */

/* A bank's name as a file can carry it: lower case, letters, digits, - _.
   False (and why) for a name with nothing usable in it. */
bool ls_freqd_bank_name(const char *typed, char *out, size_t n, char *why, size_t wn);
bool ls_freqd_bank_new(const char *typed, char *why, size_t wn);
bool ls_freqd_bank_rename(int bank, const char *typed, char *why, size_t wn);
/* Not deleted: moved to freqs/trash, where a hand can take it back. */
bool ls_freqd_bank_trash(int bank, char *why, size_t wn);

/* Loaded entries `idx` into `bank` (made if it is not there). Ones already
   in that bank are left out, and with `skip_dups` so are ones the bank has
   already (the same channel, as above). Answers how many went. */
int  ls_freqd_copy(const uint16_t *idx, int n, const char *bank, bool skip_dups, char *why, size_t wn);
/* The same, and out of the files they were in. Built-in entries are copied
   and stay. A note longer than a screen line is cut by a move: an entry
   travels as FREQS holds it, not as its line was. */
int  ls_freqd_move(const uint16_t *idx, int n, const char *bank, char *why, size_t wn);

/* `idx` as a CSV CHIRP opens: FM, NFM and AM memories with tone, DTCS and
   the comment. Other modes are left out and counted in the sentence. */
bool ls_freqd_export_chirp(const uint16_t *idx, int n, const char *path, char *why, size_t wn);

#ifdef __cplusplus
}
#endif

#endif /* LS_FREQS_DESK_H */
