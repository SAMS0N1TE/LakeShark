/* JOURNAL on the desk: the entry table's rows. The screen fills a row from
   each entry the field worker holds; this sorts them by a column, matches
   typed text and wraps an entry's note for the reading pane. No drawing and
   no worker in here, so the bench checks it with rows of its own. */
#ifndef LS_JOURNAL_DESK_H
#define LS_JOURNAL_DESK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { LS_JC_TITLE, LS_JC_WHEN, LS_JC_SOURCE, LS_JC_FREQ, LS_JC_RSSI, LS_JC_PLACE, LS_JC_SIZE, LS_JC_KEPT, LS_JC__COUNT };

typedef struct {
    uint16_t index;             /* where the worker holds it; 0 is the newest */
    uint32_t id;
    char title[48];
    char when[24];              /* as the screen prints it */
    int64_t time_us;            /* for the order when there was no clock */
    bool clock;                 /* `when` is a UTC stamp, which sorts as text */
    char source[12];
    bool gps, imu, signal, saved;
    double lat, lon;
    uint32_t freq;              /* Hz, 0 for none */
    float rssi;
    uint16_t chars;             /* the note's length */
} ls_jrnd_row_t;

/* order[0..n) becomes the rows' indices sorted by `col`, ascending unless
   `desc`. LS_JC_WHEN ascending is the newest first. A row with nothing in
   that column goes last either way. */
void ls_jrnd_sort(uint8_t *order, const ls_jrnd_row_t *rows, int n, int col, bool desc);
void ls_jrnd_cell(const ls_jrnd_row_t *r, int col, char *out, size_t n);
/* Case-blind: does `text` hold `needle`? An empty needle is held by all. */
bool ls_jrnd_holds(const char *text, const char *needle);
/* The next line of `text` from `off`: at most `width` characters, broken at
   a space when there is one, ended by a newline. Copies it to `line` and
   answers the offset of the line after; text[off] == 0 when done. */
int ls_jrnd_wrap(const char *text, int off, int width, char *line, size_t n);
/* How many lines that makes. */
int ls_jrnd_lines(const char *text, int width);

#ifdef __cplusplus
}
#endif

#endif /* LS_JOURNAL_DESK_H */
