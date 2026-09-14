/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_LTE_RESAMPLE_H
#define LS_LTE_RESAMPLE_H
#include <stddef.h>
#include <stdint.h>
#include "lte_sync.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Center-channel low-pass conversion for LTE synchronization/PBCH analysis.
 * This deliberately discards the outer channel: it is NOT a SIB/PDSCH path.
 * Input is continuous, interleaved complex U8 at 2-20 MS/s, <= 2M samples.
 * Both filter margins are discarded. No padding, sample repetition or gaps.
 * Workspace and output are caller-owned; cancellation returns zero samples. */
size_t lte_resample_workspace_size(void);
size_t lte_resample_count(size_t input_samples,uint32_t input_rate);
size_t lte_resample_u8(const uint8_t *input,size_t input_samples,uint32_t input_rate,
                       uint8_t *output,size_t output_capacity,void *workspace,
                       lte_sync_yield_fn yield,void *arg);
#ifdef __cplusplus
}
#endif
#endif
