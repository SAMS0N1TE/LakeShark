
#ifndef FLEX_H
#define FLEX_H

#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FLEX rate/level modes.  Numeric values are the contract with the fixture -
   see bench/fixtures/flex_gen.h::flex_mode_t. */
typedef enum {
    LS_FLEX_MODE_1600_2 = 0,
    LS_FLEX_MODE_3200_2 = 1,
    LS_FLEX_MODE_3200_4 = 2,
    LS_FLEX_MODE_6400_4 = 3,
} ls_flex_mode_t;

typedef struct flex_ctx flex_ctx_t;

flex_ctx_t *flex_create(fm_state_t *out, ls_flex_mode_t mode);
void        flex_destroy(flex_ctx_t *c);
void        flex_set_mode(flex_ctx_t *c, ls_flex_mode_t mode);
void        flex_reset(flex_ctx_t *c);

void        flex_process(flex_ctx_t *c, const float *demod, int n);

bool           flex_synced       (const flex_ctx_t *c);
ls_flex_mode_t flex_detected_mode(const flex_ctx_t *c);
uint32_t       flex_n_frames     (const flex_ctx_t *c);
uint32_t       flex_n_pages      (const flex_ctx_t *c);
uint32_t       flex_n_cwerr      (const flex_ctx_t *c);
int            flex_near_min     (const flex_ctx_t *c);

#ifdef __cplusplus
}
#endif

#endif
