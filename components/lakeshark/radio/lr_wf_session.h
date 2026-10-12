#ifndef LR_WF_SESSION_H
#define LR_WF_SESSION_H
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
/* UI holds the radio until stop returns. Owner confirms each tune by receiving
 * checked blocks; center remains zero until then. start is nonblocking. */
esp_err_t lr_wf_start(uint32_t hz);
/* Single owner invokes consumer before releasing each checked block. Never
 * block here. NULL/0 signals a discontinuity and requires a full DSP reset.
 * Consumer registration is immutable until stop returns. */
typedef void (*lr_wf_consumer_t)(void *, const uint8_t *, size_t, bool spare);
esp_err_t lr_wf_start_consumer(uint32_t hz, lr_wf_consumer_t consume, void *ctx);
typedef struct { uint32_t lost_pairs, gaps; bool done; esp_err_t error; } lr_wf_stats;
void lr_wf_get_stats(lr_wf_stats *out);
void lr_wf_stop_stats(lr_wf_stats *out);
void lr_wf_stop(void);
#endif
