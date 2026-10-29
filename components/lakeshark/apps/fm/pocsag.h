
#ifndef POCSAG_H
#define POCSAG_H

#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pocsag_ctx pocsag_ctx_t;

/* Each finished page goes to the sink (on the decoding task); a NULL sink
   counts pages without rendering, storing or logging text. */
typedef void (*pocsag_page_fn)(void *user, const fm_page_t *pg);
pocsag_ctx_t *pocsag_create_sink(pocsag_page_fn sink, void *user, int baud);
/* The FM page ring as the sink: pocsag_create(out) is
   pocsag_create_sink(pocsag_page_to_fm, out). */
void          pocsag_page_to_fm(void *fm_state, const fm_page_t *pg);
pocsag_ctx_t *pocsag_create(fm_state_t *out, int baud);
void          pocsag_destroy(pocsag_ctx_t *c);
void          pocsag_set_baud(pocsag_ctx_t *c, int baud);
void          pocsag_reset(pocsag_ctx_t *c);
/* Stream gaps discard alignment and unfinished pages, keeping counters. */
void          pocsag_seam(pocsag_ctx_t *c);
void          pocsag_sample_rate(pocsag_ctx_t *c, int rate);
typedef void (*pocsag_cw_fn)(void *arg, uint32_t cw, int idx, int result);
void          pocsag_observe(pocsag_ctx_t *c, pocsag_cw_fn fn, void *arg);
/* A quiet decoder keeps its pages in `out` and does not publish them onto
   the decoded bus (core/ls_decoded.h): TIME MACHINE's replay publishes its
   own, stamped with when they were on the air. */
void          pocsag_quiet(pocsag_ctx_t *c, bool quiet);

void          pocsag_process(pocsag_ctx_t *c, const float *demod, int n);

/* A 64-byte batch after the hardware has removed its sync word. */
bool pocsag_process_batch(pocsag_ctx_t *c, const uint8_t *data, int len,
                          bool inverted, bool contiguous);

/* Clock-recovered FIFO bytes, MSB first, without the sample timing loop.
   The native stream driver restores the stripped sync; seams keep counters. */
void pocsag_process_bits(pocsag_ctx_t *c, const uint8_t *data, size_t n);

/* One received codeword against its BCH(31,21) and even parity: 0 when it
   is clean, 1 when one flipped bit was put right in *cw, -1 when it is past
   correcting. */
int           pocsag_check_codeword(uint32_t *cw);

bool          pocsag_inverted(const pocsag_ctx_t *c);
bool          pocsag_synced(const pocsag_ctx_t *c);
int           pocsag_baud_of(const pocsag_ctx_t *c);
uint32_t      pocsag_n_frames(const pocsag_ctx_t *c);
uint32_t      pocsag_n_pages(const pocsag_ctx_t *c);
uint32_t      pocsag_n_cwerr(const pocsag_ctx_t *c);
uint32_t      pocsag_n_addr(const pocsag_ctx_t *c);
uint32_t      pocsag_n_msg(const pocsag_ctx_t *c);
int           pocsag_near_min(const pocsag_ctx_t *c);
uint32_t      pocsag_n_near(const pocsag_ctx_t *c);

#ifdef __cplusplus
}
#endif

#endif
