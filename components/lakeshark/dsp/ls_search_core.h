#ifndef LS_SEARCH_CORE_H
#define LS_SEARCH_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ls_sweep_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Repeating wideband search: the part that decides what is a signal.

   Takes one stitched sweep (the int8 dBFS vector ls_sweep_fold builds) per
   pass, keeps a noise floor for EVERY bin across passes, and turns bins that
   stand well above it into a small table of hits. Pure arithmetic - the radio
   loop is ls_search.c. */

/* ------------------------------------------------------------------ plan */

/* Just over the 4687.5 Hz an FFT bin is at 2.4 MS/s, so every output bin is
   reached by at least one FFT bin. */
#define LS_SEARCH_BIN_HZ        5000u
#define LS_SEARCH_RATE_HZ       2400000u
#define LS_SEARCH_FFT_N         512      /* SPEC_FFT_N */

/* Only the central 75% of each tune is trusted (+-900 kHz at 2.4 MS/s): past
   that is the analogue and decimation roll-off. */
#define LS_SEARCH_USABLE_PCT    75

/* The sweep abandons 40 kHz around each centre for the DC spike
   (LS_SWEEP_DC_GUARD_HZ); 9 FFT bins, 42 kHz, is that rounded up to whole
   bins and is masked on BOTH sides of the centre here. */
#define LS_SEARCH_DC_BINS       9

/* Tune spacing. Each tune supplies the +-375 kHz about its centre, which is
   well inside the trusted +-900, and the centre's own hole is read from the
   neighbour a hop away (about 710-800 kHz, still inside it). 51 tunes for
   136-174 MHz, against 45 for the classic single-sideband plan. */
#define LS_SEARCH_HOP_HZ        750000u

/* The overlapped plan for [lo_hz, hi_hz). False for a range that cannot be
   planned. */
bool ls_search_plan(uint64_t lo_hz, uint64_t hi_hz, ls_sweep_plan_t *out);

/* ------------------------------------------------------------- thresholds */

/* A bin is over when it reads this far above its own floor. Averaged-FFT
   noise wanders about 1.5 dB, so nine is six sigma: no bin of dead air gets
   there by chance, and a carrier that is merely audible clears it easily. */
#define LS_SEARCH_HIT_DB        9.0f

/* A bin this far over its floor on ONE look is a hit at once. A bin is seen
   for about 2 ms once a pass and a pass is seconds, so a voice transmission
   is usually a single look and the 2-look rule would miss it. ls_sweep
   averages 8 FFT frames per tune, which makes a noise bin's power gamma
   distributed (16 degrees of freedom): 1.6 dB sd, and the chance of a bin
   reading 11 dB over the mean (12 less 1 dB for the floor's own bias and
   jitter) is 4e-34, about 6e-30 false hits per 7600-bin pass. The tail alone
   would allow 8 dB (2e-6 a pass); 12 keeps the rest as margin for what is
   not Gaussian - impulse noise, spurs - and stays clear of HIT_DB. */
#define LS_SEARCH_STRONG_DB     12.0f

/* Consecutive passes a bin must be over before it counts. One look is a
   click, a spur or somebody's switching supply; two is a signal. */
#define LS_SEARCH_LOOKS         2

/* Warm-up. A bin makes no hit, by either rule, until its floor has this many
   looks behind it: 20 passes, about 100 s over 136-174 MHz. Until then every
   bin's floor is a running mean of EVERYTHING it reads, signal or not, so a
   spur or a carrier that is there from the start becomes the baseline rather
   than a hit, and the floor of an ordinary noise bin has stopped moving by
   more than a few tenths of a dB. (On the board a 4-look warm-up let about 85
   constant spurs through at passes 5 and 6.) Nothing is made during warm-up,
   so there is nothing made then to forget afterwards. */
#define LS_SEARCH_WARMUP        20

/* A bin over HIT_DB in this share of its last LS_SEARCH_HISTORY looks is
   "constant": a spur or a carrier that is simply always there. It is listed
   once and not announced again, and a table full of them never turns over. */
#define LS_SEARCH_HISTORY       8
#define LS_SEARCH_CONST_PCT     80

/* Fixed hit table. When it is full a new signal replaces the one seen the
   fewest times, constants last; a constant newcomer never replaces anything.
   About 100 constant bins have been seen across 136-174 MHz on the board. */
#define LS_SEARCH_MAX_HITS      128

/* Hit frequencies are snapped to the channel raster, which is what the
   scanner and the presets work in. 2.5 kHz divides both the 12.5 kHz and the
   7.5 kHz VHF rasters. */
#define LS_SEARCH_RASTER_HZ     2500u

/* Hits closer than this are one hit: a cluster nearer than this to a known
   hit is that hit, and bins this close are one cluster (a quiet bin between
   two over ones does not split them). A wide signal such as POCSAG shows as
   two or three bins with a dip between; the centroid of a narrow one jitters
   by a bin from pass to pass. Strictly closer: 12.5 kHz channels stay two. */
#define LS_SEARCH_MATCH_HZ      12500u

/* Floor follower. 1/16 per pass for a bin that is not over: slow enough that
   noise does not move it, fast enough to follow a drifting floor
   (temperature, a neighbour's switcher). Until it has 14 looks the step is
   1/(looks+2), a running mean with the block median as its first sample. */
#define LS_SEARCH_FLOOR_ALPHA   0.0625f

/* After warm-up, a bin that IS over still pulls its floor up, slowly, so a spur or a
   permanent carrier is eventually learned and a floor that was wrong by a
   lot recovers. 1/128: a barely-over signal must outlast the looks it takes
   to be classified constant (7 of 8 over) before it is absorbed, and 1/64
   would end a +10 dB one in about 7 passes. */
