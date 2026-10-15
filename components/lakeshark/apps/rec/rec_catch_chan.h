#ifndef REC_CATCH_CHAN_H
#define REC_CATCH_CHAN_H
/* CATCH channel windows: which bursts inside the RTL slice are worth a look.
   WATCH ALL keeps today's behaviour. WATCH CHOSEN keeps only bursts whose
   frequency falls inside a saved window; the rest are counted and dropped
   before the LR2021 is asked to jump. Pure logic plus one small saved blob,
   so the host tests build it without the radio. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CATCH_CHAN_MAX 8
#define CATCH_CHAN_WIDTH 200000u      /* default window, centre +/- 100 kHz */
#define CATCH_CHAN_MIN_WIDTH 5000u
#define CATCH_CHAN_MAX_WIDTH 2400000u
#define CATCH_CHAN_MIN_HZ 25200000u
#define CATCH_CHAN_MAX_HZ 1764800000u
#define CATCH_SLICE_HZ 2400000u       /* what one RTL slice covers */
#define CATCH_PRESETS 4
#define CATCH_WATCH_BLOB 74u

typedef enum { CATCH_WATCH_ALL, CATCH_WATCH_CHOSEN } catch_watch_mode_t;
/* A slot with centre 0 is unused. */
typedef struct { uint32_t centre, width; } catch_chan_t;
typedef struct { uint32_t mode; catch_chan_t chan[CATCH_CHAN_MAX]; } catch_watch_t;

/* ---- pure ---- */
unsigned catch_watch_count(const catch_watch_t *);
/* CHOSEN with at least one channel. CHOSEN with none watches ALL. */
bool catch_watch_filtering(const catch_watch_t *);
/* Visible explanation when CHOSEN is set but nothing is chosen, else NULL. */
const char *catch_watch_note(const catch_watch_t *);
/* Edges are inside; windows may overlap. Not filtering: everything passes. */
bool catch_watch_allows(const catch_watch_t *, uint32_t hz);
bool catch_watch_valid(const catch_watch_t *);
/* Slot used, -1 for an invalid window, -2 when full. The same centre again
   updates that slot's width rather than adding a twin. */
int catch_watch_add(catch_watch_t *, uint32_t centre, uint32_t width);
bool catch_watch_remove(catch_watch_t *, unsigned slot);
bool catch_chan_in_slice(uint32_t slice_centre, uint32_t hz);
/* Presets (433.92, 315.00, 868.35, 915.00 MHz) inside the slice. */
unsigned catch_watch_presets(uint32_t slice_centre, uint32_t *out, unsigned cap);
/* Column range of a window on a strip of `cols` cells spanning the slice.
   False when the window misses the slice; the ends are clipped to it. */
bool catch_chan_cols(uint32_t slice_centre, const catch_chan_t *, int cols,
                     int *left, int *right);
int catch_slice_col(uint32_t slice_centre, uint32_t hz, int cols);

/* ---- saved form: magic, version, the struct, additive check ---- */
size_t catch_watch_encode(const catch_watch_t *, uint8_t out[CATCH_WATCH_BLOB]);
bool catch_watch_decode(const uint8_t *, size_t, catch_watch_t *);

/* ---- the one live copy, shared by the UI and the RTL worker ---- */
void catch_watch_load(void);          /* once; NVS read, not for the render path */
void catch_watch_get(catch_watch_t *);
bool catch_watch_set(const catch_watch_t *);   /* validates, applies, saves */
bool catch_watch_passes(uint32_t hz);          /* RTL worker: live filter */
#ifndef ESP_PLATFORM
/* Host tests: the saved blob stands in for NVS; "reboot" forgets the live copy. */
void catch_watch_host_reboot(void);
uint8_t *catch_watch_host_blob(size_t *len);
#endif
#ifdef __cplusplus
}
#endif
#endif
