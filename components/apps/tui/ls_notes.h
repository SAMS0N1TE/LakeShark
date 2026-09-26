/* Field notes: one Markdown file per note under /sdcard/notes, readable on
   any computer. The first "# " line is the title. Data dropped in from the
   radios and sensors are "> TAG ..." lines, so the list can say what a note
   holds without a database, and a person reading the file sees plain text.

   The UI never touches the card for a write or a scan: those are queued and
   run on the field module's I/O worker. Loading one note is a direct read,
   because the editor cannot open without it. */

#ifndef LS_NOTES_H
#define LS_NOTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_NOTES_MAX       200
#define LS_NOTE_TEXT_MAX   16384
#define LS_NOTE_STEM       40
#define LS_NOTE_TITLE      48
#define LS_NOTE_PNG_MAX_PX (480 * 480)

typedef enum {
    LS_NOTE_HAS_GPS     = 1 << 0,
    LS_NOTE_HAS_MAP     = 1 << 1,
    LS_NOTE_HAS_RADIO   = 1 << 2,
    LS_NOTE_HAS_BEARING = 1 << 3,
    LS_NOTE_HAS_CHECK   = 1 << 4,
    LS_NOTE_HAS_SENSORS = 1 << 5,
} ls_note_badge_t;

typedef struct {
    char stem[LS_NOTE_STEM];     /* file name without ".md" */
    char title[LS_NOTE_TITLE];
    uint32_t bytes, seq;
    uint16_t checks_open, checks_done;
    uint8_t badges;
    char when[20];               /* the note's own "2026-09-25T14:21" line, if any */
    bool has_place;              /* a GPS, MAP or BEARING line gave a position */
    double lat, lon;             /* the first such position */
} ls_note_info_t;

/* Pure: title, badges, checklist counts and first position of a note. */
void ls_notes_summarize(const char *text, ls_note_info_t *info);
/* "> MAP lat, lon  zNN  file" and "> GPS lat, lon ..." lines. */
bool ls_notes_parse_place(const char *line, double *lat, double *lon);
bool ls_notes_parse_map(const char *line, double *lat, double *lon, int *zoom,
                        char *file, size_t file_cap);

/* Starts the store (queues a scan). Safe to call on every NOTES entry. */
bool ls_notes_start(void);
void ls_notes_refresh(void);
bool ls_notes_scanning(void);
int  ls_notes_count(void);
/* Newest first. */
bool ls_notes_at(int index, ls_note_info_t *out);
int  ls_notes_find(const char *stem);
const char *ls_notes_status(void);
uint32_t ls_notes_generation(void);

/* Reads a note into buf. Direct, not queued. */
bool ls_notes_load(const char *stem, char *buf, size_t cap);
/* Queued. The text is copied, so the caller may keep editing. */
bool ls_notes_save(const char *stem, const char *text);
/* Picks a new stem, reserves it in the index and queues the first save. */
bool ls_notes_create(const char *text, char *stem_out, size_t cap);
/* Moves the note, and its pictures, into notes/trash. Nothing is deleted. */
bool ls_notes_trash(const char *stem);
/* A note written by another app: a title line, then the body. */
bool ls_notes_mark(const char *title, const char *body);
/* Saves an RGB565 picture beside the notes as <leaf>.png. */
bool ls_notes_save_png(const char *leaf, const uint16_t *px, int w, int h);
/* A fresh leaf name for a picture that belongs to `stem`. */
void ls_notes_picture_leaf(const char *stem, char *out, size_t cap);
const char *ls_notes_directory(void);
/* Another folder than /sdcard/notes: the simulator points it at fixtures. */
void ls_notes_use_directory(const char *dir);

/* Runs one queued operation. The field module's I/O worker calls it. */
void ls_notes_io_step(void);

#ifdef LS_NOTES_TEST
void ls_notes_test_reset(const char *directory);
#endif

#ifdef __cplusplus
}
#endif
#endif
