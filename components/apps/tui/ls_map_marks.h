/* Things placed on the map by hand: markers, and lines drawn point by
   point. Kept in RAM, written to the card as plain text so they survive a
   restart and can be read or edited on a computer. Live state is UI-owned;
   deferred file I/O uses worker-owned PSRAM snapshots. */

#ifndef LS_MAP_MARKS_H
#define LS_MAP_MARKS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LS_MARKS_MAX      48
#define LS_MARK_NAME      24
#define LS_SKETCH_MAX     12
#define LS_SKETCH_PTS     64

/* The symbols a marker can carry, each one printable character so a
   marker reads the same in every map style and in a recording. */
typedef enum {
    LS_MARK_STAR = 0,   /* '*' a place          */
    LS_MARK_FLAG,       /* 'F' a meeting point  */
    LS_MARK_HAZARD,     /* '!' a hazard         */
    LS_MARK_TARGET,     /* 'X' a target         */
    LS_MARK_CAMP,       /* 'A' a camp           */
    LS_MARK_WATER,      /* 'W' water            */
    LS_MARK_CAR,        /* 'P' a vehicle        */
    LS_MARK_AID,        /* '+' first aid        */
    LS_MARK__COUNT
} ls_mark_icon_t;

char        ls_mark_icon_char(int icon);
const char *ls_mark_icon_name(int icon);

typedef struct {
    double  lat, lon;
    uint8_t icon;
    char    name[LS_MARK_NAME];
} ls_mark_t;

typedef struct {
    int     n;
    float   lat[LS_SKETCH_PTS], lon[LS_SKETCH_PTS];
    uint8_t hue;               /* TUI colour, without the bright bit */
    char    name[LS_MARK_NAME];
} ls_sketch_t;

/* Where they live. The simulator and the host tests point it elsewhere. */
void ls_marks_use_file(const char *path);
const char *ls_marks_file(void);

/* Read the file once; later calls do nothing until ls_marks_reload. */
void ls_marks_load(void);
/* Enable before UI starts; service from the SD worker. Saves become queued. */
void ls_marks_defer_io(void);
void ls_marks_io_step(void);
void ls_marks_reload(void);
/* Write everything (queue a snapshot when deferred I/O is enabled).
   Deferred errors are available from ls_marks_error after the worker runs.
   False when a synchronous write fails or a snapshot cannot be queued;
   what is in RAM is unchanged either way. */
bool ls_marks_save(void);
/* Why the last load or save did not happen, or NULL. */
const char *ls_marks_error(void);

int              ls_marks_count(void);
const ls_mark_t *ls_marks_at(int i);
/* The index of the new marker, or -1 when full. Named "MARK n" when
   `name` is NULL or empty. */
int  ls_marks_add(double lat, double lon, int icon, const char *name);
bool ls_marks_rename(int i, const char *name);
bool ls_marks_set_icon(int i, int icon);
bool ls_marks_delete(int i);

int                ls_sketch_count(void);
const ls_sketch_t *ls_sketch_at(int i);
bool ls_sketch_delete(int i);
bool ls_sketch_rename(int i, const char *name);

/* The line being drawn, which is not in the list until it is finished. */
void               ls_sketch_begin(void);
bool               ls_sketch_drawing(void);
bool               ls_sketch_add_point(double lat, double lon);
bool               ls_sketch_undo(void);
const ls_sketch_t *ls_sketch_open(void);
/* Keeps a line of two or more points and returns its index, -1 otherwise.
   Either way drawing stops. */
int                ls_sketch_finish(void);
void               ls_sketch_cancel(void);

/* Great circle length of a line, in metres. */
double ls_sketch_length_m(const ls_sketch_t *s);

#ifdef __cplusplus
}
#endif

#endif /* LS_MAP_MARKS_H */
