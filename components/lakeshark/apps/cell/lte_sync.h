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
/* Every cell that reaches three consistent SSS observations in one capture,
 * strongest first (neighbouring sectors on one carrier differ in N_ID(2) or
 * in timing). Each carries the 10 ms frame boundary in this capture. The PSS
 * search covers fo_centre +- fo_span Hz in 5 kHz steps (fo_span 40000 around 0
 * is the whole oscillator range; the dongle's known error needs far less).
 * Returns whether any was found; nothing partial on cancellation. */
#define LTE_SYNC_MAX_CELLS 4
typedef struct { lte_sync_result_t r; int frame_start; } lte_sync_cell_t;
typedef struct { int count; lte_sync_cell_t cell[LTE_SYNC_MAX_CELLS]; } lte_sync_set_t;
bool lte_sync_find_cells(const uint8_t *iq,size_t samples,void *workspace,lte_sync_set_t *out,
                         lte_sync_yield_fn yield,void *arg,int fo_centre,int fo_span);
#ifdef __cplusplus
}
#endif
#endif
