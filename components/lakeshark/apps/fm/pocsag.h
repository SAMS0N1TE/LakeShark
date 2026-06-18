
#ifndef POCSAG_H
#define POCSAG_H

#include "fm_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pocsag_ctx pocsag_ctx_t;

pocsag_ctx_t *pocsag_create(fm_state_t *out, int baud);
void          pocsag_destroy(pocsag_ctx_t *c);
void          pocsag_set_baud(pocsag_ctx_t *c, int baud);
void          pocsag_reset(pocsag_ctx_t *c);

void          pocsag_process(pocsag_ctx_t *c, const float *demod, int n);

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
