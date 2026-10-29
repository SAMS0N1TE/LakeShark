/* ISM: rtl_433's device decoders (lr433_dec.c) for every sub-GHz receiver,
   not only the LR2021's packet engines.

   Three pieces, each usable alone:

   - The front end (rec_ism_front_*): an SDR's I/Q in, pulse edges out. The
     envelope is sliced against a tracked floor for OOK; while the envelope
     says a carrier is up, rec_fsk's discriminator slices the frequency for
     FSK. Both edge lists of a burst are handed over when it ends. These are
     the ENV/DISC inputs of the RX head design reduced to EDGES.
   - The decoder (rec_ism_decode_edges): EDGES in, rtl_433 messages out. The
     edges, in signed microseconds as REC and SUB-GHZ WATCH keep them, are
     laid back out as decisions at a fixed rate and given to lr433_decode, the
     same function the LR2021 path calls; ERT meters are read at their chip
     rate after their detector pattern, as the LR2021's ERT session does.
   - The sink (rec_ism_publish): whoever wants decoded devices subscribes a
     callback. A message heard twice within a second through two paths (CATCH
     and WATCH, METERS' tap and WATCH) reaches the subscribers once.

   The front end and the decoder are pure: no radio, no allocation, no
   globals. The caller owns the work space, which belongs in PSRAM. */

#ifndef REC_ISM_H
#define REC_ISM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rec_fsk.h"
#include "../../../apps/tui/experiments/lr433_dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- decoder -- */

/* Decisions a second the edges are laid out at: 10 us, several to the
   shortest pulse any of the decoders takes (ERT chips are read at their own
   32768). A gap longer than REC_ISM_GAP_CLAMP_US is cut to it: every
   decoder's reset is shorter, and it keeps a burst with long pauses inside
   the buffer. */
#define REC_ISM_RATE         100000u
#define REC_ISM_ERT_RATE     32768u
#define REC_ISM_GAP_CLAMP_US 30000u
#define REC_ISM_BITS_BYTES   8192u      /* 655 ms at REC_ISM_RATE */

typedef struct {
    lr433_work_t lr;
    uint8_t bits[REC_ISM_BITS_BYTES];
    uint8_t ert[256];                   /* the chips after an ERT detector */
} rec_ism_work_t;

/* Decode one burst's edges: positive microseconds for carrier (or the upper
   tone), negative for its absence. `fsk` picks the FSK devices, otherwise
   the OOK devices and the ERT meters. Writes up to `max` distinct messages
   and returns how many. */
int rec_ism_decode_edges(const int32_t *edges, int n, bool fsk,
                         rec_ism_work_t *w, lr433_msg_t *out, int max);

/* The message on one line of at most n-1 characters, ASCII:
   "ERT-SCM 12345678 339972", "Acu-Tower 12053 C 26.7C 74%". */
void rec_ism_describe(const lr433_msg_t *m, char *out, size_t n);

/* ---------------------------------------------------------- front end -- */

typedef void (*rec_ism_burst_fn)(void *ctx, const int32_t *ook, int n_ook,
                                 const int32_t *fsk, int n_fsk, int peak);

typedef struct {
    uint32_t fs;            /* input pairs a second                       */
    uint32_t gap_us;        /* quiet that ends a burst                    */
    uint32_t min_us;        /* shorter runs fold into the one before      */
    int min_edges;          /* fewer and the burst is dropped as a blip   */
    /* envelope */
    int32_t floor_acc;
    int env, floor, prev_mag;
    int level;              /* the burst's carrier level, slowly decaying */
    bool carrier;
    /* the two slicers */
    rec_fsk_t fsk;
    bool lvl[2];
    uint32_t run[2];        /* samples at the current level               */
    uint64_t t_samples[2], t_us[2];   /* the burst so far, per list       */
    /* the burst */
    bool active;
    uint32_t quiet;         /* samples of envelope quiet                  */
    int peak;
    int32_t *buf[2];        /* OOK edges, FSK edges                       */
    int n[2], cap;
    rec_ism_burst_fn fn;
    void *ctx;
    uint32_t bursts;        /* handed over so far                         */
} rec_ism_front_t;

/* `ook` and `fsk` each hold `cap` edges; both are the caller's. */
void rec_ism_front_init(rec_ism_front_t *f, uint32_t fs, int32_t *ook, int32_t *fsk,
                        int cap, rec_ism_burst_fn fn, void *ctx);
/* Unsigned 8-bit interleaved I/Q, as the RTL-SDR and REC deliver it. */
void rec_ism_front_u8(rec_ism_front_t *f, const uint8_t *iq, size_t bytes);
/* Forget a burst in progress: the stream jumped (retune, dropped data). */
void rec_ism_front_reset(rec_ism_front_t *f);

/* --------------------------------------------------------------- sink -- */

typedef struct {
    lr433_msg_t m;
    uint32_t hz;            /* where it was heard, 0 when not known        */
    float rssi;             /* dBm, NAN when not known                     */
    uint8_t source;         /* rec_source_t                                */
} rec_ism_msg_t;

typedef void (*rec_ism_sink_fn)(const rec_ism_msg_t *msg, void *ctx);

#define REC_ISM_SINKS 4
/* Register once, at start. Sinks run on the publishing task (a receive
   worker), so they copy and return. False when the table is full. */
bool rec_ism_subscribe(rec_ism_sink_fn fn, void *ctx);
void rec_ism_unsubscribe(rec_ism_sink_fn fn, void *ctx);
/* `now_us` is the caller's clock, for the duplicate window. */
void rec_ism_publish(const rec_ism_msg_t *msg, int64_t now_us);
/* Messages published and duplicates dropped, for the console. */
void rec_ism_counts(uint32_t *published, uint32_t *duplicates);

/* ------------------------------------------- on REC's SDR stream (rx) -- */

#define REC_ISM_PLAN_MAX 16
/* Run the front end on REC's SDR stream (rec_set_iq_tap) tuned to hz[0],
   stepping through hz[0..n) every dwell_ms when n > 1, and publish what
   decodes. n == 0 stops and gives REC back the frequency it had. REC must be
   the running receiver; the decode runs on rec_rx. rec_ism_rx.c, device only. */
bool rec_ism_listen(const uint32_t *hz, int n, uint32_t dwell_ms);
typedef struct {
    bool on;
    uint32_t hz, bursts, decoded;
    int plan_n, plan_at;
} rec_ism_listen_t;
void rec_ism_listen_status(rec_ism_listen_t *out);

#ifdef __cplusplus
}
#endif

#endif /* REC_ISM_H */