#define LS_SEARCH_FLOOR_ALPHA_OVER (1.0f / 128.0f)

/* The first pass has nothing to follow, so each block of this many bins
   starts at its median: a carrier is narrow against it and cannot drag the
   starting floor up under itself. */
#define LS_SEARCH_INIT_BLOCK    64

/* A known hit that comes back after more than this many silent passes is
   reported again (kind LS_SEARCH_HIT_AGAIN); a flicker is not. */
#define LS_SEARCH_AGAIN_GAP     5

/* Which rule made a hit. */
typedef enum {
    LS_SEARCH_PATH_STEADY = 0,   /* over HIT_DB on LS_SEARCH_LOOKS passes */
    LS_SEARCH_PATH_STRONG,       /* over STRONG_DB on one pass */
} ls_search_path_t;

typedef struct {
    uint32_t freq_hz;      /* snapped to LS_SEARCH_RASTER_HZ */
    ls_search_path_t path; /* the rule that first made it a hit */
    bool     constant;     /* over on most of its recent looks: a spur */
    float    max_db;       /* strongest look, dB over that bin's floor */
    uint32_t count;        /* passes it was confirmed on */
    uint32_t first_s;      /* uptime seconds, first and last confirmed */
    uint32_t last_s;
    uint32_t first_pass;
    uint32_t last_pass;
} ls_search_hit_t;

/* floor (4 bytes) + run + history + looks, per bin. */
#define LS_SEARCH_BYTES_PER_BIN 7

typedef struct {
    uint64_t start_hz;
    uint32_t bin_hz;
    uint32_t n_bins;
    float   *floor;        /* dBFS per bin, carved from the caller's memory */
    uint8_t *run;          /* consecutive passes over; bit 7: over STRONG_DB now */
    uint8_t *hist;         /* one bit per look, newest in bit 0: over HIT_DB */
    uint8_t *looks;        /* looks the floor has behind it, saturating */
    bool     primed;
    uint32_t pass;
    uint32_t n_hits;
    uint32_t dropped;      /* constant newcomers the full table had no room for */
    ls_search_hit_t hit[LS_SEARCH_MAX_HITS];
} ls_search_t;

typedef enum {
    LS_SEARCH_HIT_NEW = 0,
    LS_SEARCH_HIT_AGAIN,
} ls_search_hit_kind_t;

/* Called as it happens, with the hit's state after this pass. */
typedef void (*ls_search_hit_fn)(const ls_search_hit_t *hit,
                                 ls_search_hit_kind_t kind, void *user);

/* `mem` is n_bins * LS_SEARCH_BYTES_PER_BIN bytes, 4-aligned, and belongs to
   the caller. Bins are bin_hz wide starting at start_hz, the geometry of the
   ls_sweep plan the vectors come from. */
void ls_search_init(ls_search_t *s, uint64_t start_hz, uint32_t bin_hz,
                    uint32_t n_bins, void *mem);

/* Forget every floor and every hit; the next pass starts the floors over. */
void ls_search_reset(ls_search_t *s);

/* Fold in one pass. A run of touching bins that are over HIT_DB is one
   signal; it is a hit when a bin of it is over STRONG_DB, or has been over
   HIT_DB for LS_SEARCH_LOOKS passes - and every bin of it is past the
   warm-up. `dbfs` holds n_bins values, LS_SWEEP_NO_DATA where nothing was
   measured. `now_s` is uptime in seconds. Returns how many NEW hits this
   pass added. */
uint32_t ls_search_feed(ls_search_t *s, const int8_t *dbfs, uint32_t now_s,
                        ls_search_hit_fn fn, void *user);

/* "steady" or "strong". */
const char *ls_search_path_name(ls_search_path_t path);

/* Nearest multiple of LS_SEARCH_RASTER_HZ. */
uint64_t ls_search_snap(uint64_t hz);

/* Fraction of the passes since it was first confirmed that it was confirmed
   on, 0..1. */
float ls_search_duty(const ls_search_t *s, const ls_search_hit_t *h);

/* How many hits are constant. */
uint32_t ls_search_n_constant(const ls_search_t *s);

/* Indexes into s->hit of up to `max` hits: the ones that are not constant
   first, then most-seen first (ties: strongest first). Returns how many. */
uint32_t ls_search_top(const ls_search_t *s, uint32_t *idx, uint32_t max);

/* One dump line for bins [first, first+count): the current floor, the last
   look and how many of the last 8 looks each bin was over, for analysis off
   the board.

     SEARCH-DUMP D <pass> <hz of the first bin's centre> <step hz> <count> F<hex> L<hex> H<digits>

   F and L are two hex digits per bin, (dB + 100) * 2 clamped to 0..255, so
   00 is "no reading" or -100 dBFS and under and FF is +27.5. H is one digit
   per bin, 0-8. `last` is the n_bins vector of the last pass. Every line
   starts SEARCH-DUMP so one filter takes the header, the lines and the end.
   Returns the length, 0 if it does not fit in `cap` or the arguments are
   bad. */
size_t ls_search_dump_line(const ls_search_t *s, const int8_t *last,
                           uint32_t first, uint32_t count, char *out,
                           size_t cap);

/* The whole dump, a line at a time: a tune's worth of bins per line, so one
   line per tune. `cursor` starts at 0; each call writes the next line and
   moves it on, and returns 0 when there is no more (or `cap` is too small:
   ls_search_dump_cap says what is enough). */
uint32_t ls_search_dump_per_line(const ls_sweep_plan_t *plan);
size_t   ls_search_dump_cap(uint32_t per_line);
size_t   ls_search_dump_next(const ls_search_t *s, const int8_t *last,
                             uint32_t per_line, uint32_t *cursor, char *out,
                             size_t cap);

#ifdef __cplusplus
}
#endif

#endif
