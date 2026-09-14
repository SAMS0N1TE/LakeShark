/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_LTE_SYNC_H
#define LS_LTE_SYNC_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* LTE FDD normal CP, 1.92 MS/s. PSS/SSS yields PCI, not subscriber identity,
 * PLMN, tracking area, cell global identity, or proof of a simulator. */
typedef struct { int pci,hits,pairs,cfo_hz;float pss_score,sss_score; } lte_sync_result_t;
typedef bool (*lte_sync_yield_fn)(void *arg);
size_t lte_sync_workspace_size(void);
/* First 10 ms frame boundary in the most recently searched IQ, or -1.
 * Valid only after a successful find using this same workspace. */
int lte_sync_frame_start(const void *workspace);
bool lte_sync_find(const uint8_t *iq,size_t samples,void *workspace,
                   lte_sync_result_t *out,lte_sync_yield_fn yield,void *arg);
#ifdef __cplusplus
}
#endif
#endif
