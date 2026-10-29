/* The rest of the pocsag.h surface the RX HEAD POCSAG plugin calls, for
   test_field, whose own fakes cover create/destroy/set_baud/pages/frames/
   batch/bits/seam and count every call. These answer and do nothing. */
#include "apps/fm/pocsag.h"
void pocsag_reset(pocsag_ctx_t *c) { (void)c; }
void pocsag_sample_rate(pocsag_ctx_t *c, int rate) { (void)c; (void)rate; }
void pocsag_process(pocsag_ctx_t *c, const float *d, int n) { (void)c; (void)d; (void)n; }
bool pocsag_synced(const pocsag_ctx_t *c) { (void)c; return false; }
uint32_t pocsag_n_cwerr(const pocsag_ctx_t *c) { (void)c; return 0; }
uint32_t pocsag_n_addr(const pocsag_ctx_t *c) { (void)c; return 0; }
uint32_t pocsag_n_msg(const pocsag_ctx_t *c) { (void)c; return 0; }
int pocsag_near_min(const pocsag_ctx_t *c) { (void)c; return 32; }
uint32_t pocsag_n_near(const pocsag_ctx_t *c) { (void)c; return 0; }
