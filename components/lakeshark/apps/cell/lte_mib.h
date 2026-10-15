/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef LS_LTE_MIB_H
#define LS_LTE_MIB_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "lte_sync.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Normal-CP FDD PBCH at 1.92 MS/s. Bandwidth and frame configuration only;
 * neither network identity nor evidence that a transmitter is legitimate. */
typedef struct {
    int n_rb, antenna_ports, phich_duration, phich_resource;
    int sfn, frames, first_frame;
    uint32_t payload;
} lte_mib_result_t;
size_t lte_mib_workspace_size(void);
/* Requires two separate CRC-valid PBCH occasions with matching configuration
 * and progressing SFNs. first_frame indexes this capture's 10 ms boundaries.
 * Returns no partial result on failure or cancellation. */
bool lte_mib_find(const uint8_t *iq, size_t samples, int pci, int frame_start,
                  int cfo_hz, void *workspace, lte_mib_result_t *out,
                  lte_sync_yield_fn yield, void *arg);
/* Same acceptance checks as find, returning after two consistent CRC-valid
 * occasions. frames is the number actually checked, not an extrapolated total.
 * Use find for exhaustive replay/reference measurement. */
bool lte_mib_confirm(const uint8_t *iq, size_t samples, int pci, int frame_start,
                     int cfo_hz, void *workspace, lte_mib_result_t *out,
                     lte_sync_yield_fn yield, void *arg);
/* RSRP-like level: mean power of the port 0 cell reference signals in the
 * central six resource blocks (slot 1, symbols 0 and 4) over every whole
 * frame in the capture, per resource element, in dB relative to a full-scale
 * complex tone. Uncalibrated and at the receiver's gain: use it to compare
 * cells heard with one setup, not as dBm. Needs the PCI and frame boundary
 * from lte_sync. False when the capture holds no whole frame. */
bool lte_mib_crs_power(const uint8_t *iq, size_t samples, int pci, int frame_start,
                       int cfo_hz, void *workspace, float *dbfs);
#ifdef __cplusplus
}
#endif
#endif
